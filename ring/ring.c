/*
 * ring.c
 *
 * Lock-free ring buffer implementations.
 *
 * This file contains the concrete implementations of the lock-free ring
 * buffers declared in ring.h. It includes initialization, destruction,
 * and core enqueue/dequeue logic.
 *
 * Implementation details:
 *  - Uses atomic operations with explicit memory ordering.
 *  - Avoids locks and blocking synchronization primitives.
 *  - Optimized for cache efficiency and predictable latency.
 *
 * This file is internal to the ring subsystem and should not be included
 * directly by users of the API.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ring.h"

/*
 * ring_spsc_init
 *
 * Allocate and initialize a lock-free SPSC (Single Producer / Single Consumer)
 * ring buffer.
 *
 * Parameters:
 *  - cap:
 *      The capacity of the ring buffer. Must be a power of two and greater
 *      than zero. Power-of-two capacity enables efficient index wrap-around
 *      using bit masking.
 *
 * Returns:
 *  - On success:
 *      A pointer to a newly allocated and initialized ring_spsc structure.
 *  - On failure:
 *      NULL is returned if:
 *        - cap is invalid (<= 0 or not a power of two)
 *        - memory allocation fails
 *
 * Memory layout:
 *  - The ring structure and the data array are allocated as a single
 *    contiguous memory block.
 *  - The allocation is cache-line aligned to reduce false sharing
 *    and improve cache locality.
 *
 * Initialization:
 *  - All fields are zero-initialized.
 *  - writer and reader indices start at zero.
 *  - mask is set to (cap - 1) for fast modulo operations.
 *
 * Thread-safety:
 *  - This function is NOT thread-safe and must be called during
 *    single-threaded initialization.
 */
struct ring_spsc *ring_spsc_init(int cap)
{
    int ret = 0;
    size_t total = 0;
    struct ring_spsc *ring = NULL;

    if (UNLIKELY(cap <= 0 || (cap & (cap - 1)) != 0)) {
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

    return ring;
}

/*
 * ring_spsc_fini
 *
 * Destroy and free an SPSC ring buffer.
 *
 * Parameters:
 *  - ring:
 *      Pointer to a ring_spsc structure previously created by
 *      ring_spsc_init().
 *
 * Semantics:
 *  - Frees all memory associated with the ring buffer.
 *  - Safe to call with a NULL pointer (no operation is performed).
 *
 * Thread-safety:
 *  - This function is NOT thread-safe.
 *  - The caller must ensure that no producer or consumer is accessing
 *    the ring when this function is called.
 */
void ring_spsc_fini(struct ring_spsc *ring)
{
    if (ring == NULL) {
        return;
    }

    free(ring);
}