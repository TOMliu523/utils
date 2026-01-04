/*
 * ring.h
 *
 * Lock-free ring buffer implementations.
 *
 * This header provides a family of high-performance, lock-free ring queues
 * designed for concurrent systems, including the following variants:
 *
 *  - SPSC : Single Producer / Single Consumer
 *  - SPMC : Single Producer / Multi Consumer
 *  - MPSC : Multi Producer  / Single Consumer
 *  - MPMC : Multi Producer  / Multi Consumer
 *
 * Extended interfaces:
 *  - Zero-copy read interfaces for single-producer rings
 *    (reader obtains spans instead of copying elements)
 *  - Batched enqueue/dequeue operations
 *
 * Design goals:
 *  - Lock-free progress guarantees (no mutexes or blocking primitives)
 *  - Cache-friendly data layout with explicit cache-line alignment
 *  - Minimal false sharing between producers and consumers
 *  - Explicit memory ordering using acquire/release semantics
 *  - Deterministic behavior suitable for latency-critical data paths
 *
 * Usage notes:
 *  - Each ring variant has strict concurrency assumptions.
 *    Violating the producer/consumer model results in undefined behavior.
 *  - Some variants separate reservation and commit phases
 *    (e.g., read_head vs read_tail) to support multi-consumer safety.
 *  - Zero-copy interfaces are only available where correctness
 *    can be guaranteed (typically single-producer designs).
 *
 * Intended use cases:
 *  - Networking fast paths
 *  - Storage and I/O pipelines
 *  - Event loops and task schedulers
 *  - Control-plane / data-plane message passing
 *
 * This header favors simplicity, correctness, and predictability
 * over generic container abstractions.
 */

#ifndef __RING_H__
#define __RING_H__

#include <string.h>
#include <stdbool.h>

#include "type.h"
#include "macro.h"

struct ring_spsc {
    uint32_t cap;
    uint32_t mask;

    /* Monotonic producer position (written only by the producer). */
    ALIGNED(CACHE_LINE) uint32_t writer;
    /* Monotonic consumer position (written only by the consumer). */
    ALIGNED(CACHE_LINE) uint32_t reader;
    /* Flexible array holding cap pointer slots. */
    void *data[];
};

extern struct ring_spsc *ring_spsc_init(int cap);
extern void ring_spsc_fini(struct ring_spsc *ring);

/*
 * ring_spsc_write
 *
 * Enqueue up to 'nums' pointer entries from 'data[]' into the ring.
 *
 * Returns:
 *  - The number of entries actually written (0 if the ring is full).
 *
 * Threading:
 *  - Producer-only API (must not be called concurrently by multiple threads).
 *
 * Ordering:
 *  - Writes ring->data[] first, then RELEASE-stores ring->writer to publish them.
 */
static INLINE int ring_spsc_write(struct ring_spsc *ring, void *data[], uint32_t nums)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    head = ring->writer;
    tail = __atomic_load_n(&ring->reader, __ATOMIC_ACQUIRE);

    real = MIN(nums, ring->cap - (head - tail));
    if (real == 0) {
        return 0;
    }

    idx = head & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&ring->data[idx], &data[0], first * sizeof(void *));
    if (real > first) {
        memcpy(&ring->data[0], &data[first], (real - first) * sizeof(void *));
    }

    __atomic_store_n(&ring->writer, head + real, __ATOMIC_RELEASE);
    return real;
}

/*
 * ring_spsc_read
 *
 * Dequeue up to 'max' pointer entries into 'data[]'.
 *
 * Returns:
 *  - The number of entries actually read (0 if the ring is empty).
 *
 * Threading:
 *  - Consumer-only API.
 *
 * Ordering:
 *  - ACQUIRE-loads ring->writer before reading ring->data[] to ensure visibility.
 *  - RELEASE-stores ring->reader after copying to publish consumed progress.
 */
static INLINE int ring_spsc_read(struct ring_spsc *ring, void *data[], uint32_t max)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    head = __atomic_load_n(&ring->writer, __ATOMIC_ACQUIRE);
    tail = ring->reader;

    real = MIN(max, head - tail);
    if (real == 0) {
        return 0;
    }

    idx = tail & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&data[0], &ring->data[idx], first * sizeof(void *));
    if (real > first) {
        memcpy(&data[first], &ring->data[0], (real - first) * sizeof(void *));
    }

    __atomic_store_n(&ring->reader, tail + real, __ATOMIC_RELEASE);
    return real;
}

