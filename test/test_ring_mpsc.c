#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <errno.h>

#include "ring.h"

/* ---------------- utils ---------------- */

static inline uint64_t nsec_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

#define DIE(...) do { \
    fprintf(stderr, "FATAL: " __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    exit(1); \
} while (0)

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED at %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        exit(2); \
    } \
} while (0)

/* ---------------- test config ---------------- */

typedef enum {
    MODE_COPY_READ = 0,
    MODE_SPAN_READ = 1,
} read_mode_t;

typedef struct {
    struct ring_mpsc *ring;
    int producers;
    uint64_t per_prod;      // items per producer
    int max_batch;          // max items each write() tries
    int max_read;           // max items each read() tries
    read_mode_t mode;

    // pointer pool: objs[pid][seq] stored as pointers in ring
    uint64_t *values;       // size = producers * per_prod

    _Atomic uint64_t produced_total;
    _Atomic uint64_t consumed_total;

    unsigned seed;
} ctx_t;

typedef struct {
    ctx_t *c;
    int pid;
    unsigned seed;
} prod_arg_t;

/* values layout:
 * values[pid * per_prod + seq] = (pid << 32) | seq
 */
static inline uint64_t make_id(int pid, uint32_t seq) {
    return ((uint64_t)(uint32_t)pid << 32) | (uint64_t)seq;
}

static inline int id_pid(uint64_t id) { return (int)(id >> 32); }
static inline uint32_t id_seq(uint64_t id) { return (uint32_t)id; }

/* ---------------- producer ---------------- */

static void *producer_thread(void *arg)
{
    prod_arg_t *pa = (prod_arg_t *)arg;
    ctx_t *c = pa->c;
    int pid = pa->pid;

    void *tmp[65536];
    if (c->max_batch > (int)(sizeof(tmp)/sizeof(tmp[0]))) {
        DIE("max_batch too large");
    }

    uint64_t base = (uint64_t)pid * c->per_prod;
    uint64_t i = 0;

    while (i < c->per_prod) {
        int want = (c->max_batch == 1) ? 1 : (1 + (rand_r(&pa->seed) % (unsigned)c->max_batch));
        if (i + (uint64_t)want > c->per_prod) want = (int)(c->per_prod - i);

        for (int k = 0; k < want; k++) {
            tmp[k] = (void *)&c->values[base + i + (uint64_t)k];
        }

        int n = ring_mpsc_write(c->ring, tmp, want);
        if (n == 0) {
            cpu_relax();
            continue;
        }

        i += (uint64_t)n;
        atomic_fetch_add_explicit(&c->produced_total, (uint64_t)n, memory_order_relaxed);
    }

    return NULL;
}

/* ---------------- consumer ---------------- */

static void consume_ptr(ctx_t *c,
                        void *p,
                        uint32_t *next_seq,      // size producers
                        uint8_t *seen)           // bitset: 1 byte per item (simple, big but easy)
{
    uint64_t id = *(uint64_t *)p;
    int pid = id_pid(id);
    uint32_t seq = id_seq(id);

    CHECK(pid >= 0 && pid < c->producers, "bad pid=%d", pid);
    CHECK((uint64_t)seq < c->per_prod, "bad seq=%" PRIu32 " for pid=%d", seq, pid);

    // per-producer in-order check
    uint32_t exp = next_seq[pid];
    if (seq != exp) {
        DIE("ORDER mismatch: pid=%d expect_seq=%" PRIu32 " got_seq=%" PRIu32, pid, exp, seq);
    }
    next_seq[pid] = exp + 1;

    // uniqueness check (no duplicate)
    uint64_t idx = (uint64_t)pid * c->per_prod + (uint64_t)seq;
    if (seen[idx]) {
        DIE("DUPLICATE id: pid=%d seq=%" PRIu32, pid, seq);
    }
    seen[idx] = 1;
}

static void *consumer_thread(void *arg)
{
    ctx_t *c = (ctx_t *)arg;

    uint64_t total_need = (uint64_t)c->producers * c->per_prod;

    uint32_t *next_seq = (uint32_t *)calloc((size_t)c->producers, sizeof(uint32_t));
    CHECK(next_seq != NULL, "calloc next_seq failed");

    // simple seen array (1 byte per item)
    uint8_t *seen = (uint8_t *)calloc((size_t)total_need, 1);
    CHECK(seen != NULL, "calloc seen failed");

    void *out[65536];
    if (c->max_read > (int)(sizeof(out)/sizeof(out[0]))) {
        DIE("max_read too large");
    }

    uint64_t consumed = 0;

    while (consumed < total_need) {
        int got = 0;

        if (c->mode == MODE_COPY_READ) {
            int want = (c->max_read == 1) ? 1 : (1 + (rand_r(&c->seed) % (unsigned)c->max_read));
            uint64_t left = total_need - consumed;
            if ((uint64_t)want > left) want = (int)left;

            got = ring_mpsc_read(c->ring, out, want);
            if (got == 0) {
                cpu_relax();
                continue;
            }

            for (int i = 0; i < got; i++) {
                consume_ptr(c, out[i], next_seq, seen);
            }
        } else {
            struct ring_span span;
            ring_mpsc_read_start(c->ring, &span, c->max_read);

            if (span.total == 0) {
                cpu_relax();
                continue;
            }

            for (uint32_t i = 0; i < span.count1; i++) {
                consume_ptr(c, span.p1[i], next_seq, seen);
            }
            for (uint32_t i = 0; i < span.count2; i++) {
                consume_ptr(c, span.p2[i], next_seq, seen);
            }

            ring_mpsc_read_commit(c->ring, &span);
            got = (int)span.total;
        }

        consumed += (uint64_t)got;
        atomic_store_explicit(&c->consumed_total, consumed, memory_order_relaxed);
    }

    // final check: every producer finished exactly per_prod
    for (int pid = 0; pid < c->producers; pid++) {
        CHECK((uint64_t)next_seq[pid] == c->per_prod,
              "producer %d missing items: got=%" PRIu32 " need=%" PRIu64,
              pid, next_seq[pid], c->per_prod);
    }

    // final check: all seen
    for (uint64_t i = 0; i < total_need; i++) {
        if (!seen[i]) DIE("MISSING item at linear idx=%" PRIu64, i);
    }

    free(seen);
    free(next_seq);
    return NULL;
}

