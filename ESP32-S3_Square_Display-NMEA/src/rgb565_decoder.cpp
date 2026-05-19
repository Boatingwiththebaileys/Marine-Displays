#include "rgb565_decoder.h"
#include "lvgl.h"
#include <string.h>
#include "esp_heap_caps.h"

/**
 * Custom decoder for raw RGB565 binary files
 * File format: Variable size, 2 bytes per pixel (RGB565), little-endian
 * No header, just raw pixel data
 *
 * PSRAM-backed image cache: once a .bin file is read from SD it is kept in
 * PSRAM for the life of the device.  Subsequent decoder_open() calls (e.g.
 * on every screen scroll) return the cached pointer without touching SD at
 * all, eliminating sdmmc_read_blocks errors caused by WiFi/SD DMA contention.
 */

// ── PSRAM persistent image cache ─────────────────────────────────────────────
#define IMG_CACHE_MAX 32   // up to 32 unique .bin paths cached

struct ImgCacheEntry {
    char         path[128]; // lv_fs path e.g. "S:/assets/bg.bin"
    const uint8_t* data;
    size_t       size;
};

static ImgCacheEntry s_img_cache[IMG_CACHE_MAX];
static int           s_img_cache_cnt = 0;

static const uint8_t* cache_find(const char* path) {
    for (int i = 0; i < s_img_cache_cnt; i++) {
        if (strncmp(s_img_cache[i].path, path, 127) == 0)
            return s_img_cache[i].data;
    }
    return nullptr;
}

// Returns size of a cached entry, or 0 if not cached.
static size_t cache_find_size(const char* path) {
    for (int i = 0; i < s_img_cache_cnt; i++) {
        if (strncmp(s_img_cache[i].path, path, 127) == 0)
            return s_img_cache[i].size;
    }
    return 0;
}

// Store a newly allocated buffer in the cache. Returns the same pointer.
static const uint8_t* cache_store(const char* path, const uint8_t* data, size_t size) {
    if (s_img_cache_cnt >= IMG_CACHE_MAX) {
        LV_LOG_WARN("rgb565 cache full (%d/%d), %s will not be cached",
                    s_img_cache_cnt, IMG_CACHE_MAX, path);
        return data; // still usable, just won't be cached
    }
    ImgCacheEntry& e = s_img_cache[s_img_cache_cnt++];
    strncpy(e.path, path, 127);
    e.path[127] = '\0';
    e.data = data;
    e.size = size;
    LV_LOG_INFO("rgb565 cache[%d]: stored %s (%u bytes)", s_img_cache_cnt - 1, path, (unsigned)size);
    return data;
}

// Returns true if ptr is owned by the persistent cache (must NOT be freed).
static bool cache_owns(const uint8_t* ptr) {
    for (int i = 0; i < s_img_cache_cnt; i++) {
        if (s_img_cache[i].data == ptr) return true;
    }
    return false;
}
// ─────────────────────────────────────────────────────────────────────────────

static lv_res_t decoder_info(lv_img_decoder_t * decoder, const void * src, lv_img_header_t * header)
{
    (void) decoder;
    
    // Check if it's a .bin file path
    if(lv_img_src_get_type(src) == LV_IMG_SRC_FILE) {
        const char * fn = (const char *)src;
        if(strstr(fn, ".bin") != NULL) {
            uint32_t file_size = 0;

            // ── Check PSRAM cache first — avoids SD open every frame ──────
            size_t cached_size = cache_find_size(fn);
            if (cached_size > 0) {
                file_size = (uint32_t)cached_size;
            } else {
                // Not cached yet — open file to get size (only happens once)
                lv_fs_file_t f;
                lv_fs_res_t res = lv_fs_open(&f, fn, LV_FS_MODE_RD);
                if(res != LV_FS_RES_OK) {
                    return LV_RES_INV;
                }
                lv_fs_seek(&f, 0, LV_FS_SEEK_END);
                lv_fs_tell(&f, &file_size);
                lv_fs_close(&f);
            }
            // ─────────────────────────────────────────────────────────────

            // Calculate dimensions (file is width*height*2 bytes)
            uint32_t pixel_count = file_size / 2;
            uint32_t dimension = 1;
            while(dimension * dimension < pixel_count) dimension++;
            
            // It's a binary RGB565 file
            header->cf = LV_IMG_CF_TRUE_COLOR;  // RGB565 format
            header->w = dimension;  // Width from file size
            header->h = dimension;  // Height (square image)
            return LV_RES_OK;
        }
    }
    
    return LV_RES_INV;  // Not a .bin file
}