/*
 * ring_spsc_read_start
 *
 * Prepares a zero-copy read by returning pointers to the ring's internal storage.
 * It does NOT copy data out and does NOT advance the consumer cursor.
 *
 * Parameters:
 *  - max: upper bound of how many entries the caller intends to consume this round.
 *
 * Semantics:
 *  - Computes the number of currently available entries: (writer - reader).
 *  - Exposes up to 'max' entries as at most two contiguous spans (p1/p2) to
 *    handle wrap-around.
 *  - If the ring is empty, span is filled with NULL pointers and counts = 0.
 *
 * Synchronization:
 *  - ACQUIRE-load of ring->writer ensures that any entries published by the producer
 *    are visible when the consumer dereferences span->p1/p2.
 *
 * IMPORTANT:
 *  - The caller must not access beyond span->count1/count2.
 *  - After consuming, caller MUST call ring_spsc_read_commit().
 */
static INLINE void ring_spsc_read_start(struct ring_spsc *ring, struct ring_span *span, uint32_t max)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    head = __atomic_load_n(&ring->writer, __ATOMIC_ACQUIRE);
    tail = ring->reader;

    real = MIN(max, head - tail);
    if (real == 0) {
        span->p1 = span->p2 = NULL;
        span->count1 = span->count2 = 0;
        span->start = tail;
        span->total = 0;

        return;
    }

    idx = tail & ring->mask;
    first = MIN(real, ring->cap - idx);

    span->p1 = &ring->data[idx];
    span->count1 = first;
    span->p2 = (real - first != 0) ? &ring->data[0] : NULL;
    span->count2 = real - first;
    span->start = tail;
    span->total = real;

    return;
}

/*
 * ring_spsc_read_commit
 *
 * Finalizes a prior ring_spsc_read_start() by advancing the consumer cursor.
 *
 * Semantics:
 *  - Advances ring->reader from span->start to span->start + span->total.
 *  - Uses a RELEASE store so the producer can ACQUIRE-load reader and safely
 *    reuse freed slots.
 *
 * Threading:
 *  - Consumer-only API.
 *
 * Safety note:
 *  - span must come from a matching ring_spsc_read_start() on the same ring.
 *  - Committing a span twice or committing a stale span is undefined behavior.
 */
static INLINE void ring_spsc_read_commit(struct ring_spsc *ring, const struct ring_span *span)
{
    __atomic_store_n(&ring->reader, span->start + span->total, __ATOMIC_RELEASE);
}

/*
 * ring_spmc
 *
 * Lock-free Single-Producer / Multiple-Consumer ring buffer.
 *
 * Use case:
 *  - Exactly ONE writer thread produces elements into the ring.
 *  - Multiple reader threads consume elements concurrently.
 *
 * Design overview:
 *  - The writer owns and exclusively updates the global write cursor `writer`.
 *  - Each reader competes on a shared read cursor pair (`reader.head` / `reader.tail`)
 *    to reserve and publish consumed slots.
 *  - The ring buffer is bounded; capacity must be a power of two.
 *
 * Cursors:
 *  - writer:
 *      Monotonic write position.
 *      Written ONLY by the producer thread.
 *
 *  - reader.head:
 *      Reservation cursor for consumers.
 *      Updated via atomic CAS by all reader threads to claim items.
 *
 *  - reader.tail:
 *      Commit cursor for consumers.
 *      Advanced after the data has been fully consumed, ensuring correct ordering.
 *
 * Memory ordering:
 *  - Producer publishes written data with RELEASE semantics on `writer`.
 *  - Consumers ACQUIRE-load `writer` before reading data to observe writes.
 *  - Consumers RELEASE-store `reader.tail` after finishing consumption to
 *    make slots reusable by the producer.
 *
 * Cache layout:
 *  - `writer` is placed on its own cache line to avoid false sharing with readers.
 *  - `reader.head` and `reader.tail` are grouped together since they are typically
 *    modified by consumer threads.
 *
 * Notes:
 *  - This structure assumes strict SPMC usage. Using multiple writers will break
 *    correctness.
 *  - Readers must follow the reservation → consume → commit protocol.
 *  - For high contention scenarios, consider backoff or pause instructions
 *    during CAS retry loops.
 */
struct ring_cursor {
    uint32_t head;
    uint32_t tail;
} ALIGNED(CACHE_LINE);

