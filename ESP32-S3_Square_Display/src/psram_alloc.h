#pragma once
// psram_alloc.h — put large tables in PSRAM instead of internal RAM.
//
// EXT_RAM_ATTR does nothing on this framework (the pre-built Arduino libraries
// are compiled without CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY), so tables
// marked with it stayed in internal RAM. Internal RAM is what WiFi, TCP and
// the web server run short of, so tables only the app uses are allocated from
// PSRAM at start-up instead. PSRAM is added to the heap before C++ static
// initialisers run, so these can be used as global initialisers.
//
//   static char (*text)[64] = psram_array<char[64]>(NUM_SCREENS);   // text[s][i]
//
// Memory is zeroed, like a normal global. If PSRAM can't be had, it falls back
// to internal RAM so the display still works (psram_report() logs where each
// table went).

#include <esp_heap_caps.h>
#include <soc/soc_memory_layout.h>
#include <stdlib.h>
#include <stdio.h>

template <typename T>
static inline T* psram_array(size_t n) {
    void* p = heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(n, sizeof(T));
    return static_cast<T*>(p);
}

static inline void psram_report(const char* name, const void* p, size_t bytes) {
    printf("[MEM] %-22s %6u bytes in %s\n", name, (unsigned)bytes,
           !p ? "NOWHERE (allocation failed)" : esp_ptr_external_ram(p) ? "PSRAM" : "internal RAM");
}
