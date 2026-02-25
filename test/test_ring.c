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

struct mpsc_producer_ctx {
    struct ring_mpsc *ring;
    uint64_t items_per_producer;
    uint32_t batch;
    uint32_t producer_id;      /* 0 .. producer_count-1 */
};

struct mpsc_consumer_ctx {
    struct ring_mpsc *ring;
    uint64_t total_items;
    uint32_t batch;
};

struct spmc_writer_ctx {
    struct ring_spmc *ring;
    uint64_t total_items;
    uint32_t batch;
};

struct spmc_reader_ctx {
    struct ring_spmc *ring;
    uint64_t total_items;
    uint32_t batch;
    uint32_t consumer_id;
    volatile uint64_t *global_consumed;
};

struct mpmc_producer_ctx {
    struct ring_mpmc *ring;
    uint64_t items_per_producer;
    uint32_t batch;
    uint32_t producer_id;
};

struct mpmc_consumer_ctx {
    struct ring_mpmc *ring;
    uint64_t total_items;
    uint32_t batch;
    uint32_t consumer_id;
    volatile uint64_t *global_consumed;
};

static void *mpmc_write_task(void *arg)
{
    struct mpmc_producer_ctx *ctx = (struct mpmc_producer_ctx *)arg;
    struct ring_mpmc *ring = ctx->ring;

    const uint64_t total = ctx->items_per_producer;
    const uint32_t batch = ctx->batch;
    uint64_t produced = 0;

    /* Each producer writes in its own value range for easier debugging */
    uint64_t base = (uint64_t)ctx->producer_id * total;

    void *buf[256];
    if (batch > sizeof(buf) / sizeof(buf[0])) {
        fprintf(stderr, "[MPMC writer %u] write batch too large: %u\n",
                ctx->producer_id, batch);
        pthread_exit(NULL);
    }

    struct timespec ts_start = {0};
    struct timespec ts_end   = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (produced < total) {
        uint32_t n = batch;
        if (n > total - produced) {
            n = (uint32_t)(total - produced);
        }

        for (uint32_t i = 0; i < n; ++i) {
            buf[i] = (void *)(uintptr_t)(base + produced + i);
        }

        uint32_t left   = n;
        uint32_t offset = 0;

        while (left > 0) {
            uint32_t ret = ring_mpmc_write(ring, &buf[offset], left);
            if (ret == 0) {
                sched_yield();
                continue;
            }

            left    -= ret;
            offset  += ret;
            produced += ret;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec =
        (double)(ts_end.tv_sec  - ts_start.tv_sec) +
        (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (elapsed_sec > 0.0) {
        double ops_per_sec = (double)total / elapsed_sec;
        fprintf(stderr,
                "[MPMC writer %u] total=%lu, elapsed=%.6f s, rate=%.3f Mops/s\n",
                ctx->producer_id,
                (unsigned long)total,
                elapsed_sec,
                ops_per_sec / 1e6);
    } else {
        fprintf(stderr,
                "[MPMC writer %u] elapsed time too small (<= 0), total=%lu\n",
                ctx->producer_id,
                (unsigned long)total);
    }

    pthread_exit(NULL);
}

static void *mpmc_read_task(void *arg)
{
    struct mpmc_consumer_ctx *ctx = (struct mpmc_consumer_ctx *)arg;
    struct ring_mpmc *ring = ctx->ring;

    const uint64_t total = ctx->total_items;
    const uint32_t batch = ctx->batch;
    uint64_t local_consumed = 0;

    void *buf[256];
    if (batch > sizeof(buf) / sizeof(buf[0])) {
        fprintf(stderr, "[MPMC reader %u] read batch too large: %u\n",
                ctx->consumer_id, batch);
        pthread_exit(NULL);
    }

    struct timespec ts_start = {0};
    struct timespec ts_end   = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (1) {
        uint64_t g = *ctx->global_consumed;
        if (g >= total) {
            break;
        }

        uint32_t n = batch;
        uint64_t remaining = total - g;
        if (n > remaining) {
            n = (uint32_t)remaining;
        }

        uint32_t got = ring_mpmc_read(ring, buf, n);
        if (got == 0) {
            sched_yield();
            continue;
        }

        /* We only care about total counts here.
         * Strict ordering across producers/consumers is not validated.
         */
        __sync_fetch_and_add(ctx->global_consumed, got);
        local_consumed += got;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec =
        (double)(ts_end.tv_sec  - ts_start.tv_sec) +
        (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (elapsed_sec > 0.0 && local_consumed > 0) {
        double ops_per_sec = (double)local_consumed / elapsed_sec;
        fprintf(stderr,
                "[MPMC reader %u] local=%lu, elapsed=%.6f s, rate=%.3f Mops/s\n",
                ctx->consumer_id,
                (unsigned long)local_consumed,
                elapsed_sec,
                ops_per_sec / 1e6);
    } else {
        fprintf(stderr,
                "[MPMC reader %u] elapsed too small or no items, local=%lu\n",
                ctx->consumer_id,
                (unsigned long)local_consumed);
    }

    pthread_exit(NULL);
}

static void test_mpmc_case(const char *description,
                           int cap,
                           uint32_t producers,
                           uint32_t consumers,
                           uint64_t items_per_producer,
                           uint32_t write_batch,
                           uint32_t read_batch)
{
    printf("====================> %s <====================\n", description);

    struct ring_mpmc *ring = ring_mpmc_init(cap);
    if (!ring) {
        fprintf(stderr, "[MPMC] ring_mpmc_init(%d) failed, abort case: %s\n",
                cap, description);
        return;
    }

    pthread_t *writer_tids  = NULL;
    pthread_t *reader_tids  = NULL;
    struct mpmc_producer_ctx *wctx = NULL;
    struct mpmc_consumer_ctx *rctx = NULL;

    volatile uint64_t global_consumed = 0;
    uint64_t total_items = (uint64_t)producers * items_per_producer;

    writer_tids = calloc(producers, sizeof(*writer_tids));
    reader_tids = calloc(consumers, sizeof(*reader_tids));
    if (!writer_tids || !reader_tids) {
        fprintf(stderr, "[MPMC] failed to alloc thread arrays\n");
        goto out;
    }

    wctx = calloc(producers, sizeof(*wctx));
    rctx = calloc(consumers, sizeof(*rctx));
    if (!wctx || !rctx) {
        fprintf(stderr, "[MPMC] failed to alloc ctx arrays\n");
        goto out;
    }

    /* Prepare consumer contexts */
    for (uint32_t i = 0; i < consumers; ++i) {
        rctx[i].ring            = ring;
        rctx[i].total_items     = total_items;
        rctx[i].batch           = read_batch;
        rctx[i].consumer_id     = i;
        rctx[i].global_consumed = &global_consumed;
    }

    /* Prepare producer contexts */
    for (uint32_t i = 0; i < producers; ++i) {
        wctx[i].ring               = ring;
        wctx[i].items_per_producer = items_per_producer;
        wctx[i].batch              = write_batch;
        wctx[i].producer_id        = i;
    }

    /* Start consumers first so they are ready to drain the ring */
    for (uint32_t i = 0; i < consumers; ++i) {
        pthread_create(&reader_tids[i], NULL, mpmc_read_task, &rctx[i]);
    }

    /* Start producers */
    for (uint32_t i = 0; i < producers; ++i) {
        pthread_create(&writer_tids[i], NULL, mpmc_write_task, &wctx[i]);
    }

    /* Join all producers */
    for (uint32_t i = 0; i < producers; ++i) {
        pthread_join(writer_tids[i], NULL);
    }

    /* Join all consumers */
    for (uint32_t i = 0; i < consumers; ++i) {
        pthread_join(reader_tids[i], NULL);
    }

    fprintf(stderr,
            "[MPMC case done] desc=\"%s\", producers=%u, consumers=%u, "
            "total_items=%lu, global_consumed=%lu\n",
            description,
            producers,
            consumers,
            (unsigned long)total_items,
            (unsigned long)global_consumed);

out:
    if (wctx)        free(wctx);
    if (rctx)        free(rctx);
    if (writer_tids) free(writer_tids);
    if (reader_tids) free(reader_tids);
    ring_mpmc_fini(ring);
}

static void *spmc_write_task(void *arg)
{
    struct spmc_writer_ctx *ctx = (struct spmc_writer_ctx *)arg;
    struct ring_spmc *ring = ctx->ring;

    const uint64_t total = ctx->total_items;
    const uint32_t batch = ctx->batch;
    uint64_t produced = 0;

    void *buf[256];
    if (batch > sizeof(buf) / sizeof(buf[0])) {
        fprintf(stderr, "[SPMC writer] write batch too large: %u\n", batch);
        pthread_exit(NULL);
    }

    struct timespec ts_start = {0};
    struct timespec ts_end   = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (produced < total) {
        uint32_t n = batch;
        if (n > total - produced) {
            n = (uint32_t)(total - produced);
        }

        for (uint32_t i = 0; i < n; ++i) {
            buf[i] = (void *)(uintptr_t)(produced + i);
        }

        uint32_t left   = n;
        uint32_t offset = 0;

        while (left > 0) {
            uint32_t ret = ring_spmc_write(ring, &buf[offset], left);
            if (ret == 0) {
                sched_yield();
                continue;
            }

            left    -= ret;
            offset  += ret;
            produced += ret;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec =
        (double)(ts_end.tv_sec  - ts_start.tv_sec) +
        (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (elapsed_sec > 0.0) {
        double ops_per_sec = (double)total / elapsed_sec;
        fprintf(stderr,
                "[SPMC writer] total=%lu, elapsed=%.6f s, rate=%.3f Mops/s\n",
                (unsigned long)total,
                elapsed_sec,
                ops_per_sec / 1e6);
    } else {
        fprintf(stderr,
                "[SPMC writer] elapsed time too small (<= 0), total=%lu\n",
                (unsigned long)total);
    }

    pthread_exit(NULL);
}

static void *spmc_read_task(void *arg)
{
    struct spmc_reader_ctx *ctx = (struct spmc_reader_ctx *)arg;
    struct ring_spmc *ring = ctx->ring;

    const uint64_t total = ctx->total_items;
    const uint32_t batch = ctx->batch;
    uint64_t local_consumed = 0;

    void *buf[256];
    if (batch > sizeof(buf) / sizeof(buf[0])) {
        fprintf(stderr, "[SPMC reader %u] read batch too large: %u\n",
                ctx->consumer_id, batch);
        pthread_exit(NULL);
    }

    struct timespec ts_start = {0};
    struct timespec ts_end   = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (1) {
        uint64_t g = *ctx->global_consumed;
        if (g >= total) {
            break;
        }

        uint32_t n = batch;
        uint64_t remaining = total - g;
        if (n > remaining) {
            n = (uint32_t)remaining;
        }

        uint32_t got = ring_spmc_read(ring, buf, n);
        if (got == 0) {
            sched_yield();
            continue;
        }

        /* Increase global and local counters.
         * We do not enforce strict atomicity for the remaining calculation;
         * minor race is acceptable for performance testing.
         */
        __sync_fetch_and_add(ctx->global_consumed, got);
        local_consumed += got;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec =
        (double)(ts_end.tv_sec  - ts_start.tv_sec) +
        (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (elapsed_sec > 0.0 && local_consumed > 0) {
        double ops_per_sec = (double)local_consumed / elapsed_sec;
        fprintf(stderr,
                "[SPMC reader %u] local=%lu, elapsed=%.6f s, rate=%.3f Mops/s\n",
                ctx->consumer_id,
                (unsigned long)local_consumed,
                elapsed_sec,
                ops_per_sec / 1e6);
    } else {
        fprintf(stderr,
                "[SPMC reader %u] elapsed too small or no items, local=%lu\n",
                ctx->consumer_id,
                (unsigned long)local_consumed);
    }

    pthread_exit(NULL);
}

static void test_spmc_case(const char *description,
                           int cap,
                           uint32_t consumers,
                           uint64_t total_items,
                           uint32_t write_batch,
                           uint32_t read_batch)
{
    printf("====================> %s <====================\n", description);

    struct ring_spmc *ring = ring_spmc_init(cap);
    if (!ring) {
        fprintf(stderr, "[SPMC] ring_spmc_init(%d) failed, abort case: %s\n",
                cap, description);
        return;
    }

    pthread_t writer_tid;
    pthread_t *reader_tids = NULL;

    struct spmc_writer_ctx wctx;
    struct spmc_reader_ctx *rctx = NULL;

    volatile uint64_t global_consumed = 0;

    reader_tids = calloc(consumers, sizeof(*reader_tids));
    if (!reader_tids) {
        fprintf(stderr, "[SPMC] failed to alloc reader threads\n");
        ring_spmc_fini(ring);
        return;
    }

    rctx = calloc(consumers, sizeof(*rctx));
    if (!rctx) {
        fprintf(stderr, "[SPMC] failed to alloc reader ctx\n");
        free(reader_tids);
        ring_spmc_fini(ring);
        return;
    }

    /* Prepare writer context */
    wctx.ring        = ring;
    wctx.total_items = total_items;
    wctx.batch       = write_batch;

    /* Prepare reader contexts */
    for (uint32_t i = 0; i < consumers; ++i) {
        rctx[i].ring           = ring;
        rctx[i].total_items    = total_items;
        rctx[i].batch          = read_batch;
        rctx[i].consumer_id    = i;
        rctx[i].global_consumed = &global_consumed;
    }

    /* Start readers first so they are ready to consume */
    for (uint32_t i = 0; i < consumers; ++i) {
        pthread_create(&reader_tids[i], NULL, spmc_read_task, &rctx[i]);
    }

    /* Start single writer */
    pthread_create(&writer_tid, NULL, spmc_write_task, &wctx);

    /* Wait for writer */
    pthread_join(writer_tid, NULL);

    /* Wait for all readers */
    for (uint32_t i = 0; i < consumers; ++i) {
        pthread_join(reader_tids[i], NULL);
    }

    fprintf(stderr,
            "[SPMC case done] desc=\"%s\", total_items=%lu, global_consumed=%lu\n",
            description,
            (unsigned long)total_items,
            (unsigned long)global_consumed);

    free(rctx);
    free(reader_tids);
    ring_spmc_fini(ring);
}

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

static void *mpsc_write_task(void *arg)
{
    struct mpsc_producer_ctx *ctx = (struct mpsc_producer_ctx *)arg;
    struct ring_mpsc *ring = ctx->ring;

    const uint64_t total = ctx->items_per_producer;
    const uint32_t batch = ctx->batch;
    uint64_t produced = 0;

    /* Each producer uses its own ID to build a unique value space if needed */
    uint64_t base = (uint64_t)ctx->producer_id * total;

    void *buf[256];
    if (batch > sizeof(buf) / sizeof(buf[0])) {
        fprintf(stderr, "[MPSC writer %u] write batch too large: %u\n", ctx->producer_id, batch);
        pthread_exit(NULL);
    }

    struct timespec ts_start = {0};
    struct timespec ts_end   = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (produced < total) {
        uint32_t n = batch;
        if (n > total - produced) {
            n = (uint32_t)(total - produced);
        }

        for (uint32_t i = 0; i < n; ++i) {
            buf[i] = (void *)(uintptr_t)(base + produced + i);
        }

        uint32_t left   = n;
        uint32_t offset = 0;

        while (left > 0) {
            uint32_t ret = ring_mpsc_write(ring, &buf[offset], left);
            if (ret == 0) {
                sched_yield();
                continue;
            }

            left    -= ret;
            offset  += ret;
            produced += ret;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec =
        (double)(ts_end.tv_sec  - ts_start.tv_sec) +
        (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (elapsed_sec > 0.0) {
        double ops_per_sec = (double)total / elapsed_sec;
        fprintf(stderr,
                "[MPSC writer %u] total=%lu, elapsed=%.6f s, rate=%.3f Mops/s\n",
                ctx->producer_id,
                (unsigned long)total,
                elapsed_sec,
                ops_per_sec / 1e6);
    } else {
        fprintf(stderr,
                "[MPSC writer %u] elapsed time too small (<= 0), total=%lu\n",
                ctx->producer_id,
                (unsigned long)total);
    }

    pthread_exit(NULL);
}

static void *mpsc_read_task(void *arg)
{
    struct mpsc_consumer_ctx *ctx = (struct mpsc_consumer_ctx *)arg;
    struct ring_mpsc *ring = ctx->ring;

    const uint64_t total = ctx->total_items;
    const uint32_t batch = ctx->batch;
    uint64_t consumed = 0;

    void *buf[256];
    if (batch > sizeof(buf) / sizeof(buf[0])) {
        fprintf(stderr, "[MPSC reader] read batch too large: %u\n", batch);
        pthread_exit(NULL);
    }

    struct timespec ts_start = {0};
    struct timespec ts_end   = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (consumed < total) {
        uint32_t n = batch;
        if (n > total - consumed) {
            n = (uint32_t)(total - consumed);
        }

        uint32_t got = ring_mpsc_read(ring, buf, n);
        if (got == 0) {
            /* Ring is empty, yield and retry */
            sched_yield();
            continue;
        }

        /* For now we only count items, not verify ordering across producers. */
        consumed += got;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec =
        (double)(ts_end.tv_sec  - ts_start.tv_sec) +
        (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (elapsed_sec > 0.0) {
        double ops_per_sec = (double)consumed / elapsed_sec;
        fprintf(stderr,
                "[MPSC reader] total=%lu, elapsed=%.6f s, rate=%.3f Mops/s\n",
                (unsigned long)consumed,
                elapsed_sec,
                ops_per_sec / 1e6);
    } else {
        fprintf(stderr,
                "[MPSC reader] elapsed time too small (<= 0), total=%lu\n",
                (unsigned long)consumed);
    }

    pthread_exit(NULL);
}

static void test_mpsc_case(const char *description,
                           int cap,
                           uint32_t producers,
                           uint64_t items_per_producer,
                           uint32_t write_batch,
                           uint32_t read_batch)
{
    pthread_t *wids = NULL;
    pthread_t rid   = 0;

    struct ring_mpsc *ring = NULL;

    printf("====================> %s <====================\n", description);

    ring = ring_mpsc_init(cap);
    if (!ring) {
        fprintf(stderr, "[MPSC] ring_mpsc_init(%d) failed, abort case: %s\n",
                cap, description);
        return;
    }

    /* Allocate producer contexts and thread IDs */
    wids = calloc(producers, sizeof(*wids));
    if (wids != NULL) {
        fprintf(stderr, "[MPSC] failed to alloc writer threads\n");
        ring_mpsc_fini(ring);
        return;
    }

    struct mpsc_producer_ctx *pctx = calloc(producers, sizeof(*pctx));
    if (pctx != NULL) {
        fprintf(stderr, "[MPSC] failed to alloc writer ctx\n");
        free(wids);
        ring_mpsc_fini(ring);
        return;
    }

    struct mpsc_consumer_ctx rctx = {
        .ring        = ring,
        .total_items = (uint64_t)producers * items_per_producer,
        .batch       = read_batch,
    };

    /* Start consumer first (optional, but usually fine) */
    pthread_create(&rid, NULL, mpsc_read_task, &rctx);

    /* Start producers */
    for (uint32_t i = 0; i < producers; ++i) {
        pctx[i].ring              = ring;
        pctx[i].items_per_producer = items_per_producer;
        pctx[i].batch             = write_batch;
        pctx[i].producer_id       = i;

        pthread_create(&wids[i], NULL, mpsc_write_task, &pctx[i]);
    }

    /* Wait for all producers */
    for (uint32_t i = 0; i < producers; ++i) {
        pthread_join(wids[i], NULL);
    }

    /* Wait for consumer */
    pthread_join(rid, NULL);

    free(pctx);
    free(wids);
    ring_mpsc_fini(ring);
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

static void test_mpsc(void)
{
    test_mpsc_init_case();

    /*
     * MPSC Benchmark Matrix
     *
     * Parameters:
     *   cap       = ring capacity (must be power-of-2)
     *   producers = number of writer threads
     *   items     = number of items per producer
     *   wb / rb   = writer batch / reader batch size
     *
     * Note:
     *   Each item is a pointer (uint64_t encoded), so:
     *     256M items ≈ 2GB payload
     */

    uint64_t items = 64UL * 1024 * 1024;   /* 256M items */

    printf("======================== MPSC PERFORMANCE ===============================\n");

    /*
     * 1. Producer scaling (fixed batch size)
     *    Observe how throughput scales with contention on the writer side.
     */
    test_mpsc_case("MPSC: 1w1r, batch=32",
                   1u << 20,
                   1,
                   items,
                   32,
                   32);

    test_mpsc_case("MPSC: 2w1r, batch=32",
                   1u << 20,
                   2,
                   items,
                   32,
                   32);

    test_mpsc_case("MPSC: 4w1r, batch=32",
                   1u << 20,
                   4,
                   items,
                   32,
                   32);

    test_mpsc_case("MPSC: 8w1r, batch=32",
                   1u << 20,
                   8,
                   items,
                   32,
                   32);

    /*
     * 2. Batch scaling (fixed producer count)
     *    Small batches → high metadata overhead
     *    Large batches → improved throughput, worse latency
     */
    test_mpsc_case("MPSC: 4w1r, batch=1",
                   1u << 20,
                   4,
                   items,
                   1,
                   1);

    test_mpsc_case("MPSC: 4w1r, batch=8",
                   1u << 20,
                   4,
                   items,
                   8,
                   8);

    test_mpsc_case("MPSC: 4w1r, batch=32",
                   1u << 20,
                   4,
                   items,
                   32,
                   32);

    test_mpsc_case("MPSC: 4w1r, batch=128",
                   1u << 20,
                   4,
                   items,
                   128,
                   128);

    test_mpsc_case("MPSC: 4w1r, batch=256",
                   1u << 20,
                   4,
                   items,
                   256,
                   256);

    /*
     * 3. Capacity scaling
     *    Large ring reduces wrap-around contention and improves burst behavior.
     */
    test_mpsc_case("MPSC: 4w1r, cap=1M, batch=64",
                   1u << 20,   /* cap = 1M */
                   4,
                   items,
                   64,
                   64);

    test_mpsc_case("MPSC: 4w1r, cap=4M, batch=64",
                   1u << 22,   /* cap = 4M */
                   4,
                   items,
                   64,
                   64);

    test_mpsc_case("MPSC: 4w1r, cap=16M, batch=64",
                   1u << 24,   /* cap = 16M */
                   4,
                   items,
                   64,
                   64);

    /*
     * 4. Heavy-load scenario
     *    Push ring under sustained producer pressure.
     */
    test_mpsc_case("MPSC: 8w1r, batch=128 heavy",
                   1u << 22,   /* larger capacity to reduce immediate contention */
                   8,
                   512UL * 1024 * 1024, /* 512M items per producer */
                   128,
                   128);
}

static void test_spmc(void)
{
    test_spmc_init_case();

    uint64_t items = 64UL * 1024 * 1024;   /* total items written by single producer */

    printf("======================== SPMC PERFORMANCE ===============================\n");

    /* consumer scaling */
    test_spmc_case("SPMC: 1w1r, batch=32",
                   1u << 20,
                   1,
                   items,
                   32,
                   32);

    test_spmc_case("SPMC: 1w2r, batch=32",
                   1u << 20,
                   2,
                   items,
                   32,
                   32);

    test_spmc_case("SPMC: 1w4r, batch=32",
                   1u << 20,
                   4,
                   items,
                   32,
                   32);

    test_spmc_case("SPMC: 1w8r, batch=32",
                   1u << 20,
                   8,
                   items,
                   32,
                   32);

    /* batch scaling */
    test_spmc_case("SPMC: 1w4r, batch=1",
                   1u << 20,
                   4,
                   items,
                   1,
                   1);

    test_spmc_case("SPMC: 1w4r, batch=8",
                   1u << 20,
                   4,
                   items,
                   8,
                   8);

    test_spmc_case("SPMC: 1w4r, batch=128",
                   1u << 20,
                   4,
                   items,
                   128,
                   128);
}

static void test_mpmc(void)
{
    test_mpmc_init_case();

    /*
     * MPMC Benchmark Matrix
     *
     * Parameters:
     *   cap        = ring capacity (power-of-2)
     *   producers  = number of writer threads
     *   consumers  = number of reader threads
     *   items/prod = items written by each producer
     *   wb / rb    = writer batch / reader batch size
     */

    uint64_t items = 64UL * 1024 * 1024;   /* 128M items per producer */

    printf("======================== MPMC PERFORMANCE ===============================\n");

    /* 1. Symmetric scaling: P = C */
    test_mpmc_case("MPMC: 1w1r, batch=32",
                   1u << 20,
                   1, 1,
                   items,
                   32, 32);

    test_mpmc_case("MPMC: 2w2r, batch=32",
                   1u << 20,
                   2, 2,
                   items,
                   32, 32);

    test_mpmc_case("MPMC: 4w4r, batch=32",
                   1u << 20,
                   4, 4,
                   items,
                   32, 32);

    test_mpmc_case("MPMC: 8w8r, batch=32",
                   1u << 20,
                   8, 8,
                   items,
                   32, 32);

    /* 2. Writer-heavy vs reader-heavy */
    test_mpmc_case("MPMC: 8w2r, batch=64",
                   1u << 20,
                   8, 2,
                   items,
                   64, 64);

    test_mpmc_case("MPMC: 2w8r, batch=64",
                   1u << 20,
                   2, 8,
                   items,
                   64, 64);

    /* 3. Batch scaling */
    test_mpmc_case("MPMC: 4w4r, batch=1",
                   1u << 20,
                   4, 4,
                   items,
                   1, 1);

    test_mpmc_case("MPMC: 4w4r, batch=8",
                   1u << 20,
                   4, 4,
                   items,
                   8, 8);

    test_mpmc_case("MPMC: 4w4r, batch=128",
                   1u << 20,
                   4, 4,
                   items,
                   128, 128);

    test_mpmc_case("MPMC: 4w4r, batch=256",
                   1u << 20,
                   4, 4,
                   items,
                   256, 256);

    /* 4. Higher capacity to reduce wrap-around contention */
    test_mpmc_case("MPMC: 4w4r, cap=4M, batch=64",
                   1u << 22,
                   4, 4,
                   items,
                   64, 64);

    test_mpmc_case("MPMC: 4w4r, cap=16M, batch=64",
                   1u << 24,
                   4, 4,
                   items,
                   64, 64);
}

int main(int argc, char *argv[])
{
    test_spsc();
    /*test_mpsc();
    test_spmc();
    test_mpmc();*/

    return EXIT_SUCCESS;
}