struct ring_spmc {
    uint32_t cap;   /* Ring capacity (power of two) */
    uint32_t mask;  /* cap - 1, used for fast index wrap-around */

    ALIGNED(CACHE_LINE) uint32_t writer; /* Producer write cursor */
    struct ring_cursor reader;           /* Shared consumer cursors */

    void *data[]; /* Ring storage */
};

extern struct ring_spmc *ring_spmc_init(int cap);
extern void ring_spmc_fini(struct ring_spmc *ring);

/*
 * Write up to @nums elements into the ring.
 *
 * @ring: ring instance
 * @data: array of pointers to enqueue
 * @nums: number of elements to write
 *
 * Return:
 *  - Number of elements actually written (0 <= ret <= nums)
 *
 * Notes:
 *  - Single-producer only: no CAS required on writer cursor
 *  - Space availability is determined by reader.tail (oldest unconsumed)
 */
static INLINE int ring_spmc_write(struct ring_spmc *ring, void *data[], uint32_t nums)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    /* Load producer cursor (single writer, relaxed is sufficient) */
    head = ring->writer;
    /*
     * Load the global consumer tail with ACQUIRE to ensure that
     * all prior consumer reads are observed before computing free space.
     */
    tail = __atomic_load_n(&ring->reader.tail, __ATOMIC_ACQUIRE);

    /* Compute writable elements (ring capacity minus in-flight elements) */
    real = MIN(nums, ring->cap - (head - tail));
    if (real == 0) {
        return 0;
    }

    /* Write may wrap around ring end */
    idx = head & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&ring->data[idx], &data[0], first * sizeof(void *));
    if (real > first) {
        memcpy(&ring->data[0], &data[first], (real - first) * sizeof(void *));
    }

    /*
     * Publish written elements:
     *  - RELEASE ensures all data writes are visible before writer is advanced
     */
    __atomic_store_n(&ring->writer, head + real, __ATOMIC_RELEASE);
    return real;
}

/*
 * Read up to @max elements from the ring.
 *
 * @ring: ring instance
 * @data: output array to store dequeued pointers
 * @max: maximum number of elements to read
 *
 * Return:
 *  - Number of elements actually read (0 <= ret <= max)
 *
 * Algorithm:
 *  1) Load reader.head (current reservation point)
 *  2) Load writer cursor to determine available elements
 *  3) CAS reader.head to reserve a contiguous range [old_head, old_head + real)
 *  4) Copy data from the reserved range
 *  5) Advance reader.tail in order to commit consumption
 *
 * Correctness:
 *  - reader.head reservation prevents multiple consumers from reading
 *    the same elements
 *  - reader.tail advancement guarantees global consumption order
 */
static INLINE int ring_spmc_read(struct ring_spmc *ring, void *data[], uint32_t max)
{
    uint32_t idx = 0;
    uint32_t real = 0;
    uint32_t first = 0;
    uint32_t start = 0;
    uint32_t reader_head = 0;
    uint32_t writer_head = 0;

    /* Load current reservation cursor */
    reader_head = __atomic_load_n(&ring->reader.head, __ATOMIC_RELAXED);

    /*
     * Reserve a read range using CAS on reader.head.
     * reader_head always represents the OLD value on CAS success.
     */
    do {
        /*
         * Load writer cursor with ACQUIRE to ensure
         * producer data is visible before reading.
         */
        writer_head = __atomic_load_n(&ring->writer, __ATOMIC_ACQUIRE);

        real = MIN(max, writer_head - reader_head);
        if (real == 0) {
            return 0;
        }
    } while (!__atomic_compare_exchange_n(&ring->reader.head,
                                          &reader_head,
                                          reader_head + real,
                                          true, /* weak CAS is sufficient */
                                          __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));

    /*
     * reader_head is the starting index of this reader's reserved range.
     * All data must be read starting from this OLD head.
     */
    idx = reader_head & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&data[0], &ring->data[idx], first * sizeof(void *));
    if (real > first) {
        memcpy(&data[first], &ring->data[0], (real - first) * sizeof(void *));
    }

    /*
     * Commit consumption in order by advancing reader.tail.
     * Only the reader owning the current tail may advance it.
     */
    for (;;) {
        start = reader_head;
        if (__atomic_compare_exchange_n(&ring->reader.tail,
                                        &start,
                                        reader_head + real,
                                        true,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            break;
        }

        /* Busy-wait until prior readers commit */
        __builtin_ia32_pause();
    }

    return real;
}

