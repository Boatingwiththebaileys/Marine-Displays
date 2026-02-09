#include "SD_Card.h"
#include "sd_backend.h"
#include "esp_log.h"
#include "heap_debug.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/i2c.h"
#include "Display_ST7701.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

bool SDCard_Flag;
bool SDCard_Finish;

// Controls whether we attempt to scan /sdcard/assets and use SD-based images.
// Default: follow the Arduino demo and allow SD asset scanning at boot so the
// display and examples that rely on SD files behave as expected.
// NOTE: if SD media is flaky on your hardware you can set this to false to
// avoid touching the card during early boot.
bool SD_AllowAssetScan = true;

uint16_t SDCard_Size;
uint16_t Flash_Size;

// Last asset path found or created by EnsureAssetFilename. Empty when none.
char LastFoundAssetPath[128] = {0};

// Pointer to mounted sdmmc card used by monitor/unmount helpers
static sdmmc_card_t *s_sd_card = NULL;

// helper: map a logical path to VFS path under /sdcard
static void make_vfs_path(const char* in, char* out, size_t outlen) {
  if (!in || !out) return;
  if (strncmp(in, "/sdcard", 7) == 0) {
    strncpy(out, in, outlen-1);
    out[outlen-1] = '\0';
    return;
  }
  if (in[0] == '/') {
    snprintf(out, outlen, "/sdcard%s", in);
    return;
  }
  snprintf(out, outlen, "/sdcard/%s", in);
}

void SD_D3_Dis(){
  Set_EXIO(EXIO_PIN4,Low);
  vTaskDelay(pdMS_TO_TICKS(10));
}
void SD_D3_EN(){
  Set_EXIO(EXIO_PIN4,High);
  vTaskDelay(pdMS_TO_TICKS(10));
}
esp_err_t SD_Init() {
  // SD initialization: follow the Arduino demo's SD_MMC flow exactly.
  SDCard_Flag = false;
  SDCard_Finish = false;

  printf("SD_Init: Arduino-demo SD_MMC begin sequence\n");

  // Enable verbose logs for SD/MMC to aid debugging
  esp_log_level_set("sdmmc", ESP_LOG_DEBUG);

  // Do *not* perform SDSPI or pre-mount I2C writes here — match Arduino demo ordering.
  // Configure SDMMC pins and call SD_MMC.begin just like the Arduino demo.
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
  vTaskDelay(pdMS_TO_TICKS(10));

  printf("SD_Init: calling SD_MMC.begin('/sdcard', true)\n");
  if (SD_MMC.begin("/sdcard", true)) {
    printf("SD_Init: SD_MMC begin succeeded\n");
    sd_backend_set(SD_BACKEND_SDMMC);
    SDCard_Flag = true;
    SDCard_Finish = true;
    // Note: SD_MMC.begin doesn't return an sdmmc_card_t pointer here; the SD_MMC
    // Arduino wrapper manages it. Monitor/unmount via SD_MMC.end() when needed.
  } else {
    printf("SD_Init: SD_MMC begin failed\n");
    SD_D3_Dis();
    sd_backend_set(SD_BACKEND_NONE);
    SDCard_Flag = false;
    SDCard_Finish = true;
    return ESP_ERR_TIMEOUT;
  }

  // Create a simple FreeRTOS task to monitor SD health (same as before)
  static bool sd_monitor_task_created = false;
  if (!sd_monitor_task_created) {
    sd_monitor_task_created = true;
    xTaskCreate([](void* pv) {
      int consecutive_failures = 0;
      const int failure_threshold = 2; // two consecutive failures -> disable
      for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        // Quick health check: ensure /sdcard exists and we can stat it
        if (!sd_exists("/sdcard")) {
          consecutive_failures++;
          printf("SD_Monitor: /sdcard health check failed (count=%d)\n", consecutive_failures);
        } else {
          if (consecutive_failures > 0) {
            printf("SD_Monitor: /sdcard health restored\n");
          }
          consecutive_failures = 0;
        }

        if (consecutive_failures >= failure_threshold) {
          printf("SD_Monitor: SD failure threshold reached; disabling SD\n");
          // Attempt to cleanly disable SD_MMC
          SD_MMC.end();
          SD_D3_Dis();
          sd_backend_set(SD_BACKEND_NONE);
          SDCard_Flag = false;
          // Keep monitoring disabled after action
          break;
        }
      }
      vTaskDelete(NULL);
    }, "sd_monitor", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
  }

  return ESP_OK;
}

