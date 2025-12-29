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
    if (LIKELY(ret != 0)) {
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