/* ---------------- runner ---------------- */

static void run_one_case(const char *name,
                         uint32_t cap,
                         int producers,
                         uint64_t per_prod,
                         int max_batch,
                         int max_read,
                         read_mode_t mode,
                         unsigned seed)
{
    printf("=== %s ===\n", name);
    printf("cap=%u producers=%d per_prod=%" PRIu64 " total=%" PRIu64
           " max_batch=%d max_read=%d mode=%s\n",
           cap, producers, per_prod, (uint64_t)producers * per_prod,
           max_batch, max_read, (mode==MODE_COPY_READ?"copy":"span"));

    struct ring_mpsc *ring = ring_mpsc_init((int)cap);
    CHECK(ring != NULL, "ring_mpsc_init failed");

    ctx_t c;
    memset(&c, 0, sizeof(c));
    c.ring = ring;
    c.producers = producers;
    c.per_prod = per_prod;
    c.max_batch = max_batch;
    c.max_read = max_read;
    c.mode = mode;
    c.seed = seed;

    uint64_t total_need = (uint64_t)producers * per_prod;

    // allocate values array and fill deterministic ids
    c.values = (uint64_t *)malloc(sizeof(uint64_t) * total_need);
    CHECK(c.values != NULL, "malloc values failed");

    for (int pid = 0; pid < producers; pid++) {
        uint64_t base = (uint64_t)pid * per_prod;
        for (uint64_t s = 0; s < per_prod; s++) {
            c.values[base + s] = make_id(pid, (uint32_t)s);
        }
    }

    pthread_t tr;
    pthread_t *tp = (pthread_t *)calloc((size_t)producers, sizeof(pthread_t));
    prod_arg_t *pa = (prod_arg_t *)calloc((size_t)producers, sizeof(prod_arg_t));
    CHECK(tp && pa, "calloc tp/pa failed");

    uint64_t t0 = nsec_now();

    int rc = pthread_create(&tr, NULL, consumer_thread, &c);
    CHECK(rc == 0, "pthread_create consumer failed: %s", strerror(rc));

    for (int pid = 0; pid < producers; pid++) {
        pa[pid].c = &c;
        pa[pid].pid = pid;
        pa[pid].seed = seed ^ (unsigned)(pid * 2654435761u);
        rc = pthread_create(&tp[pid], NULL, producer_thread, &pa[pid]);
        CHECK(rc == 0, "pthread_create producer %d failed: %s", pid, strerror(rc));
    }

    for (int pid = 0; pid < producers; pid++) {
        pthread_join(tp[pid], NULL);
    }
    pthread_join(tr, NULL);

    uint64_t t1 = nsec_now();
    double sec = (double)(t1 - t0) / 1e9;

    CHECK(atomic_load(&c.produced_total) == total_need, "produced_total mismatch");
    CHECK(atomic_load(&c.consumed_total) == total_need, "consumed_total mismatch");

    printf("PASS: %s, time=%.3f s, throughput=%.2f M items/s\n\n",
           name, sec, (double)total_need / sec / 1e6);

    free(pa);
    free(tp);
    free(c.values);
    ring_mpsc_fini(ring);
}

int main(void)
{
    // 建议：cap 至少大于 producers * max_batch，避免过度拥塞影响性能
    const uint32_t cap = 1u << 16;

    // 根据机器调整：每个 producer 产出数量
    const uint64_t per_prod = 5ULL * 1000 * 1000;

    run_one_case("MPSC copy: 4 producers", cap, 4, per_prod, 32, 64, MODE_COPY_READ, 123);
    run_one_case("MPSC copy: 8 producers", cap, 8, per_prod, 32, 64, MODE_COPY_READ, 456);

    run_one_case("MPSC span: 4 producers", cap, 4, per_prod, 32, 64, MODE_SPAN_READ, 789);
    run_one_case("MPSC span: 8 producers", cap, 8, per_prod, 32, 64, MODE_SPAN_READ, 999);

    printf("ALL DONE.\n");
    return 0;
}
