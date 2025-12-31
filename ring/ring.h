/*
 * ring.h
 *
 * Lock-free ring buffer implementations.
 *
 * This header provides high-performance, lock-free ring queue primitives
 * designed for concurrent systems, including:
 *  - SPSC (Single Producer / Single Consumer)
 *  - MPSC (Multi Producer / Single Consumer)
 *  - MPMC (Multi Producer / Multi Consumer)
 *  - Zero-copy read/write interfaces using span descriptors
 *
 * Design goals:
 *  - Lock-free progress guarantees (no mutexes or blocking primitives)
 *  - Cache-friendly data layout and minimal false sharing
 *  - Explicit memory ordering with acquire/release semantics
 *  - Suitable for high-throughput and low-latency data paths
 *
 * Usage notes:
 *  - Each ring variant has strict concurrency assumptions; misuse will
 *    result in undefined behavior.
 *  - APIs are designed to be explicit about ownership and commit phases
 *    (e.g., read_start / read_commit).
 *  - The implementation favors simplicity and predictability over
 *    general-purpose abstractions.
 *
 * This header is intended for systems-level components such as networking,
 * storage pipelines, and event processing loops.
 */

#ifndef __RING_H__
#define __RING_H__

#include <string.h>

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

    head = __atomic_load_n(&ring->writer, __ATOMIC_RELAXED);
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
    tail = __atomic_load_n(&ring->reader, __ATOMIC_RELAXED);

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
    tail = __atomic_load_n(&ring->reader, __ATOMIC_RELAXED);

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

#endif // __RING_H__