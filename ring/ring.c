/******************************************************************************************
 * Lock-free SPSC ring (Single Producer / Single Consumer)
 * Lock-free MPMC ring (Multi Producer / Multi Consumer)
 * Lock-free ring: Single Writer + Multiple Readers with ordered consumption (SPMR-Ordered)
 ******************************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "ring.h"

struct ring_spsc *ring_spsc_init(int cap)
{
    int ret = 0;
    size_t total = 0;
    struct ring_spsc *ring = NULL;

    if (UNLIKELY((cap <= 0) || ((cap & (cap - 1)) != 0))) {
        return NULL;
    }

    total = sizeof(struct ring_spsc) + cap * sizeof(void *);
    ret = posix_memalign((void **)&ring, CACHE_LINE, total);
    if (UNLIKELY(ret != 0)) {
        return NULL;
    }

    memset(ring, 0, total);

    ring->cap = cap;
    ring->mask = cap - 1;
    ring->head = 0;
    ring->tail = 0;

    return ring;
}

void ring_spsc_fini(struct ring_spsc *ring)
{
    if (UNLIKELY(ring == NULL)) {
        return;
    }

    free(ring);
}

struct ring_mpmc *ring_mpmc_init(int cap)
{
    int ret = 0;
    uint64_t total = 0;
    struct ring_mpmc *ring = NULL;

    if (UNLIKELY((cap <= 0) || (cap & (cap - 1)) != 0)) {
        return NULL;
    }

    total = sizeof(struct ring_mpmc) + sizeof(void *) * cap;
    ret = posix_memalign((void **)&ring, CACHE_LINE, total);
    if (UNLIKELY(ret != 0)) {
        return NULL;
    }

    memset(ring, 0, total);

    ring->cap = cap;
    ring->mask = cap - 1;

    return ring;
}

void ring_mpmc_fini(struct ring_mpmc *ring)
{
    if (ring == NULL) {
        return;
    }

    free(ring);
}

struct ring_spmro *ring_spmro_init(int cap, int role_nums)
{
    int ret = 0;
    size_t total = 0;
    struct ring_spmro *ring = NULL;

    if (UNLIKELY((cap <= 0) || ((cap & (cap - 1)) != 0) || role_nums <= 0)) {
        return NULL;
    }

    total = sizeof(struct ring_spmro) + sizeof(void *) * cap;
    ret = posix_memalign((void **)&ring, CACHE_LINE, total);
    if (UNLIKELY(ret != 0)) {
        return NULL;
    }

    memset(ring, 0, total);

    ret = posix_memalign((void **)&ring->roles, CACHE_LINE, role_nums * sizeof(struct role));
    if (UNLIKELY(ret != 0)) {
        free(ring);
        return NULL;
    }

    memset(ring->roles, 0, role_nums * sizeof(struct role));

    ring->cap = cap;
    ring->mask = cap - 1;
    ring->n_role = role_nums;

    return ring;
}

void ring_spmro_fini(struct ring_spmro *ring)
{
    if (ring == NULL) {
        return;
    }

    free(ring->roles);
    free(ring);
}