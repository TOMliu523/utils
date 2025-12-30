/******************************************************************************************
 * Lock-free SPSC ring (Single Producer / Single Consumer)
 * Lock-free MPMC ring (Multi Producer / Multi Consumer)
 * Lock-free ring: Single Writer + Multiple Readers with ordered consumption (SPMR-Ordered)
 ******************************************************************************************/

#ifndef __RING_H__
#define __RING_H__

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifndef MIN
#define MIN(x, y) \
    ({ \
        typeof(x) _x = (x); \
        typeof(y) _y = (y); \
        _x > _y ? _y : _x; \
    })
#endif // MIN

#ifndef MAX
#define MAX(x, y) \
    ({ \
        typeof(x) _x = (x); \
        typeof(y) _y = (y); \
        _x > _y ? _x ? _y; \
    })
#endif // MAX

#ifndef ALIGNED
#define ALIGNED(n) __attribute__((aligned(n)))
#endif // ALIGNED

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif // CACHE_LINE

#ifndef INLINE
#define INLINE inline __attribute__((always_inline))
#endif //INLINE

#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#endif // LIKELY

#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif // LIKELY

/*
 * Lock-free SPSC ring (Single Producer / Single Consumer)
 *
 * Goal:
 *  - Exactly one writer thread (producer) and one reader thread (consumer)
 *  - No locks, no CAS, only atomic loads/stores with proper memory ordering
 *
 * Structure / invariants:
 *  - Capacity (cap) must be a power of two; mask = cap - 1 is used for fast wrap-around.
 *  - 'head' is written ONLY by the producer.
 *  - 'tail' is written ONLY by the consumer.
 *    => Because each index has a single writer, we do not need compare-and-swap.
 *
 * Concurrency & memory ordering:
 *  - Producer:
 *      1) Writes elements into ring->data[head ...].
 *      2) Publishes the new head with a RELEASE store.
 *    The RELEASE ensures that all element writes become visible before head is observed by the consumer.
 *
 *  - Consumer:
 *      1) Reads head with an ACQUIRE load.
 *         ACQUIRE pairs with producer's RELEASE so the consumer sees the element writes.
 *      2) Reads elements from ring->data[tail ...].
 *      3) Publishes the new tail with a RELEASE store.
 *
 *  - Producer reads tail with an ACQUIRE load to observe freed slots before writing new elements.
 *
 * Notes:
 *  - This design is safe only for 1P/1C. Adding more producers or consumers requires a different algorithm.
 *  - Prefer padding head/tail to separate cache lines to avoid false sharing.
 */

struct ring_spsc {
    int cap;
    int mask;

    ALIGNED(CACHE_LINE) uint32_t head;
    ALIGNED(CACHE_LINE) uint32_t tail;

    void *data[];
};

extern struct ring_spsc *ring_spsc_init(int cap);
extern void ring_spsc_fini(struct ring_spsc *ring);

static INLINE int ring_spsc_write(struct ring_spsc *ring, void *data[], int nums)
{
    uint32_t top = 0;
    uint32_t diff = 0;
    uint32_t real = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t remain = 0;

    head = ring->head;
    tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);

    remain = ring->cap - (head - tail);
    real = MIN(remain, nums);
    if (real == 0) {
        return 0;
    }

    diff = ring->cap - (head & ring->mask);
    top = MIN(diff, real);
    memcpy(&ring->data[head & ring->mask], data, top * sizeof(void*));
    if (real > top) {
        memcpy(&ring->data[0], &data[top], (real - top) * sizeof(void *));
    }

    __atomic_store_n(&ring->head, head + real, __ATOMIC_RELEASE);
    return real;
}

static INLINE int ring_spsc_read(struct ring_spsc *ring, void *data[], int max)
{
    uint32_t top = 0;
    uint32_t diff = 0;
    uint32_t real = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t remain = 0;

    tail = ring->tail;
    head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);

    remain = head - tail;
    real = MIN(max, remain);
    if (real == 0) {
        return 0;
    }

    diff = ring->cap - (tail & ring->mask);
    top = MIN(real, diff);

    memcpy(data, &ring->data[tail & ring->mask], top * sizeof(void *));
    if (real > top) {
        memcpy(&data[top], &ring->data[0], (real - top) * sizeof(void *));
    }

    __atomic_store_n(&ring->tail, tail + real, __ATOMIC_RELEASE);
    return real;
}

