#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "ring.h"

#define CHECK_EQ(_expr, _result, _info, _succ, _fail) \
    do { \
        if ((_expr) == (_result)) { \
            fprintf(stderr, "%s", _succ); \
        } else { \
            fprintf(stderr, "%s", _fail); \
        } \
        fprintf(stderr, " -> %s\n", _info); \
    } while (0)

#define CHECK_NE(_expr, _result, _info, _succ, _fail) \
    do { \
        if ((_expr) != (_result)) { \
            fprintf(stderr, "%s", _succ); \
        } else { \
            fprintf(stderr, "%s", _fail); \
        } \
        fprintf(stderr, " -> %s\n", _info); \
    } while (0)

enum READER_MODE {
    READER_COPY,
    READER_SPAN,
};

struct spsc_ctx {
    void *ring;
    uint64_t test_items;
    uint32_t write_item;
    uint32_t read_item;
    enum READER_MODE mode;
};

static void *spsc_write_task(void *arg)
{
    struct spsc_ctx *ctx = (struct spsc_ctx *)arg;
    struct ring_spsc *ring = (struct ring_spsc *)ctx->ring;

    const uint64_t total = ctx->test_items;
    const uint32_t batch = ctx->write_item;
    uint64_t produced = 0;

    void *buf[256];
    if (batch > sizeof(buf) / sizeof(buf[0])) {
        fprintf(stderr, "write batch too large: %u\n", batch);
        pthread_exit(NULL);
    }

    struct timespec ts_start = {0};
    struct timespec ts_end = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    while (produced < total) {
        uint32_t n = batch;
        if (n > total - produced) {
            n = (uint32_t)(total - produced);
        }

        for (uint32_t i = 0; i < n; ++i) {
            buf[i] = (void *)(uintptr_t)(produced + i);
        }

        uint32_t left = n;
        uint32_t offset = 0;

        while (left > 0) {
            uint32_t ret = ring_spsc_write(ring, &buf[offset], left);
            if (ret == 0) {
                sched_yield();
                continue;
            }
            left   -= ret;
            offset += ret;
            produced += ret;
        }
    }

    /* Record end timestamp for the writer */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec = (double)(ts_end.tv_sec  - ts_start.tv_sec) + (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    if (elapsed_sec > 0.0) {
        double ops_per_sec = (double)total / elapsed_sec;
        fprintf(stderr, "[SPSC writer] total=%lu, elapsed=%.6f s, rate=%.3f Mops/s\n",
                (unsigned long)total, elapsed_sec, ops_per_sec / 1e6);
    } else {
        fprintf(stderr, "[SPSC writer] elapsed time too small (<= 0), total=%lu\n", (unsigned long)total);
    }

    pthread_exit(NULL);
}

static void *spsc_read_task(void *arg)
{
    struct spsc_ctx *ctx = (struct spsc_ctx *)arg;
    struct ring_spsc *ring = (struct ring_spsc *)ctx->ring;

    const uint64_t total = ctx->test_items;
    const uint32_t batch = ctx->read_item;
    uint64_t consumed = 0;
    uint64_t error_cnt = 0;

    void *buf[256];
    if (batch > sizeof(buf) / sizeof(buf[0])) {
        fprintf(stderr, "read batch too large: %u\n", batch);
        pthread_exit(NULL);
    }

    struct timespec ts_start = {0};
    struct timespec ts_end = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    while (consumed < total) {
        uint32_t n = batch;
        if (n > total - consumed) {
            n = (uint32_t)(total - consumed);
        }

        if (ctx->mode == READER_COPY) {
            uint32_t got = ring_spsc_read(ring, buf, n);
            if (got == 0) {
                sched_yield();
                continue;
            }

            for (uint32_t i = 0; i < got; ++i) {
                uint64_t v = (uint64_t)(uintptr_t)buf[i];
                uint64_t expect = consumed + i;
                if (v != expect) {
                    ++error_cnt;
                    // fprintf(stderr, "SPSC read error: expect=%lu, got=%lu\n",
                    //         (unsigned long)expect, (unsigned long)v);
                }
            }

            consumed += got;
        } else {

            /*
             *   uint32_t ring_spsc_read_start(struct ring_spsc *r,
             *                                 struct ring_span *span,
             *                                 uint32_t max);
             *   void     ring_spsc_read_commit(struct ring_spsc *r,
             *                                  struct ring_span *span);
             */

            struct ring_span span;
            ring_spsc_read_start(ring, &span, n);
            if (span.total == 0) {
                sched_yield();
                continue;
            }

            uint64_t expect = consumed;
            void **p = span.p1;
            for (uint32_t i = 0; i < span.count1; ++i) {
                uint64_t v = (uint64_t)(uintptr_t)p[i];
                if (v != expect) {
                    ++error_cnt;
                }
                ++expect;
            }
            p = span.p2;
            for (uint32_t i = 0; i < span.count2; ++i) {
                uint64_t v = (uint64_t)(uintptr_t)p[i];
                if (v != expect) {
                    ++error_cnt;
                }
                ++expect;
            }

            consumed += span.total;

            ring_spsc_read_commit(ring, &span);
        }
    }

    /* Record end timestamp for the reader */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec = (double)(ts_end.tv_sec  - ts_start.tv_sec) + (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (error_cnt == 0) {
        fprintf(stderr, "[SPSC reader] total=%lu, write: %d, read: %d, verify OK\n", (unsigned long)total, ctx->write_item, ctx->read_item);
    } else {
        fprintf(stderr, "[SPSC reader] total=%lu, error_cnt=%lu\n", (unsigned long)total, (unsigned long)error_cnt);
    }

    if (elapsed_sec > 0.0) {
        double ops_per_sec = (double)consumed / elapsed_sec;
        fprintf(stderr, "[SPSC reader] elapsed=%.6f s, rate=%.3f Mops/s\n", elapsed_sec, ops_per_sec / 1e6);
    } else {
        fprintf(stderr, "[SPSC reader] elapsed time too small (<= 0), total=%lu\n", (unsigned long)consumed);
    }

    pthread_exit(NULL);
}

static void test_spsc_case(const char *description,
                           int cap,
                           uint64_t test_items,
                           uint32_t write_item,
                           uint32_t read_item,
                           enum READER_MODE mode)
{
    pthread_t wid = {0};
    pthread_t rid = {0};
    struct spsc_ctx ctx = {
        .ring = NULL,
        .test_items = test_items,
        .write_item = write_item,
        .read_item = read_item,
        .mode = mode,
    };

    ctx.ring = ring_spsc_init(cap);

    printf("====================> %s: %lu <====================\n", description, test_items);
    pthread_create(&wid, NULL, spsc_write_task, &ctx);
    pthread_create(&rid, NULL, spsc_read_task, &ctx);

    pthread_join(wid, NULL);
    pthread_join(rid, NULL);

    ring_spsc_fini(ctx.ring);
}

static void test_spsc_init_case(void)
{
    int cap = 1024 * 1024;
    struct ring_spsc *ring = NULL;

    printf("======================== SPSC INIT FINI ===============================\n");
    CHECK_NE(ring = ring_spsc_init(cap), NULL, "test case0: ring_spsc_init 1024 * 1024", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "test case0: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "test case0: mask", "SUCCESS", "FAILURE");
    ring_spsc_fini(ring);

    cap = 1024;
    CHECK_NE(ring = ring_spsc_init(cap), NULL, "test case1: ring_spsc_init 1024", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "test case1: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "test case1: mask", "SUCCESS", "FAILURE");

    void *write = (void *)(intptr_t)0x12345678;
    ring_spsc_write(ring, &write, 1);
    CHECK_EQ(ring->writer, 1, "test case1: write cursor", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->reader, 0, "test case1: read cursor", "SUCCESS", "FAILURE");

    void *read = NULL;
    ring_spsc_read(ring, &read, 1);
    CHECK_EQ(ring->writer, 1, "test case1: write cursor", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->reader, 1, "test case1: read cursor", "SUCCESS", "FAILURE");

    ring_spsc_fini(ring);

    cap = 1;
    CHECK_NE(ring = ring_spsc_init(cap), NULL, "test case2: ring_spsc_init 1", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "test case2: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "test case2: mask", "SUCCESS", "FAILURE");
    ring_spsc_fini(ring);

    cap = 1024 + 1;
    CHECK_EQ(ring = ring_spsc_init(cap), NULL, "test case2: ring_spsc_init 1025 (not power of 2)", "SUCCESS", "FAILURE");
    ring_spsc_fini(ring);
}

static void test_mpmc_init_case(void)
{
    int cap = 1024 * 1024;
    struct ring_mpmc *ring = NULL;

    printf("======================== MPMC INIT FINI ===============================\n");

    cap = 1024 * 1024;
    CHECK_NE(ring = ring_mpmc_init(cap), NULL, "MPMC test case0: ring_mpmc_init 1024 * 1024", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "MPMC test case0: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "MPMC test case0: mask", "SUCCESS", "FAILURE");
    ring_mpmc_fini(ring);

    cap = 1024;
    CHECK_NE(ring = ring_mpmc_init(cap), NULL, "MPMC test case1: ring_mpmc_init 1024", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "MPMC test case1: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "MPMC test case1: mask", "SUCCESS", "FAILURE");
    ring_mpmc_fini(ring);

    cap = 1;
    CHECK_NE(ring = ring_mpmc_init(cap), NULL, "MPMC test case2: ring_mpmc_init 1", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "MPMC test case2: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "MPMC test case2: mask", "SUCCESS", "FAILURE");
    ring_mpmc_fini(ring);

    cap = 1024 + 1;
    CHECK_EQ(ring = ring_mpmc_init(cap), NULL, "MPMC test case3: ring_mpmc_init 1025 (not power of 2)", "SUCCESS", "FAILURE");
    ring_mpmc_fini(ring);
}

static void test_spsc(void)
{
    test_spsc_init_case();
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 1, 1, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 1, 1, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 2, 2, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 2, 2, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 4, 4, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 4, 4, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 8, 8, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 8, 8, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 16, 16, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 16, 16, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 32, 32, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 32, 32, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 64, 64, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 64, 64, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 128, 128, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024, 128, 128, READER_SPAN);
}

int main(int argc, char *argv[])
{
    test_spsc();

    return EXIT_SUCCESS;
}