// Attempt a conservative recovery if SD reads fail after panel takeover.
void SD_RecoveryCheck(void) {
  if (!SDCard_Flag) return;
  struct stat st;
  if (stat("/sdcard", &st) == 0) {
    // basic health check ok
    return;
  }

  printf("SD_Recovery: /sdcard inaccessible -> attempting recovery\n");
  // Try a conservative re-init: end and begin the SD_MMC wrapper, ensuring CS
  // is released so the card doesn't see spurious SPI traffic during panel activity.
  SD_MMC.end();
  vTaskDelay(pdMS_TO_TICKS(10));

  // Ensure SD CS is released (expander-driven) before attempting re-init
  SD_D3_EN();
  vTaskDelay(pdMS_TO_TICKS(10));

  if (SD_MMC.begin("/sdcard", true)) {
    printf("SD_Recovery: SD_MMC begin succeeded\n");
    sd_backend_set(SD_BACKEND_SDMMC);
    SDCard_Flag = true;
    SDCard_Finish = true;
  } else {
    printf("SD_Recovery: SD_MMC begin failed\n");
    SDCard_Flag = false;
    SDCard_Finish = true;
    SD_D3_Dis();
    sd_backend_set(SD_BACKEND_NONE);
  }
}
bool File_Search(const char* directory, const char* fileName)    
{
  char vpath[256];
  make_vfs_path(directory, vpath, sizeof(vpath));
  struct stat st;
  if (stat(vpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
    printf("Path: <%s> does not exist\r\n", directory);
    return false;
  }
  DIR *d = opendir(vpath);
  if (!d) return false;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (strcmp(entry->d_name, fileName) == 0) {
      if (strcmp(directory, "/") == 0)
        printf("File '%s%s' found in root directory.\r\n", directory, fileName);
      else
        printf("File '%s/%s' found in root directory.\r\n", directory, fileName);
      closedir(d);
      return true;
    }
  }
  if (strcmp(directory, "/") == 0)
    printf("File '%s%s' not found in root directory.\r\n", directory, fileName);
  else
    printf("File '%s/%s' not found in root directory.\r\n", directory, fileName);
  closedir(d);
  return false;
}
uint16_t Folder_retrieval(const char* directory, const char* fileExtension, char File_Name[][100],uint16_t maxFiles)                                                                      
{
  char vpath[256];
  make_vfs_path(directory, vpath, sizeof(vpath));
  struct stat st;
  if (stat(vpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
    printf("Path: <%s> does not exist\r\n", directory);
    return 0;
  }
  uint16_t fileCount = 0;
  char filePath[200];
  DIR *d = opendir(vpath);
  if (!d) return 0;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL && fileCount < maxFiles) {
    if (entry->d_type == DT_DIR) continue;
    if (strstr(entry->d_name, fileExtension)) {
      strncpy(File_Name[fileCount], entry->d_name, sizeof(File_Name[fileCount]));
      if (strcmp(directory, "/") == 0) snprintf(filePath, sizeof(filePath), "%s%s", directory, entry->d_name);
      else snprintf(filePath, sizeof(filePath), "%s/%s", directory, entry->d_name);
      printf("File found: %s\r\n", filePath);
      fileCount++;
    }
  }
  closedir(d);
  if (fileCount > 0) {
    printf("Retrieved %d files\r\n", fileCount);
    return fileCount;
  } else {
    printf("No files with extension '%s' found in directory: %s\r\n", fileExtension, directory);
    return 0;
  }
}

void Flash_test()
{
  printf("/********** RAM Test**********/\r\n");
  // Get Flash size
  uint32_t flashSize = ESP.getFlashChipSize();
  Flash_Size = flashSize/1024/1024;
  printf("Flash size: %d MB \r\n", flashSize/1024/1024);

  printf("/******* RAM Test Over********/\r\n\r\n");
}

