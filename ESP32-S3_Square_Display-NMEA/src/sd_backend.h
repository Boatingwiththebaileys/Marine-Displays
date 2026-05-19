#pragma once

#include <sys/stat.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SD_BACKEND_NONE = 0,
  SD_BACKEND_SDSPI = 1,
  SD_BACKEND_SDMMC = 2,
} sd_backend_t;

static inline void sd_backend_set(sd_backend_t b) {
  (void)b; // placeholder: store if needed later
}

static inline sd_backend_t sd_backend_get(void) {
  return SD_BACKEND_NONE;
}

// Simple existence check using POSIX stat. Returns true if path exists.
static inline bool sd_exists(const char *path) {
  if (!path) return false;
  struct stat st;
  return (stat(path, &st) == 0);
}

#ifdef __cplusplus
}
#endif