/*
 * Multi-Producer / Single-Consumer (MPSC) ring buffer structure.
 *
 * Concurrency model:
 *  - Multiple producer threads concurrently enqueue elements
 *  - Exactly one consumer thread dequeues elements
 *
 * Cursor ownership:
 *  - writer.head : shared among all producers, used to reserve slots (CAS)
 *  - writer.tail : shared among all producers, used to publish committed writes
 *  - reader      : owned exclusively by the single consumer
 *
 * Indexing:
 *  - All cursors are monotonic counters
 *  - Actual array index is computed as (cursor & mask)
 *
 * Cache-line layout:
 *  - writer cursors may be frequently contended by producers
 *  - reader cursor is placed on a separate cache line to avoid
 *    false sharing between producers and the consumer
 *
 * Fields:
 *  @cap   Ring capacity (number of elements), must be power of two
 *  @mask  cap - 1, used for fast modulo operation
 *  @writer.head  Producer reservation cursor
 *  @writer.tail  Producer commit cursor
 *  @reader       Consumer read cursor
 *
 * Data storage:
 *  - data[] holds pointers to enqueued elements
 *  - Storage is contiguous and wraps around using mask
 */
struct ring_mpsc {
    uint32_t cap;   /* Ring capacity (power of two) */
    uint32_t mask;  /* cap - 1, used for fast index wrap-around */

    struct ring_cursor writer;          /* Shared producer cursors (head/tail) */
    ALIGNED(CACHE_LINE) uint32_t reader; /* Consumer read cursor (single owner) */

    void *data[]; /* Ring storage */
};

extern struct ring_mpsc *ring_mpsc_init(int cap);
extern void ring_mpsc_fini(struct ring_mpsc *ring);

/*
 * Enqueue up to @nums elements into the ring.
 *
 * @ring: ring instance
 * @data: array of pointers to enqueue
 * @nums: number of elements requested to enqueue
 *
 * Return:
 *  - Number of elements actually enqueued (0 <= ret <= nums)
 *
 * Algorithm (producer side):
 *  1) Load writer.head (current reservation point)
 *  2) Load reader cursor to compute available space
 *  3) CAS writer.head to reserve a contiguous range
 *  4) Copy data into reserved slots
 *  5) Advance writer.tail in order to publish writes
 *
 * Notes:
 *  - Multiple producers may execute this function concurrently
 *  - writer.head reservation guarantees unique slot ownership
 *  - writer.tail ensures FIFO visibility to the consumer
 */
static INLINE int ring_mpsc_write(struct ring_mpsc *ring, void *data[], int nums)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    /* Load current producer reservation cursor */
    head = __atomic_load_n(&ring->writer.head, __ATOMIC_RELAXED);

    /*
     * Reserve space by advancing writer.head.
     * reader cursor is loaded with ACQUIRE to ensure
     * visibility of prior consumer progress.
     */
    do {
        tail = __atomic_load_n(&ring->reader, __ATOMIC_ACQUIRE);

        /* Compute available slots */
        real = MIN(nums, ring->cap - (head - tail));
        if (real == 0) {
            return 0;
        }
    } while (!__atomic_compare_exchange_n(&ring->writer.head,
                                          &head,
                                          head + real,
                                          true,
                                          __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));

    /* Copy data into reserved ring slots (may wrap around) */
    idx = head & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&ring->data[idx], &data[0], first * sizeof(void *));
    if (real > first) {
        memcpy(&ring->data[0], &data[first], (real - first) * sizeof(void *));
    }

    /*
     * Publish writes in FIFO order.
     * Only the producer owning the current writer.tail
     * is allowed to advance it.
     */
    for (;;) {
        uint32_t start = head;

        if (__atomic_compare_exchange_n(&ring->writer.tail,
                                        &start,
                                        start + real,
                                        true,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            break;
        }

        /* Wait until earlier producers commit */
        __builtin_ia32_pause();
    }

    return real;
}

/*
 * Dequeue up to @max elements from the ring.
 *
 * @ring: ring instance
 * @data: output array to receive dequeued pointers
 * @max: maximum number of elements to dequeue
 *
 * Return:
 *  - Number of elements actually dequeued (0 <= ret <= max)
 *
 * Notes:
 *  - Single-consumer only: no CAS required
 *  - Data visibility is guaranteed by acquiring writer.tail
 */
