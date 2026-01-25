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

struct ctx {
    void *ring;
    uint64_t test_items;
    uint32_t write_item;
    uint32_t read_item;
    enum READER_MODE mode;
};

static void *spsc_write_task(void *arg)
{
    struct ctx *ctx = (struct ctx *) arg;

    pthread_exit(NULL);
}

static void *spsc_read_task(void *arg)
{
    int total = 0;
    const uint32_t count = 1024 * 1024;
    struct ctx *ctx = (struct ctx *) arg;
    struct ring_spsc *ring = (struct ring_spsc *)ctx->ring;
    // uint32_t *read_pool = malloc(sizeof(*read_pool) * count);

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
    struct ctx ctx = {
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

static void test_spmc_init_case(void)
{
    int cap = 1024 * 1024;
    struct ring_spmc *ring = NULL;

    printf("======================== SPMC INIT FINI ===============================\n");
    CHECK_NE(ring = ring_spmc_init(cap), NULL, "test case0: ring_spmc_init 1024 * 1024", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "test case0: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "test case0: mask", "SUCCESS", "FAILURE");
    ring_spmc_fini(ring);

    cap = 1024;
    CHECK_NE(ring = ring_spmc_init(cap), NULL, "test case1: ring_spmc_init 1024", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "test case1: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "test case 1: mask", "SUCCESS", "FAILURE");
    ring_spmc_fini(ring);

    cap = 1;
    CHECK_NE(ring = ring_spmc_init(cap), NULL, "test case2: ring_spmc_init 1", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "test case2: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "test case2: mask", "SUCCESS", "FAILURE");
    ring_spmc_fini(ring);

    cap = 1024 + 1;
    CHECK_EQ(ring = ring_spmc_init(cap), NULL, "test case3: ring_spmc_init 1025 (not power of 2)", "SUCCESS", "FAILURE");
    ring_spmc_fini(ring);
}

static void test_mpsc_init_case(void)
{
        int cap = 1024 * 1024;
    struct ring_mpsc *ring = NULL;

    printf("======================== MPSC INIT FINI ===============================\n");

    cap = 1024 * 1024;
    CHECK_NE(ring = ring_mpsc_init(cap), NULL, "MPSC test case0: ring_mpsc_init 1024 * 1024", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "MPSC test case0: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "MPSC test case0: mask", "SUCCESS", "FAILURE");
    ring_mpsc_fini(ring);

    cap = 1024;
    CHECK_NE(ring = ring_mpsc_init(cap), NULL, "MPSC test case1: ring_mpsc_init 1024", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "MPSC test case1: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "MPSC test case1: mask", "SUCCESS", "FAILURE");
    ring_mpsc_fini(ring);

    cap = 1;
    CHECK_NE(ring = ring_mpsc_init(cap), NULL, "MPSC test case2: ring_mpsc_init 1", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->cap, cap, "MPSC test case2: cap", "SUCCESS", "FAILURE");
    CHECK_EQ(ring->mask, cap - 1, "MPSC test case2: mask", "SUCCESS", "FAILURE");
    ring_mpsc_fini(ring);

    cap = 1024 + 1;
    CHECK_EQ(ring = ring_mpsc_init(cap), NULL, "MPSC test case3: ring_mpsc_init 1025 (not power of 2)", "SUCCESS", "FAILURE");
    ring_mpsc_fini(ring);
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

static void test_init_case(void)
{
    test_spsc_init_case();
    test_spmc_init_case();
    test_mpsc_init_case();
    test_mpmc_init_case();
}

int main(int argc, char *argv[])
{
    test_init_case();

    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 1, 1, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 1, 1, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 2, 2, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 2, 2, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 4, 4, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 4, 4, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 8, 8, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 8, 8, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 16, 16, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 16, 16, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 32, 32, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 32, 32, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 64, 64, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 64, 64, READER_SPAN);
    test_spsc_case("SPSC copy: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 128, 128, READER_COPY);
    test_spsc_case("SPSC span: 1w1r", 1u << 20, 1024UL * 1024 * 1024 * 20, 128, 128, READER_SPAN);

    return EXIT_SUCCESS;
}
