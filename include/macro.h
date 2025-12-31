/*
 * macro.h
 *
 * Internal macro definitions.
 *
 * This header contains **only low-level, foundational macros** that are
 * shared across the entire codebase, such as:
 *
 *  - Utility macros (MIN, MAX, ARRAY_SIZE, etc.)
 *  - Compiler / platform helpers (INLINE, ALIGNED, LIKELY, UNLIKELY)
 *  - Cache-line / alignment related constants
 *  - Debug / assertion helpers used globally
 *
 * Design rules:
 *  - NO business logic macros.
 *  - NO module-specific macros.
 *  - Macros must be generic, stable, and widely reusable.
 *  - Keep dependencies minimal; this file should be safe to include anywhere.
 *
 * This header is intended to be included early and broadly.
 */

#ifndef __MACRO_H__
#define __MACRO_H__

#ifndef MIN
#define MIN(x, y) \
    ({ \
        typeof(x) _x = (x); \
        typeof(y) _y = (y); \
        _x > _y ? _y : _x; \
    })
#endif // MIN

#ifndef MAX
#define MAX(x, y) \
    ({ \
        typeof(x) _x = (x); \
        typeof(y) _y = (y); \
        _x > _y ? _x : _y; \
    })
#endif // MAX

#ifndef ALIGNED
#define ALIGNED(n) __attribute__((aligned(n)))
#endif // ALIGNED

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif // CACHE_LINE

#ifndef INLINE
#define INLINE inline __attribute__((always_inline))
#endif //INLINE

#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#endif // LIKELY

#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif // LIKELY

#endif // __MACRO_H__