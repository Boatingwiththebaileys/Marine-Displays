#ifndef HEAP_DEBUG_H
#define HEAP_DEBUG_H

// Minimal stub for heap_debug.h used by SD_Card.cpp
// Provides no-op helpers so builds that include this header succeed.

#ifdef __cplusplus
extern "C" {
#endif

static inline void heap_debug_mark(const char* tag) { (void)tag; }
static inline void heap_debug_dump(const char* tag) { (void)tag; }

#ifdef __cplusplus
}
#endif

#endif // HEAP_DEBUG_H