// Ensure an asset with a long filename exists by looking for an 8.3 short-name
// match (prefixMatch) in the assets folder and renaming it to desiredName
// if found. Returns true if desiredName exists or was created.
bool EnsureAssetFilename(const char* desiredName, const char* prefixMatch)
{
  // If asset scanning is disabled, skip entirely to avoid SD access.
  if (!SD_AllowAssetScan) {
    printf("EnsureAssetFilename: asset scanning disabled -> skipping\n");
    LastFoundAssetPath[0] = '\0';
    return false;
  }
  // If the SD card isn't mounted, skip asset scanning entirely to avoid
  // forcing SD access during boot (this prevents noisy sdmmc logs and
  // ensures boot continues when assets are absent).
  if (!SDCard_Flag) {
    printf("EnsureAssetFilename: SD not mounted -> skipping asset scan\n");
    LastFoundAssetPath[0] = '\0';
    return false;
  }
  // Try multiple common mount locations (including typo variants) and
  // check both SD_MMC and POSIX stat so this works whether the card
  // is mounted via the SD_MMC driver or via a VFS mount at /sdcard.
  const char *candidates[] = {
    "/sdcard/assets", "/assets",
    "/sdcard/ASSETS", "/ASSETS",
    "/sdcard/assest", "/assest",
    "/spiffs/assets", "/spiffs",
    "/spiffs/ASSETS"
  };
  const char *assetsPath = NULL;
  struct stat st;
  for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
    const char *p = candidates[i];
    bool found = false;
    if (sd_exists(p)) found = true;
    else if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) found = true;
    if (found) { assetsPath = p; break; }
  }
  if (!assetsPath) {
    printf("EnsureAssetFilename: assets directory not found\n");
    return false;
  }

  char desiredPath[128];
  snprintf(desiredPath, sizeof(desiredPath), "%s/%s", assetsPath, desiredName);
  if (sd_exists(desiredPath) || (stat(desiredPath, &st) == 0)) {
    printf("EnsureAssetFilename: desired asset already present: %s\n", desiredPath);
    strncpy(LastFoundAssetPath, desiredPath, sizeof(LastFoundAssetPath)-1);
    LastFoundAssetPath[sizeof(LastFoundAssetPath)-1] = '\0';
    return true;
  }

  // (SD_MMC support removed) — fall through to POSIX/VFS handling below.

  // POSIX fallback / SDSPI or SD_MMC open failure
  DIR *d = opendir(assetsPath);
  if (!d) {
    printf("EnsureAssetFilename: failed to open %s\n", assetsPath);
    return false;
  }
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    const char* fname = entry->d_name;
    if (!fname) continue;
    size_t pm_len = strlen(prefixMatch);
    if (strncasecmp(fname, prefixMatch, pm_len) == 0) {
      char oldpath[128];
      char desiredPathLocal[128];
      snprintf(oldpath, sizeof(oldpath), "%s/%s", assetsPath, fname);
      snprintf(desiredPathLocal, sizeof(desiredPathLocal), "%s/%s", assetsPath, desiredName);
      printf("EnsureAssetFilename: found candidate '%s' -> will attempt to create '%s'\n", oldpath, desiredPathLocal);
      // Try POSIX rename first
      if (rename(oldpath, desiredPathLocal) == 0) {
        strncpy(LastFoundAssetPath, desiredPathLocal, sizeof(LastFoundAssetPath)-1);
        LastFoundAssetPath[sizeof(LastFoundAssetPath)-1] = '\0';
        closedir(d);
        return true;
      }
      // Fallback: copy via POSIX
      FILE *src = fopen(oldpath, "rb");
      if (!src) { printf("EnsureAssetFilename: failed to open source %s\n", oldpath); closedir(d); return false; }
      FILE *dst = fopen(desiredPathLocal, "wb");
      if (!dst) { printf("EnsureAssetFilename: failed to open dest %s\n", desiredPathLocal); fclose(src); closedir(d); return false; }
      const size_t BUF_SZ = 512;
      uint8_t buf[BUF_SZ];
      size_t r;
      while ((r = fread(buf, 1, BUF_SZ, src)) > 0) fwrite(buf, 1, r, dst);
      fclose(dst); fclose(src);
      if (remove(oldpath) == 0) {
        strncpy(LastFoundAssetPath, desiredPathLocal, sizeof(LastFoundAssetPath)-1);
        LastFoundAssetPath[sizeof(LastFoundAssetPath)-1] = '\0';
        closedir(d);
        return true;
      }
    }
  }
  closedir(d);
  printf("EnsureAssetFilename: no matching file found for prefix '%s'\n", prefixMatch);
  return false;
}

// Runtime SD control implementations -----------------------------------------------------------------

bool SD_IsMounted() {
  return SDCard_Flag;
}


