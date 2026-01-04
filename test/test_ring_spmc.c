// test_ring_spmc.c
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

// Include your ring header
// #include "ring.h"

/* --------- You likely already have these macros in your project --------- */
#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

#ifndef INLINE
#define INLINE __inline__ __attribute__((always_inline))
#endif

#ifndef ALIGNED
#define ALIGNED(x) __attribute__((aligned(x)))
#endif

#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

static INLINE void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    sched_yield();
#endif
}

/* --------------------- Paste your ring_spmc implementation here ---------------------
 * If you already compile ring_spmc from ring.c/ring.h, remove this section and
 * just include ring.h above.
 */
struct ring_cursor {
    uint32_t head;
    uint32_t tail;
} ALIGNED(CACHE_LINE);

struct ring_spmc {
    uint32_t cap;
    uint32_t mask;

    ALIGNED(CACHE_LINE) uint32_t writer;
    struct ring_cursor reader;

    void *data[];
};

extern struct ring_spmc *ring_spmc_init(int cap);
extern void ring_spmc_fini(struct ring_spmc *ring);

static INLINE int ring_spmc_write(struct ring_spmc *ring, void *data[], uint32_t nums)
{
    uint32_t idx = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t real = 0;
    uint32_t first = 0;

    head = ring->writer;
    tail = __atomic_load_n(&ring->reader.tail, __ATOMIC_ACQUIRE);

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
    return (int)real;
}

static INLINE int ring_spmc_read(struct ring_spmc *ring, void *data[], uint32_t max)
{
    uint32_t idx = 0;
    uint32_t real = 0;
    uint32_t first = 0;
    uint32_t start = 0;
    uint32_t reader_head = 0;
    uint32_t writer_head = 0;

    reader_head = __atomic_load_n(&ring->reader.head, __ATOMIC_RELAXED);

    do {
        writer_head = __atomic_load_n(&ring->writer, __ATOMIC_ACQUIRE);

        real = MIN(max, writer_head - reader_head);
        if (real == 0) {
            return 0;
        }
    } while (!__atomic_compare_exchange_n(&ring->reader.head,
                                          &reader_head,
                                          reader_head + real,
                                          true,
                                          __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));

    idx = reader_head & ring->mask;
    first = MIN(real, ring->cap - idx);
    memcpy(&data[0], &ring->data[idx], first * sizeof(void *));
    if (real > first) {
        memcpy(&data[first], &ring->data[0], (real - first) * sizeof(void *));
    }

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
        cpu_relax();
    }

    return (int)real;
}

struct ring_spmc *ring_spmc_init(int cap)
{
    int ret = 0;
    size_t total = 0;
    struct ring_spmc *ring = NULL;

    if (UNLIKELY(cap <= 0 || (cap & (cap - 1)) != 0)) {
        return NULL;
    }

    total = sizeof(struct ring_spmc) + (size_t)cap * sizeof(void *);
    ret = posix_memalign((void **)&ring, CACHE_LINE, total);
    if (UNLIKELY(ret != 0)) {
        return NULL;
    }

    memset(ring, 0, total);

    ring->cap = (uint32_t)cap;
    ring->mask = (uint32_t)cap - 1;
    ring->writer = 0;
    ring->reader.head = 0;
    ring->reader.tail = 0;

    return ring;
}

void ring_spmc_fini(struct ring_spmc *ring)
{
    if (ring == NULL) {
        return;
    }
    free(ring);
}
/* --------------------- end ring_spmc implementation --------------------- */

/* --------------------- Test payload --------------------- */
enum { ITEM_MAGIC = 0xC0FFEE42 };

struct item {
    uint32_t magic;
    uint32_t seq;
};

static INLINE uint32_t fast_rand_u32(uint64_t *state)
{
    // xorshift64*
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return (uint32_t)((x * 2685821657736338717ULL) >> 32);
}

struct test_ctx {
    struct ring_spmc *ring;

    uint32_t cap;
    uint32_t consumers;
    uint32_t total_items;

    // seen[seq] == 1 once consumed (atomic byte)
    uint8_t *seen;

    // counters
    uint32_t produced;
    uint32_t consumed;

    // flags
    int producer_done;
};

static INLINE int ring_is_empty(struct ring_spmc *ring)
{
    uint32_t w = __atomic_load_n(&ring->writer, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&ring->reader.tail, __ATOMIC_ACQUIRE);
    return w == t;
}

