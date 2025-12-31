/*
 * type.h
 *
 * This header defines fundamental and shared data types for the project.
 *
 * Scope:
 *  - Basic typedefs and small helper structs
 *  - Commonly used fixed-layout or semantic types
 *
 * Design rules:
 *  - No macros (except those strictly required for types)
 *  - No function declarations or implementations
 *  - No module- or feature-specific types
 *
 * Types defined here should be stable, minimal, and suitable for use
 * across all layers of the project.
 */

#ifndef __TYPE_H__
#define __TYPE_H__

#include <stdint.h>

/*
 * ring_span
 *
 * Describes a zero-copy readable span returned by ring_spsc_read_start().
 *
 * Purpose:
 *  - Expose the currently available contiguous elements in the ring buffer
 *    without copying data into a temporary array.
 *  - Handles wrap-around by splitting the readable region into up to two parts.
 *
 * Layout:
 *  - The logical readable range is [tail, head).
 *  - Because the ring is circular, this range may wrap around the end of
 *    the underlying array.
 *
 * Fields:
 *  - p1 / count1:
 *      First contiguous segment of readable pointers.
 *      Always points into ring->data at index (tail & mask).
 *
 *  - p2 / count2:
 *      Second contiguous segment (only valid if wrap-around occurs).
 *      Points to ring->data[0].
 *      If no wrap-around is needed, p2 is NULL and count2 is 0.
 *
 *  - total:
 *      Total number of readable elements.
 *      Defined as count1 + count2.
 *
 * Usage contract:
 *  - The consumer must NOT modify ring state between read_start and read_commit.
 *  - After consuming the elements described by this span, the consumer
 *    must call ring_spsc_read_commit() to advance the tail index.
 *
 * Concurrency:
 *  - Valid only for the single consumer thread.
 *  - Data pointed to by p1/p2 is stable until read_commit() is called.
 */
struct ring_span {
    void **p1;        // First contiguous segment of readable entries
    uint32_t count1;  // Number of entries in the first segment

    void **p2;        // Second segment (wrap-around), or NULL if not used
    uint32_t count2;  // Number of entries in the second segment

    uint32_t start;
    uint32_t total;   // Total readable entries: count1 + count2
};

#endif // __TYPE_H__