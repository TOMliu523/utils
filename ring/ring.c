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

/*
 * Initialize a Single-Producer / Multi-Consumer ring buffer.
 *
 * @cap:
 *  - Ring capacity (number of elements)
 *  - Must be a power of two for fast index wrap-around
 *
 * Return:
 *  - Pointer to an initialized ring_spmc structure on success
 *  - NULL on invalid parameters or allocation failure
 *
 * Memory layout:
 *  --------------------------------------------------
 *  | struct ring_spmc | data[0] ... data[cap - 1] |
 *  --------------------------------------------------
 *
 * Alignment:
 *  - The entire ring is aligned to CACHE_LINE bytes
 *  - Writer and reader cursors are cache-line aligned to
 *    minimize false sharing between producer and consumers
 *
 * Initial state:
 *  - writer = 0
 *  - reader.head = 0
 *  - reader.tail = 0
 */
struct ring_spmc *ring_spmc_init(int cap)
{
    int ret = 0;
    size_t total = 0;
    struct ring_spmc *ring = NULL;

    /*
     * Validate capacity:
     *  - Must be positive
     *  - Must be a power of two
     */
    if (UNLIKELY(cap <= 0 || (cap & (cap - 1)) != 0)) {
        return NULL;
    }

    /*
     * Allocate a single contiguous memory block:
     *  - ring metadata
     *  - ring data array
     */
    total = sizeof(struct ring_spmc) + cap * sizeof(void *);

    /*
     * Allocate cache-line aligned memory to ensure
     * predictable performance and correct alignment
     * for atomic operations.
     */
    ret = posix_memalign((void **)&ring, CACHE_LINE, total);
    if (UNLIKELY(ret != 0)) {
        return NULL;
    }

    /* Zero-initialize all fields, including data slots */
    memset(ring, 0, total);

    /* Initialize ring parameters */
    ring->cap = cap;
    ring->mask = cap - 1;

    /* Initialize cursors (monotonic counters) */
    ring->writer = 0;
    ring->reader.head = 0;
    ring->reader.tail = 0;

    return ring;
}

/*
 * Destroy a ring_spmc instance.
 *
 * @ring:
 *  - Ring instance returned by ring_spmc_init()
 *
 * Notes:
 *  - Caller must ensure no producer or consumer is
 *    accessing the ring when this function is called
 *  - This function does not perform any synchronization
 */
void ring_spmc_fini(struct ring_spmc *ring)
{
    if (ring == NULL) {
        return;
    }

    free(ring);
}

/*
 * Initialize a Multi-Producer / Single-Consumer (MPSC) ring buffer.
 *
 * Concurrency model:
 *  - Multiple producer threads may concurrently enqueue elements
 *  - Exactly ONE consumer thread may dequeue elements
 *
 * @cap:
 *  - Ring capacity (number of elements)
 *  - Must be a power of two to enable fast index wrap-around
 *
 * Return:
 *  - Pointer to an initialized ring_mpsc instance on success
 *  - NULL on invalid parameters or allocation failure
 *
 * Memory layout:
 *  --------------------------------------------------
 *  | struct ring_mpsc | data[0] ... data[cap - 1] |
 *  --------------------------------------------------
 *
 * Alignment:
 *  - The ring is allocated with CACHE_LINE alignment
 *  - Writer cursors are typically cache-line aligned to
 *    reduce contention between multiple producers
 *
 * Initial state:
 *  - writer.head = 0
 *  - writer.tail = 0
 *  - reader      = 0
 *
 * Notes:
 *  - This function only initializes the ring metadata.
 *  - No synchronization is required during initialization
 *    as the ring is not yet shared with other threads.
 */
struct ring_mpsc *ring_mpsc_init(int cap)
{
    int ret = 0;
    size_t total = 0;
    struct ring_mpsc *ring = NULL;

    /*
     * Validate capacity:
     *  - Must be positive
     *  - Must be a power of two
     */
    if (UNLIKELY(cap <= 0 || (cap & (cap - 1)) != 0)) {
        return NULL;
    }