static void *producer_thread(void *arg)
{
    struct test_ctx *ctx = (struct test_ctx *)arg;
    uint64_t rng = (uint64_t)time(NULL) ^ (uintptr_t)pthread_self();

    const uint32_t max_batch = 32;
    void *batch[max_batch];

    uint32_t seq = 0;
    while (seq < ctx->total_items) {
        uint32_t want = (fast_rand_u32(&rng) % max_batch) + 1;
        want = MIN(want, ctx->total_items - seq);

        // prepare items
        for (uint32_t i = 0; i < want; i++) {
            struct item *it = (struct item *)malloc(sizeof(*it));
            assert(it != NULL);
            it->magic = ITEM_MAGIC;
            it->seq = seq + i;
            batch[i] = it;
        }

        // enqueue (may be partial)
        uint32_t off = 0;
        while (off < want) {
            int n = ring_spmc_write(ctx->ring, &batch[off], want - off);
            if (n > 0) {
                off += (uint32_t)n;
                __atomic_add_fetch(&ctx->produced, (uint32_t)n, __ATOMIC_RELAXED);
            } else {
                // ring full
                cpu_relax();
            }
        }

        seq += want;
    }

    __atomic_store_n(&ctx->producer_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

struct consumer_arg {
    struct test_ctx *ctx;
    uint32_t id;
};

static void *consumer_thread(void *arg)
{
    struct consumer_arg *ca = (struct consumer_arg *)arg;
    struct test_ctx *ctx = ca->ctx;

    uint64_t rng = ((uint64_t)time(NULL) << 1) ^ (uintptr_t)pthread_self() ^ (uint64_t)ca->id;
    const uint32_t max_batch = 32;
    void *batch[max_batch];

    for (;;) {
        uint32_t want = (fast_rand_u32(&rng) % max_batch) + 1;

        int n = ring_spmc_read(ctx->ring, batch, want);
        if (n == 0) {
            // If producer is done and ring empty, we are done.
            int done = __atomic_load_n(&ctx->producer_done, __ATOMIC_ACQUIRE);
            if (done && ring_is_empty(ctx->ring)) {
                break;
            }
            cpu_relax();
            continue;
        }

        for (int i = 0; i < n; i++) {
            struct item *it = (struct item *)batch[i];
            assert(it != NULL);
            assert(it->magic == ITEM_MAGIC);
            assert(it->seq < ctx->total_items);

            // mark seen[seq] atomically, ensure no duplicates
            uint8_t old = __atomic_exchange_n(&ctx->seen[it->seq], 1, __ATOMIC_ACQ_REL);
            if (old != 0) {
                fprintf(stderr, "ERROR: duplicate consume seq=%u (consumer=%u)\n",
                        it->seq, ca->id);
                abort();
            }

            free(it);
        }

        __atomic_add_fetch(&ctx->consumed, (uint32_t)n, __ATOMIC_RELAXED);
    }

    return NULL;
}

/* --------------------- Test cases --------------------- */

static void test_init_params(void)
{
    printf("[TEST] init params...\n");

    assert(ring_spmc_init(0) == NULL);
    assert(ring_spmc_init(-1) == NULL);

    // not power of two
    assert(ring_spmc_init(3) == NULL);
    assert(ring_spmc_init(6) == NULL);
    assert(ring_spmc_init(1023) == NULL);

    // power of two
    struct ring_spmc *r = ring_spmc_init(1024);
    assert(r != NULL);
    assert(r->cap == 1024);
    assert(r->mask == 1023);
    ring_spmc_fini(r);

    printf("  OK\n");
}

static void run_spmc_stress(uint32_t cap, uint32_t consumers, uint32_t total_items)
{
    printf("[TEST] SPMC stress: cap=%u consumers=%u items=%u ...\n",
           cap, consumers, total_items);

    struct test_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.cap = cap;
    ctx.consumers = consumers;
    ctx.total_items = total_items;

    ctx.ring = ring_spmc_init((int)cap);
    assert(ctx.ring != NULL);

    ctx.seen = (uint8_t *)calloc((size_t)total_items, 1);
    assert(ctx.seen != NULL);

    pthread_t prod;
    pthread_t *th = (pthread_t *)calloc(consumers, sizeof(pthread_t));
    struct consumer_arg *args = (struct consumer_arg *)calloc(consumers, sizeof(*args));
    assert(th && args);

    // start consumers first (common in real systems)
    for (uint32_t i = 0; i < consumers; i++) {
        args[i].ctx = &ctx;
        args[i].id = i;
        int rc = pthread_create(&th[i], NULL, consumer_thread, &args[i]);
        assert(rc == 0);
    }

    int rc = pthread_create(&prod, NULL, producer_thread, &ctx);
    assert(rc == 0);

    pthread_join(prod, NULL);
    for (uint32_t i = 0; i < consumers; i++) {
        pthread_join(th[i], NULL);
    }

    uint32_t produced = __atomic_load_n(&ctx.produced, __ATOMIC_RELAXED);
    uint32_t consumed = __atomic_load_n(&ctx.consumed, __ATOMIC_RELAXED);

    // basic counts
    assert(produced == total_items);
    assert(consumed == total_items);

    // verify all seen exactly once
    for (uint32_t i = 0; i < total_items; i++) {
        if (ctx.seen[i] != 1) {
            fprintf(stderr, "ERROR: missing seq=%u\n", i);
            abort();
        }
    }

    free(args);
    free(th);
    free(ctx.seen);
    ring_spmc_fini(ctx.ring);

    printf("  OK (produced=%u consumed=%u)\n", produced, consumed);
}

int main(void)
{
    test_init_params();

    // small cap -> heavy wrap-around + contention
    run_spmc_stress(/*cap=*/64,  /*consumers=*/2,  /*items=*/200000000);
    run_spmc_stress(/*cap=*/64,  /*consumers=*/4,  /*items=*/200000000);

    // larger cap -> higher throughput, still concurrent
    run_spmc_stress(/*cap=*/1024, /*consumers=*/4, /*items=*/500000000);

    // edge case: 1 consumer (still should work)
    run_spmc_stress(/*cap=*/128, /*consumers=*/1, /*items=*/200000000);

    printf("All tests passed.\n");
    return 0;
}
