/******************************************************************************************
 * test
 ******************************************************************************************/

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

static inline void cpu_relax(void) {
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
    MODE_COPY_READ = 0,   // ring_spsc_read copies data into out[]
    MODE_SPAN_READ = 1,   // ring_spsc_read_start/commit, read directly from ring memory
} read_mode_t;

typedef struct {
    struct ring_spsc *ring;
    uint64_t total_items;
    int      max_batch;      // Maximum number of items written per writer operation
    int      max_read;       // Maximum number of items read per reader operation
    read_mode_t mode;
    unsigned seed;
    _Atomic uint64_t produced;
    _Atomic uint64_t consumed;
    _Atomic int stop;
} ctx_t;

/* ---------------- writer/reader ---------------- */

// To reduce malloc/free interference: we use a global array to store uint64_t values.
// The ring stores pointers. Each pointer refers to values[i], so the reader can
// directly dereference the pointer to validate ordering.
static uint64_t *g_values = NULL;

static void *writer_thread(void *arg)
{
    ctx_t *c = (ctx_t *)arg;

    void *tmp[65536]; // Large enough (ring capacity is usually <= 64k)
    if (c->max_batch > (int)(sizeof(tmp)/sizeof(tmp[0]))) {
        DIE("max_batch too large");
    }

    uint64_t i = 0;
    while (i < c->total_items) {
        int want;

        if (c->max_batch == 1) {
            want = 1;
        } else {
            // 1..max_batch
            want = 1 + (rand_r(&c->seed) % (unsigned)c->max_batch);
        }
        if (i + (uint64_t)want > c->total_items) {
            want = (int)(c->total_items - i);
        }

        for (int k = 0; k < want; k++) {
            tmp[k] = (void *)&g_values[i + (uint64_t)k];
        }

        int n = ring_spsc_write(c->ring, tmp, want);
        if (n == 0) {
            cpu_relax();
            continue;
        }

        i += (uint64_t)n;
        atomic_store_explicit(&c->produced, i, memory_order_relaxed);
    }

    return NULL;
}

static void *reader_thread(void *arg)
{
    ctx_t *c = (ctx_t *)arg;

    uint64_t expect = 0;
    void *out[65536];
    if (c->max_read > (int)(sizeof(out)/sizeof(out[0]))) {
        DIE("max_read too large");
    }

    while (expect < c->total_items) {
        int got = 0;

        if (c->mode == MODE_COPY_READ) {
            int want = c->max_read == 1 ? 1 : (1 + (rand_r(&c->seed) % (unsigned)c->max_read));
            if ((uint64_t)want > (c->total_items - expect)) {
                want = (int)(c->total_items - expect);
            }

            got = ring_spsc_read(c->ring, out, want);
            if (got == 0) {
                cpu_relax();
                continue;
            }

            for (int i = 0; i < got; i++) {
                uint64_t v = *(uint64_t *)out[i];
                if (v != expect) {
                    DIE("COPY_READ mismatch: expect=%" PRIu64 " got=%" PRIu64, expect, v);
                }
                expect++;
            }
        } else {
            // Span read: obtain span, traverse ring memory directly, then commit
            struct ring_span span;
            ring_spsc_read_start(c->ring, &span, 100);

            if (span.total == 0) {
                cpu_relax();
                continue;
            }

            // To cover the wrap-around case, validate p1 and p2 separately
            for (uint32_t i = 0; i < span.count1; i++) {
                uint64_t v = *(uint64_t *)span.p1[i];
                if (v != expect) {
                    DIE("SPAN_READ mismatch in p1: expect=%" PRIu64 " got=%" PRIu64 " (i=%u)", expect, v, i);
                }
                expect++;
            }
            for (uint32_t i = 0; i < span.count2; i++) {
                uint64_t v = *(uint64_t *)span.p2[i];
                if (v != expect) {
                    DIE("SPAN_READ mismatch in p2: expect=%" PRIu64 " got=%" PRIu64 " (i=%u)", expect, v, i);
                }
                expect++;
            }

            ring_spsc_read_commit(c->ring, &span);
            got = (int)span.total;
        }

        atomic_store_explicit(&c->consumed, expect, memory_order_relaxed);
        (void)got;
    }

    return NULL;
}

