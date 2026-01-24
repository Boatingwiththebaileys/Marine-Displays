#include "rgb565_decoder.h"
#include "lvgl.h"
#include <string.h>

/**
 * Custom decoder for raw RGB565 binary files
 * File format: Variable size, 2 bytes per pixel (RGB565), little-endian
 * No header, just raw pixel data
 */

static lv_res_t decoder_info(lv_img_decoder_t * decoder, const void * src, lv_img_header_t * header)
{
    (void) decoder;
    
    // Check if it's a .bin file path
    if(lv_img_src_get_type(src) == LV_IMG_SRC_FILE) {
        const char * fn = (const char *)src;
        if(strstr(fn, ".bin") != NULL) {
            // Open file to get size
            lv_fs_file_t f;
            lv_fs_res_t res = lv_fs_open(&f, fn, LV_FS_MODE_RD);
            if(res != LV_FS_RES_OK) {
                return LV_RES_INV;
            }
            
            // Get file size
            uint32_t file_size;
            lv_fs_seek(&f, 0, LV_FS_SEEK_END);
            lv_fs_tell(&f, &file_size);
            lv_fs_close(&f);
            
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
            
            // Allocate buffer in PSRAM
            dsc->img_data = (const uint8_t*)lv_mem_alloc(file_size);
            if(dsc->img_data == NULL) {
                LV_LOG_ERROR("Failed to allocate memory for RGB565 image (%d bytes)", file_size);
                lv_fs_close(&f);
                return LV_RES_INV;
            }
            
            // Read all data at once
            uint32_t bytes_read;
            res = lv_fs_read(&f, (void*)dsc->img_data, file_size, &bytes_read);
            lv_fs_close(&f);
            
            if(res != LV_FS_RES_OK || bytes_read != file_size) {
                LV_LOG_ERROR("Failed to read file data: read %d of %d bytes", bytes_read, file_size);
                lv_mem_free((void*)dsc->img_data);
                return LV_RES_INV;
            }
            
            LV_LOG_INFO("Loaded RGB565 binary: %s (%d bytes)", fn, file_size);
            return LV_RES_OK;
        }
    }
    
    return LV_RES_INV;
}

static void decoder_close(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{
    (void) decoder;
    
    // Free the allocated buffer
    if(dsc->img_data) {
        lv_mem_free((void*)dsc->img_data);
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