    /*
     * Allocate a single contiguous memory block containing:
     *  - Ring metadata (cursors and configuration)
     *  - Ring storage array
     */
    total = sizeof(struct ring_mpsc) + cap * sizeof(void *);

    /*
     * Allocate cache-line aligned memory to improve cache locality
     * and to ensure proper alignment for atomic operations.
     */
    ret = posix_memalign((void **)&ring, CACHE_LINE, total);
    if (UNLIKELY(ret != 0)) {
        return NULL;
    }

    /* Zero-initialize all fields, including the ring storage */
    memset(ring, 0, total);

    /* Initialize ring parameters */
    ring->cap = cap;
    ring->mask = cap - 1;

    /*
     * Initialize writer cursors:
     *  - writer.head is typically used by producers to reserve slots
     *  - writer.tail is used to publish committed writes
     */
    ring->writer.head = 0;
    ring->writer.tail = 0;
    /*
     * Initialize reader cursor:
     *  - Only accessed by the single consumer thread
     */
    ring->reader = 0;

    return ring;
}

/*
 * Destroy a ring_mpsc instance.
 *
 * @ring:
 *  - Ring instance returned by ring_mpsc_init()
 *
 * Notes:
 *  - Caller must ensure that no producer or consumer
 *    is accessing the ring when this function is called.
 *  - This function performs no synchronization and
 *    simply releases the allocated memory.
 */
void ring_mpsc_fini(struct ring_mpsc *ring)
{
    if (ring == NULL) {
        return;
    }

    free(ring);
}

/*
 * ring_mpmc_init
 *
 * Allocate and initialize a lock-free MPMC (Multi-Producer / Multi-Consumer)
 * ring buffer instance.
 *
 * Semantics:
 *  - Allocates a single contiguous memory block containing both the ring
 *    control structure and the backing storage for pointer slots.
 *  - Initializes all producer and consumer cursors to zero.
 *
 * Requirements:
 *  - @cap must be a power of two.
 *  - The ring uses monotonic cursors with wrap-around via masking.
 *
 * Memory layout:
 *  - The ring structure ends with a flexible array member `data[]`.
 *  - The total allocation size is:
 *        sizeof(struct ring_mpmc) + cap * sizeof(void *)
 *
 * Thread safety:
 *  - This function is NOT thread-safe.
 *  - It must be called during single-threaded initialization,
 *    before any producer or consumer threads access the ring.
 *
 * Return:
 *  - Pointer to an initialized ring_mpmc instance on success.
 *  - NULL on invalid parameters or allocation failure.
 */
struct ring_mpmc *ring_mpmc_init(int cap)
{
    int ret = 0;
    size_t total = 0;
    struct ring_mpmc *ring = NULL;

    /* Capacity must be a positive power of two */
    if (UNLIKELY(cap <= 0 || (cap & (cap - 1)) != 0)) {
        return NULL;
    }

    /* Allocate space for ring structure + pointer slots */
    total = sizeof(struct ring_mpmc) + cap * sizeof(void *);
    ret = posix_memalign((void **)&ring, CACHE_LINE, total);
    if (UNLIKELY(ret != 0)) {
        return NULL;
    }

    /* Zero-initialize all fields, including cursors and data slots */
    memset(ring, 0, total);

    ring->cap = cap;
    ring->mask = cap - 1;

    /*
     * Initialize producer and consumer cursors.
     * Although memset() already zeroed these fields,
     * they are explicitly assigned here for clarity
     * and future maintainability.
     */
    ring->writer.head = 0;
    ring->writer.tail = 0;
    ring->reader.head = 0;
    ring->reader.tail = 0;

    return ring;
}

/*
 * ring_mpmc_fini
 *
 * Destroy an MPMC ring buffer instance and release its memory.
 *
 * Semantics:
 *  - Frees the memory block allocated by ring_mpmc_init().
 *  - The caller must ensure that no producer or consumer threads
 *    are accessing the ring when this function is called.
 *
 * Thread safety:
 *  - NOT thread-safe.
 *  - Must be called only after all concurrent accesses have stopped.
 */
void ring_mpmc_fini(struct ring_mpmc *ring)
{
    if (ring == NULL) {
        return;
    }

    free(ring);
}