static lv_res_t decoder_open(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{
    (void) decoder;
    
    // Check if it's a .bin file
    if(lv_img_src_get_type(dsc->src) == LV_IMG_SRC_FILE) {
        const char * fn = (const char *)dsc->src;
        if(strstr(fn, ".bin") != NULL) {

            // ── Check PSRAM cache first — avoids SD read ──────────────────
            const uint8_t* cached = cache_find(fn);
            if (cached) {
                dsc->img_data = cached;
                return LV_RES_OK;
            }
            // ─────────────────────────────────────────────────────────────

            // Open file to get actual size
            lv_fs_file_t f;
            lv_fs_res_t res = lv_fs_open(&f, fn, LV_FS_MODE_RD);
            if(res != LV_FS_RES_OK) {
                LV_LOG_ERROR("Failed to open file: %s", fn);
                return LV_RES_INV;
            }
            
            // Get file size
            uint32_t file_size;
            lv_fs_seek(&f, 0, LV_FS_SEEK_END);
            lv_fs_tell(&f, &file_size);
            lv_fs_seek(&f, 0, LV_FS_SEEK_SET);  // Reset to beginning
            
            // Allocate buffer — prefer PSRAM so it survives in the cache
            uint8_t* buf = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) {
                // Fallback to internal DRAM (unlikely to be available for large images)
                buf = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_8BIT);
            }
            if (!buf) {
                LV_LOG_ERROR("Failed to allocate %u bytes for %s", (unsigned)file_size, fn);
                lv_fs_close(&f);
                return LV_RES_INV;
            }
            
            // Read in chunks to handle short DMA reads when iRAM is tight
            // (e.g. during SSL connections that consume internal DMA buffers)
            uint32_t total_read = 0;
            const uint32_t CHUNK = 8192;
            while (total_read < file_size) {
                uint32_t to_read = file_size - total_read;
                if (to_read > CHUNK) to_read = CHUNK;
                uint32_t chunk_read = 0;
                res = lv_fs_read(&f, buf + total_read, to_read, &chunk_read);
                if (res != LV_FS_RES_OK || chunk_read == 0) break;
                total_read += chunk_read;
            }
            lv_fs_close(&f);
            
            if(total_read != file_size) {
                LV_LOG_ERROR("Failed to read file data: read %d of %d bytes", total_read, file_size);
                heap_caps_free(buf);
                return LV_RES_INV;
            }
            
            LV_LOG_INFO("Loaded RGB565 binary: %s (%d bytes)", fn, file_size);

            // Store in persistent PSRAM cache so future opens skip SD entirely
            dsc->img_data = cache_store(fn, buf, file_size);
            return LV_RES_OK;
        }
    }
    
    return LV_RES_INV;
}

static void decoder_close(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{
    (void) decoder;
    
    // Only free if the buffer is NOT owned by the persistent PSRAM cache.
    // Cached buffers live for the life of the device — freeing them would
    // cause a dangling pointer on next cache_find() hit.
    if(dsc->img_data && !cache_owns(dsc->img_data)) {
        heap_caps_free((void*)dsc->img_data);
        dsc->img_data = NULL;
    }
}

void rgb565_decoder_init(void)
{
    lv_img_decoder_t * dec = lv_img_decoder_create();
    lv_img_decoder_set_info_cb(dec, decoder_info);
    lv_img_decoder_set_open_cb(dec, decoder_open);
    lv_img_decoder_set_close_cb(dec, decoder_close);
    
    LV_LOG_INFO("RGB565 binary decoder initialized");
}

void rgb565_preload_file(const char* lv_path)
{
    if (!lv_path || lv_path[0] == '\0') return;
    if (!strstr(lv_path, ".bin")) return;
    if (cache_find(lv_path)) {
        LV_LOG_INFO("rgb565 preload: already cached %s", lv_path);
        return;
    }

    lv_fs_file_t f;
    if (lv_fs_open(&f, lv_path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        LV_LOG_WARN("rgb565 preload: cannot open %s", lv_path);
        return;
    }

    uint32_t file_size = 0;
    lv_fs_seek(&f, 0, LV_FS_SEEK_END);
    lv_fs_tell(&f, &file_size);
    lv_fs_seek(&f, 0, LV_FS_SEEK_SET);

    uint8_t* buf = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_8BIT);
    if (!buf) {
        LV_LOG_WARN("rgb565 preload: alloc failed for %s (%u bytes)", lv_path, (unsigned)file_size);
        lv_fs_close(&f);
        return;
    }

    uint32_t bytes_read = 0;
    lv_fs_res_t res = lv_fs_read(&f, buf, file_size, &bytes_read);
    lv_fs_close(&f);

    if (res != LV_FS_RES_OK || bytes_read != file_size) {
        LV_LOG_WARN("rgb565 preload: read failed %s (%u/%u bytes)", lv_path, (unsigned)bytes_read, (unsigned)file_size);
        heap_caps_free(buf);
        return;
    }

    cache_store(lv_path, buf, file_size);
    LV_LOG_INFO("rgb565 preloaded %s (%u bytes into PSRAM)", lv_path, (unsigned)file_size);
}
