#pragma once
#include "Arduino.h"
#include <cstring>
#include "FS.h"
#include "SD_MMC.h"

#include "TCA9554PWR.h"

#define SD_CLK_PIN   2
#define SD_CMD_PIN  1 
#define SD_D0_PIN  4

extern uint16_t SDCard_Size;
extern uint16_t Flash_Size;

esp_err_t SD_Init();

// Check SD health after potential pin-sharing events (panel takeover) and
// attempt a conservative recovery (SD_MMC.end()/SD_MMC.begin) if reads fail.
void SD_RecoveryCheck(void);void Flash_test();

// Control external SD CS (EXIO_PIN4) helper APIs used for diagnostics
void SD_D3_Dis();
void SD_D3_EN();

// Runtime SD control helpers
bool SD_IsMounted();

bool File_Search(const char* directory, const char* fileName);
uint16_t Folder_retrieval(const char* directory, const char* fileExtension, char File_Name[][100],uint16_t maxFiles);