static INLINE int ring_mpsc_read(struct ring_mpsc *ring, void *data[], int max)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    /* Acquire published writes */
    tail = __atomic_load_n(&ring->writer.tail, __ATOMIC_ACQUIRE);
    head = __atomic_load_n(&ring->reader, __ATOMIC_RELAXED);

    real = MIN(max, tail - head);
    if (real == 0) {
        return 0;
    }

    /* Copy out data (may wrap around) */
    idx = head & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&data[0], &ring->data[idx], first * sizeof(void *));
    if (real > first) {
        memcpy(&data[first], &ring->data[0], (real - first) * sizeof(void *));
    }

    /* Commit consumption */
    __atomic_store_n(&ring->reader, head + real, __ATOMIC_RELEASE);
    return real;
}

/*
 * Start a zero-copy dequeue operation.
 *
 * @ring: ring instance
 * @span: output span descriptor
 * @max: maximum number of elements to reserve
 *
 * The span describes up to two contiguous memory regions
 * corresponding to a logical linear read range.
 *
 * Notes:
 *  - Single-consumer only
 *  - The caller must later call ring_mpsc_read_commit()
 *    to finalize consumption
 */
static INLINE void ring_mpsc_read_start(struct ring_mpsc *ring, struct ring_span *span, int max)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    /* Acquire published writes */
    tail = __atomic_load_n(&ring->writer.tail, __ATOMIC_ACQUIRE);
    head = __atomic_load_n(&ring->reader, __ATOMIC_RELAXED);

    real = MIN(max, tail - head);
    if (real == 0) {
        /* Empty span */
        span->p1 = span->p2 = NULL;
        span->count1 = span->count2 = 0;
        span->start = head;
        span->total = 0;

        return;
    }

    /* Describe contiguous regions */
    idx = head & ring->mask;
    first = MIN(real, ring->cap - idx);

    span->p1 = &ring->data[idx];
    span->count1 = first;
    span->p2 = (real - first != 0) ? &ring->data[0] : NULL;
    span->count2 = real - first;
    span->start = head;
    span->total = real;

    return;
}

/*
 * Commit a zero-copy dequeue operation.
 *
 * @ring: ring instance
 * @span: span descriptor previously returned by ring_mpsc_read_start()
 *
 * Notes:
 *  - Must be called exactly once per successful read_start()
 *  - Advances the consumer cursor, making space visible to producers
 */
static INLINE void ring_mpsc_read_commit(struct ring_mpsc *ring, const struct ring_span *span)
{
    __atomic_store_n(&ring->reader, span->start + span->total, __ATOMIC_RELEASE);
}

/*
 * Multi-Producer / Multi-Consumer (MPMC) ring buffer structure.
 *
 * Concurrency model:
 *  - Multiple producer threads concurrently enqueue elements
 *  - Multiple consumer threads concurrently dequeue elements
 *
 * Cursor ownership:
 *  - writer.head : shared among all producers, used to reserve write slots (CAS)
 *  - writer.tail : shared among all producers, used to publish committed writes
 *  - reader.head : shared among all consumers, used to reserve read slots (CAS)
 *  - reader.tail : shared among all consumers, used to commit consumption in order
 *
 * Indexing:
 *  - All cursors are monotonic counters
 *  - Actual array indices are computed as (cursor & mask)
 *
 * Ordering guarantees:
 *  - writer.tail advances in strictly increasing order, ensuring FIFO write order
 *  - reader.tail advances in strictly increasing order, ensuring FIFO read order
 *
 * Cache-line considerations:
 *  - writer and reader cursors are heavily contended in MPMC scenarios
 *  - Implementations may further pad or align cursors to reduce false sharing
 *
 * Fields:
 *  @cap   Ring capacity (number of elements), must be power of two
 *  @mask  cap - 1, used for fast wrap-around indexing
 *  @writer.head  Producer reservation cursor
 *  @writer.tail  Producer commit cursor
 *  @reader.head  Consumer reservation cursor
 *  @reader.tail  Consumer commit cursor
 *
 * Data storage:
 *  - data[] holds pointers to enqueued elements
 *  - Storage is contiguous and wraps around using mask
 *
 * Notes:
 *  - This is the most general and most contended ring variant
 *  - Correctness requires strict adherence to memory ordering rules
 *  - Typically higher overhead than SPSC/SPMC/MPSC variants
 */
struct ring_mpmc {
    uint32_t cap;   /* Ring capacity (power of two) */
    uint32_t mask;  /* cap - 1, used for fast index wrap-around */