/* ---------------- test runner ---------------- */

static void run_one_case(const char *name,
                         uint32_t cap,
                         uint64_t total_items,
                         int max_batch,
                         int max_read,
                         read_mode_t mode,
                         unsigned seed)
{
    printf("=== %s ===\n", name);
    printf("cap=%u total=%" PRIu64 " max_batch=%d max_read=%d mode=%s\n",
           cap, total_items, max_batch, max_read, (mode==MODE_COPY_READ?"copy":"span"));

    struct ring_spsc *ring = ring_spsc_init((int)cap);
    CHECK(ring != NULL, "ring_spsc_init failed");

    ctx_t c;
    memset(&c, 0, sizeof(c));
    c.ring = ring;
    c.total_items = total_items;
    c.max_batch = max_batch;
    c.max_read  = max_read;
    c.mode = mode;
    c.seed = seed;

    pthread_t tw, tr;

    uint64_t t0 = nsec_now();

    int rc = pthread_create(&tw, NULL, writer_thread, &c);
    CHECK(rc == 0, "pthread_create writer failed: %s", strerror(rc));
    rc = pthread_create(&tr, NULL, reader_thread, &c);
    CHECK(rc == 0, "pthread_create reader failed: %s", strerror(rc));

    pthread_join(tw, NULL);
    pthread_join(tr, NULL);

    uint64_t t1 = nsec_now();
    double sec = (double)(t1 - t0) / 1e9;

    CHECK(atomic_load(&c.produced) == total_items, "produced mismatch");
    CHECK(atomic_load(&c.consumed) == total_items, "consumed mismatch");

    printf("PASS: %s, time=%.3f s, throughput=%.2f M items/s\n\n",
           name, sec, (double)total_items / sec / 1e6);

    ring_spsc_fini(ring);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    // Increase total for stress testing
    const uint64_t total = 32ULL * 1000 * 1000 * 16; // items

    // Prepare data: values[i] = i
    g_values = (uint64_t *)malloc(sizeof(uint64_t) * total);
    CHECK(g_values != NULL, "malloc g_values failed");
    for (uint64_t i = 0; i < total; i++) {
        g_values[i] = i;
    }

    run_one_case("SPSC copy: 1w1r",  1u<<16, total, 1, 1, MODE_COPY_READ, 1);
    run_one_case("SPSC copy: 2w2r",  1u<<16, total, 2, 2, MODE_COPY_READ, 2);
    run_one_case("SPSC copy: 4w4r",  1u<<16, total, 4, 4, MODE_COPY_READ, 4);
    run_one_case("SPSC copy: 8w8r",  1u<<16, total, 8, 8, MODE_COPY_READ, 8);
    run_one_case("SPSC copy: 16w16r",  1u<<16, total, 16, 16, MODE_COPY_READ, 16);
    run_one_case("SPSC copy: 32w32r",  1u<<16, total, 32, 32, MODE_COPY_READ, 32);
    run_one_case("SPSC copy: 64w64r",  1u<<16, total, 64, 64, MODE_COPY_READ, 64);

    run_one_case("SPSC copy: random batch", 1u<<16, total, 64, 64, MODE_COPY_READ, 456);

    run_one_case("SPSC span: random batch", 1u<<16, total, 1, 1, MODE_SPAN_READ, 1);
    run_one_case("SPSC span: random batch", 1u<<16, total, 2, 2, MODE_SPAN_READ, 2);
    run_one_case("SPSC span: random batch", 1u<<16, total, 4, 4, MODE_SPAN_READ, 4);
    run_one_case("SPSC span: random batch", 1u<<16, total, 8, 8, MODE_SPAN_READ, 8);
    run_one_case("SPSC span: random batch", 1u<<16, total, 16, 16, MODE_SPAN_READ, 16);
    run_one_case("SPSC span: random batch", 1u<<16, total, 32, 32, MODE_SPAN_READ, 32);
    run_one_case("SPSC span: random batch", 1u<<16, total, 64, 64, MODE_SPAN_READ, 64);

    run_one_case("SPSC span: small cap wrap-around", 1024, total, 32, 32, MODE_SPAN_READ, 2024);

    free(g_values);
    printf("ALL DONE.\n");
    return 0;
}