/*
 * Lock-free MPMC ring (Multi Producer / Multi Consumer)
 *
 * Goal:
 *  - Multiple producers and multiple consumers concurrently enqueue/dequeue
 *  - Lock-free progress (system as a whole makes progress), typically using CAS and per-slot sequence
 *
 * Recommended algorithm:
 *  - Use per-slot sequence numbers (a.k.a. "bounded MPMC queue" / Dmitry Vyukov algorithm).
 *  - Maintain two monotonic counters:
 *      - enqueue_pos (tail-like): next position to reserve for producers
 *      - dequeue_pos (head-like): next position to reserve for consumers
 *  - Each slot has a 'seq' that indicates whether it is ready for enqueue or dequeue.
 *
 * Slot state model (conceptual):
 *  - Initially: slot[i].seq = i
 *  - Producer wants to write at position p:
 *      - The slot is writable if slot[p].seq == p
 *      - Producer CAS-reserves enqueue_pos from p to p+1
 *      - Producer writes data into slot[p]
 *      - Producer RELEASE-stores slot[p].seq = p + 1 to publish the element
 *
 *  - Consumer wants to read at position c:
 *      - The slot is readable if slot[c].seq == c + 1
 *      - Consumer CAS-reserves dequeue_pos from c to c+1
 *      - Consumer reads data from slot[c]
 *      - Consumer RELEASE-stores slot[c].seq = c + cap to mark the slot free for the next wrap
 *
 * Memory ordering rules:
 *  - Producer publishes element with RELEASE store to slot.seq (or a RELEASE store to a ready flag).
 *  - Consumer reads slot.seq with ACQUIRE load before reading the element.
 *    => Ensures consumer sees the producer's data writes.
 *  - Similarly, consumer RELEASE-stores the seq when freeing; producers ACQUIRE-load it before writing again.
 *
 * Notes:
 *  - This design is bounded (fixed capacity), avoids ABA with per-slot sequence numbers, and scales well.
 *  - CAS contention can be reduced by batching and by using per-NUMA rings if needed.
 *  - Do NOT attempt to extend an SPSC algorithm to MPMC by merely making head/tail atomic; it will break.
 */
struct writer {
    uint32_t head;
    uint32_t tail;
} ALIGNED(CACHE_LINE);

struct reader {
    uint32_t head;
    uint32_t tail;
} ALIGNED(CACHE_LINE);

struct ring_mpmc {
    uint32_t cap;
    uint32_t mask;

    struct writer writer;
    struct reader reader;

    void *data[];
};

extern struct ring_mpmc *ring_mpmc_init(int cap);
extern void ring_mpmc_fini(struct ring_mpmc *);