    struct ring_cursor writer; /* Shared producer cursors */
    struct ring_cursor reader; /* Shared consumer cursors */

    void *data[]; /* Ring storage */
};

extern struct ring_mpmc *ring_mpmc_init(int cap);
extern void ring_mpmc_fini(struct ring_mpmc *ring);

/*
 * ring_mpmc_write
 *
 * Multi-producer enqueue for a bounded ring buffer.
 *
 * High-level algorithm:
 *  1) Reserve a contiguous range [head, head+real) by CAS advancing writer.head.
 *     - Each producer gets a unique reservation (no overlap).
 *  2) Copy payload pointers into ring->data[] (may wrap).
 *  3) Publish the reservation in FIFO order by advancing writer.tail.
 *     - Only the producer that owns the current writer.tail may advance it.
 *     - Others spin until prior producers publish first.
 *
 * Memory ordering:
 *  - reader.tail is loaded with ACQUIRE to observe consumer progress (free space).
 *  - Data stores (memcpy to ring->data[]) MUST become visible before writer.tail
 *    is advanced to make them observable to consumers.
 *    => writer.tail CAS success must have RELEASE semantics.
 *
 * Notes:
 *  - This "head-reserve + tail-publish" pattern preserves FIFO visibility, but
 *    it can suffer from head-of-line blocking: a slow producer that reserved a
 *    large range can delay all later producers at the writer.tail publish step.
 *  - For true scalable MPMC, a per-slot sequence algorithm (Vyukov bounded queue)
 *    is typically preferred.
 */
static INLINE int ring_mpmc_write(struct ring_mpmc *ring, void *data[], int nums)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    head = __atomic_load_n(&ring->writer.head, __ATOMIC_RELAXED);

    do {
        tail = __atomic_load_n(&ring->reader.tail, __ATOMIC_ACQUIRE);

        real = MIN(nums, ring->cap - (head - tail));
        if (real == 0) {
            return 0;
        }
    } while (!__atomic_compare_exchange_n(&ring->writer.head,
                                          &head,
                                          head + real,
                                          true,
                                          __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));

    idx = head & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&ring->data[idx], &data[0], first * sizeof(void *));
    if (real > first) {
        memcpy(&ring->data[0], &data[first], (real - first) * sizeof(void *));
    }

    for (;;) {
        uint32_t start = head;
        if (__atomic_compare_exchange_n(&ring->writer.tail,
                                        &start,
                                        start + real,
                                        true,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            break;
        }

        __builtin_ia32_pause();
    }

    return real;
}

/*
 * ring_mpmc_read
 *
 * Multi-consumer dequeue for a bounded ring buffer.
 *
 * High-level algorithm:
 *  1) Reserve a contiguous range [head, head+real) by CAS advancing reader.head.
 *     - Each consumer gets a unique reservation.
 *  2) Copy payload pointers out of ring->data[] (may wrap).
 *  3) Publish consumption in FIFO order by advancing reader.tail.
 *     - Only the consumer that owns the current reader.tail may advance it.
 *     - Others spin until prior consumers publish first.
 *
 * Memory ordering:
 *  - writer.tail is loaded with ACQUIRE to observe producers' published writes.
 *  - reader.tail CAS success must have RELEASE semantics so producers that
 *    ACQUIRE-load reader.tail can see freed slots before reusing them.
 *
 * Notes:
 *  - This "head-reserve + tail-publish" pattern preserves order, but like the
 *    writer side it can experience head-of-line blocking.
 */
static INLINE int ring_mpmc_read(struct ring_mpmc *ring, void *data[], int max)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    head = __atomic_load_n(&ring->reader.head, __ATOMIC_RELAXED);

    do {
        tail = __atomic_load_n(&ring->writer.tail, __ATOMIC_ACQUIRE);

        real = MIN(max, tail - head);
        if (real == 0) {
            return 0;
        }
    } while (!__atomic_compare_exchange_n(&ring->reader.head,
                                          &head,
                                          head + real,
                                          true,
                                          __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));

    idx = head & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&data[0], &ring->data[idx], first * sizeof(void *));
    if (real > first) {
        memcpy(&data[first], &ring->data[0], (real - first) * sizeof(void *));
    }

    for (;;) {
        uint32_t start = head;
        if (__atomic_compare_exchange_n(&ring->reader.tail,
                                        &start,
                                        start + real,
                                        true,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            break;
        }

        __builtin_ia32_pause();
    }

    return real;
}

#endif // __RING_H__