/******************************************************************************************
 * test
 ******************************************************************************************/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <inttypes.h>

#include "ring.h"

#define DATA_MAX 1024 * 1024 * 256
#define NS 1000000000UL

static uint64_t *sp_send_arr;
static uint64_t *sp_recv_arr;

static void *task_send_one(void *arg)
{
    int n = 0;
    uint64_t t = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < DATA_MAX; i++) {
        do {
            n = ring_spsc_write(ring, (void **)&sp_send_arr[i], 1);
        } while (n != 1);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Writer %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 1, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void *task_recv_one(void *arg)
{
    int n = 0;
    int total = 0;
    uint64_t t = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    clock_gettime(CLOCK_MONOTONIC, &start);

    do {
        n = ring_spsc_read(ring, (void **)&sp_recv_arr[total], 1);
        if (n != 0) {
            total += n;
        }
    } while (total != DATA_MAX);

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Reader %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 1, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void test_one(void)
{
    pthread_t id1 = 0;
    pthread_t id2 = 0;

    struct ring_spsc *ring = ring_spsc_init(1024 * 64);
    pthread_create(&id1, NULL, task_send_one, ring);
    pthread_create(&id2, NULL, task_recv_one, ring);

    pthread_join(id1, NULL);
    pthread_join(id2, NULL);

    for (int i = 0; i < DATA_MAX; i++) {
        if (sp_send_arr[i] != sp_recv_arr[i]) {
            fprintf(stderr, "Process error(%d: %" PRIu64 ", %" PRIu64 ").!\n", i, sp_send_arr[i], sp_recv_arr[i]);
            ring_spsc_fini(ring);
            return;
        }
    }

    ring_spsc_fini(ring);
    printf("Test one write one read success!\n");
}

static void *task_send_ten(void *arg)
{
    int n = 0;
    uint64_t t = 0;
    uint32_t max = 0;
    uint64_t total = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (total = 0; total < DATA_MAX; ) {
        n = ring_spsc_write(ring, (void **)&sp_send_arr[total], max);
        total += n;
        max = MIN((DATA_MAX - total), 16);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Writer %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 16, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void *task_recv_ten(void *arg)
{
    int n = 0;
    int total = 0;
    uint64_t t = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    clock_gettime(CLOCK_MONOTONIC, &start);

    do {
        n = ring_spsc_read(ring, (void **)&sp_recv_arr[total], 16);
        if (n != 0) {
            total += n;
        }
    } while (total != DATA_MAX);

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Reader %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 16, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void test_ten(void)
{
    pthread_t id1 = 0;
    pthread_t id2 = 0;

    struct ring_spsc *ring = ring_spsc_init(1024 * 64);
    pthread_create(&id1, NULL, task_send_ten, ring);
    pthread_create(&id2, NULL, task_recv_ten, ring);

    pthread_join(id1, NULL);
    pthread_join(id2, NULL);

    for (int i = 0; i < DATA_MAX; i++) {
        if (sp_send_arr[i] != sp_recv_arr[i]) {
            fprintf(stderr, "Process error(%d: %" PRIu64 ", %" PRIu64 ").!\n", i, sp_send_arr[i], sp_recv_arr[i]);
            ring_spsc_fini(ring);
            return;
        }
    }

    ring_spsc_fini(ring);
    printf("Test ten write ten read success!\n");
}

static void *task_send_hundred(void *arg)
{
    int n = 0;
    uint64_t t = 0;
    uint32_t max = 0;
    uint64_t total = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (total = 0; total < DATA_MAX; ) {
        n = ring_spsc_write(ring, (void **)&sp_send_arr[total], max);
        total += n;
        max = MIN((DATA_MAX - total), 100);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Writer %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 100, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void *task_recv_hundred(void *arg)
{
    int n = 0;
    int total = 0;
    uint64_t t = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    clock_gettime(CLOCK_MONOTONIC, &start);

    do {
        n = ring_spsc_read(ring, (void **)&sp_recv_arr[total], 100);
        if (n != 0) {
            total += n;
        }
    } while (total != DATA_MAX);

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Reader %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 100, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void test_hundred(void)
{
    pthread_t id1 = 0;
    pthread_t id2 = 0;

    struct ring_spsc *ring = ring_spsc_init(1024 * 64);
    pthread_create(&id1, NULL, task_send_hundred, ring);
    pthread_create(&id2, NULL, task_recv_hundred, ring);

    pthread_join(id1, NULL);
    pthread_join(id2, NULL);

    for (int i = 0; i < DATA_MAX; i++) {
        if (sp_send_arr[i] != sp_recv_arr[i]) {
            fprintf(stderr, "Process error(%d: %" PRIu64 ", %" PRIu64 ").!\n", i, sp_send_arr[i], sp_recv_arr[i]);
            ring_spsc_fini(ring);
            return;
        }
    }

    ring_spsc_fini(ring);
    printf("Test ten write ten read success!\n");
}

static void *task_send_max(void *arg)
{
    int n = 0;
    uint64_t t = 0;
    uint32_t max = 0;
    uint64_t total = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (total = 0; total < DATA_MAX; ) {
        n = ring_spsc_write(ring, (void **)&sp_send_arr[total], max);
        total += n;
        max = MIN((DATA_MAX - total), 1024 * 64);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Writer %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 1024 * 64, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void *task_recv_max(void *arg)
{
    int n = 0;
    int total = 0;
    uint64_t t = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    clock_gettime(CLOCK_MONOTONIC, &start);

    do {
        n = ring_spsc_read(ring, (void **)&sp_recv_arr[total], 1024 * 64);
        if (n != 0) {
            total += n;
        }
    } while (total != DATA_MAX);

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Reader %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 1024 * 64, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void test_max(void)
{
    pthread_t id1 = 0;
    pthread_t id2 = 0;

    struct ring_spsc *ring = ring_spsc_init(1024 * 64);

    pthread_create(&id1, NULL, task_send_max, ring);
    pthread_create(&id2, NULL, task_recv_max, ring);

    pthread_join(id1, NULL);
    pthread_join(id2, NULL);

    for (int i = 0; i < DATA_MAX; i++) {
        if (sp_send_arr[i] != sp_recv_arr[i]) {
            fprintf(stderr, "Process error(%d: %" PRIu64 ", %" PRIu64 ").!\n", i, sp_send_arr[i], sp_recv_arr[i]);
            ring_spsc_fini(ring);
            return;
        }
    }

    ring_spsc_fini(ring);
    printf("Test ten write ten read success!\n");
}

static void rand_init(int arr[], int count, int max)
{
    for (int i = 0; i < count; i++) {
        arr[i] = rand() % max;
    }
}

static void rand64_init(uint64_t arr[], int count, uint64_t max)
{
    for (int i = 0; i < count; i++) {
        arr[i] = rand() % max;
    }
}

static void *task_send_rand(void *arg)
{
    int i = 0;
    int n = 0;
    uint64_t t = 0;
    uint32_t max = 0;
    uint64_t total = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    int write_rand_arr[32] = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    rand_init(write_rand_arr, sizeof(write_rand_arr) / sizeof(write_rand_arr[0]), 1024);
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (total = 0; total < DATA_MAX; ) {
        n = ring_spsc_write(ring, (void **)&sp_send_arr[total], max);
        total += n;
        max = MIN((DATA_MAX - total), write_rand_arr[i % 32]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Writer %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 1024 * 64, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void *task_recv_rand(void *arg)
{
    int n = 0;
    int total = 0;
    uint64_t t = 0;
    uint32_t i = 0;
    uint32_t max = 0;
    struct timespec end = {0};
    struct timespec start = {0};
    int read_rand_arr[32] = {0};
    struct ring_spsc *ring = (struct ring_spsc *)arg;

    rand_init(read_rand_arr, sizeof(read_rand_arr) / sizeof(read_rand_arr[0]), 1024);

    clock_gettime(CLOCK_MONOTONIC, &start);

    do {
        n = ring_spsc_read(ring, (void **)&sp_recv_arr[total], read_rand_arr[i % 32]);
        if (n != 0) {
            total += n;
        }
    } while (total != DATA_MAX);

    clock_gettime(CLOCK_MONOTONIC, &end);

    t = (end.tv_sec * NS + end.tv_nsec) - (start.tv_sec * NS - start.tv_nsec);
    printf("Reader %d items each time, with a data size of %d.: %" PRIu64 "s%" PRIu64 "\n", 1024 * 64, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void test_rand(void)
{
    pthread_t id1 = 0;
    pthread_t id2 = 0;

    struct ring_spsc *ring = ring_spsc_init(1024 * 64);
    pthread_create(&id1, NULL, task_send_rand, ring);
    pthread_create(&id2, NULL, task_recv_rand, ring);

    pthread_join(id1, NULL);
    pthread_join(id2, NULL);

    for (int i = 0; i < DATA_MAX; i++) {
        if (sp_send_arr[i] != sp_recv_arr[i]) {
            fprintf(stderr, "Process error(%d: %" PRIu64 ", %" PRIu64 ").!\n", i, sp_send_arr[i], sp_recv_arr[i]);
            ring_spsc_fini(ring);
            return;
        }
    }

    ring_spsc_fini(ring);
    printf("Test ten write ten read success!\n");
}

int main(int argc, char *argv[])
{
    srand(time(NULL));

    sp_send_arr = malloc(sizeof(uint64_t) * DATA_MAX);
    rand64_init(sp_send_arr, DATA_MAX, DATA_MAX);

    sp_recv_arr = malloc(sizeof(uint64_t) * DATA_MAX);

    memset(sp_recv_arr, 0, sizeof(uint64_t) * DATA_MAX);

    test_one();
    memset(sp_recv_arr, 0, sizeof(uint64_t) * DATA_MAX);
    test_ten();
    memset(sp_recv_arr, 0, sizeof(uint64_t) * DATA_MAX);
    test_hundred();
    memset(sp_recv_arr, 0, sizeof(uint64_t) * DATA_MAX);
    test_max();
    memset(sp_recv_arr, 0, sizeof(uint64_t) * DATA_MAX);
    test_rand();
    memset(sp_recv_arr, 0, sizeof(uint64_t) * DATA_MAX);

    free(sp_send_arr);
    free(sp_recv_arr);

    return EXIT_SUCCESS;
}