static INLINE int ring_mpmc_write(struct ring_mpmc *ring, void *data[], int nums)
{
    uint32_t idx = 0;
    uint32_t real = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t first = 0;
    uint32_t start = 0;
    uint32_t remain = 0;

    do {
        head = __atomic_load_n(&ring->writer.head, __ATOMIC_RELAXED);
        tail = __atomic_load_n(&ring->reader.tail, __ATOMIC_ACQUIRE);

        remain = ring->cap - (head - tail);
        real = MIN(remain, nums);

        if (real == 0) {
            return 0;
        }

        start = head;
    } while (!__atomic_compare_exchange_n(&ring->writer.head,
                                          &start,
                                          head + real,
                                          true,
                                          __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));

    idx = head & ring->mask;
    first = MIN(ring->cap - idx, real);

    memcpy(&ring->data[idx], data, first * sizeof(void *));
    if (real > first) {
        memcpy(&ring->data[0], &data[first], (real - first) * sizeof(void *));
    }

    for (;;) {
        start = head;
        if (__atomic_compare_exchange_n(&ring->writer.tail, &start, head + real, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            break;
        }

        __builtin_ia32_pause();
    }

    return real;
}

static INLINE int ring_mpmc_read(struct ring_mpmc *ring, void *data[], int max)
{
    uint32_t idx = 0;
    uint32_t real = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t first = 0;
    uint32_t start = 0;

    do {
        head = __atomic_load_n(&ring->reader.head, __ATOMIC_RELAXED);
        tail = __atomic_load_n(&ring->writer.tail, __ATOMIC_ACQUIRE);

        real = MIN(tail - head, max);
        if (real == 0) {
            return 0;
        }

        start = head;
    } while (!__atomic_compare_exchange_n(&ring->reader.head,
                                          &start,
                                          head + real,
                                          true,
                                          __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));

    idx = head & ring->mask;
    first = MIN(ring->cap - idx, real);
    memcpy(&data[0], &ring->data[idx], first * sizeof(void *));
    if (real > first) {
        memcpy(&data[first], &ring->data[0], (real - first) * sizeof(void *));
    }

    for (;;) {
        start = head;
        if (__atomic_compare_exchange_n(&ring->reader.tail, &start, head + real, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            break;
        }

        __builtin_ia32_pause();
    }

    return real;
}

/*
 * Lock-free ring: Single Writer + Multiple Readers with ordered consumption (SPMR-Ordered)
 *
 * Use case:
 *  - One writer produces a global ordered stream of entries.
 *  - Multiple readers (different roles/consumers) must process entries in the same order,
 *    but at different speeds (each reader has its own cursor).
 *
 * Key idea:
 *  - One shared write cursor: write_pos (monotonic).
 *  - One read cursor per reader: read_pos[role] (monotonic, updated ONLY by that reader).
 *  - The ring can overwrite old entries ONLY when all readers have advanced past them.
 *    Practically: the writer computes the minimum read position across all roles:
 *        min_read = min(read_pos[0..R-1])
 *    and ensures (write_pos - min_read) < cap before writing.
 *
 * Correctness invariants:
 *  - Writer is the only thread that writes data slots and advances write_pos.
 *  - Each reader 'r' is the only thread that advances read_pos[r].
 *  - A reader may read slot at position p only if p < write_pos (i.e., it has been published).
 *  - Writer may reuse/overwrite slot at position p only if p < min_read (i.e., all readers have consumed it).
 *
 * Memory ordering:
 *  - Writer:
 *      1) Writes entry payload into ring->data[pos & mask].
 *      2) Publishes by RELEASE-storing write_pos = pos + 1.
 *  - Reader:
 *      1) ACQUIRE-load write_pos to confirm an entry is published.
 *      2) Reads entry payload.
 *      3) RELEASE-store read_pos[role] = pos + 1 to publish its consumption progress.
 *  - Writer ACQUIRE-loads each read_pos[role] when computing min_read, so it sees readers' progress.
 *
 * Backpressure / capacity:
 *  - Slowest reader determines throughput. If any reader stalls, writer may block/fail enqueue
 *    once the ring is full relative to min_read.
 *
 * Notes:
 *  - If the number of roles is large, computing min_read every enqueue can be expensive.
 *    Options: compute min_read periodically, keep a cached min, or use a hierarchical min structure.
 *  - Consider cache-line padding for each read_pos[role] to avoid false sharing among readers.
 *  - This pattern preserves per-role in-order processing without locks.
 */
struct role {
    uint32_t cursor;
} ALIGNED(CACHE_LINE);

struct ring_spmro {
    uint32_t cap;
    uint32_t mask;
    int n_role;

    ALIGNED(CACHE_LINE) uint32_t head;
    struct role *role;

    void *data[];
};

extern struct ring_spmro *ring_spmro_init(int cap, int role_nums);
extern void ring_spmro_fini(struct ring_spmro *);

static INLINE int ring_spmro_write(struct ring_spmro *ring, void *data[], int nums)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    head = ring->head;
    tail = __atomic_load_n(&ring->role[ring->n_role - 1].cursor, __ATOMIC_ACQUIRE);
    real = MIN(ring->cap - (head - tail), nums);
    if (real == 0) {
        return 0;
    }

    idx = head & ring->mask;
    first = MIN(ring->cap - idx, real);
    memcpy(&ring->data[idx], &data[0], first * sizeof(void *));
    if (real > first) {
        memcpy(&ring->data[0], &data[first], (real - first) * sizeof(void *));
    }

    __atomic_store_n(&ring->head, head + real, __ATOMIC_RELEASE);
    return real;
}

static INLINE int ring_spmro_read(struct ring_spmro *ring, int id, void *data[], int max)
{
    uint32_t idx = 0;
    uint32_t cur = 0;
    uint32_t prev = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    cur = ring->role[id].cursor;
    prev = __atomic_load_n((id == 0) ? &ring->head : &ring->role[id - 1].cursor, __ATOMIC_ACQUIRE);
    real = MIN(prev - cur, max);
    if (real == 0) {
        return 0;
    }

    idx = cur & ring->mask;
    first = (ring->cap -  idx, );

    return 0;
}

#endif // __RING_H__