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

#include "ring.h"

#define DATA_MAX 1024 * 1024 * 2
#define NS 1000000000UL

static uint64_t *sp_send_arr;
static uint64_t *sp_recv_arr;

static void *task_send(void *arg)
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
    printf("Writer %d items each time, with a data size of %d.: %llus%llu\n", 1, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void *task_recv(void *arg)
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
    printf("Reader %d items each time, with a data size of %d.: %llus%llu\n", 1, DATA_MAX, t / NS, t % NS);

    pthread_exit(NULL);
}

static void test_one(void)
{
    pthread_t id1 = 0;
    pthread_t id2 = 0;

    struct ring_spsc *ring = ring_spsc_init(1024 * 64);
    sp_send_arr = malloc(sizeof(uint64_t) * DATA_MAX);
    sp_recv_arr = malloc(sizeof(uint64_t) * DATA_MAX);

    pthread_create(&id1, NULL, task_send, ring);
    pthread_create(&id2, NULL, task_recv, ring);

    pthread_join(id1, NULL);
    pthread_join(id2, NULL);

    for (int i = 0; i < DATA_MAX; i++) {
        if (sp_send_arr[i] != sp_recv_arr[i]) {
            fprintf(stderr, "Process error(%d: %llu, %llu).!\n", i, sp_send_arr[i], sp_recv_arr[i]);
            return;
        }
    }

    printf("Test one write one read success!\n");
}

int main(int argc, char *argv[])
{
    test_one();

    return EXIT_SUCCESS;
}
