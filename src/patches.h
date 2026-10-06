#pragma once
#include "common.h"

extern const uint8_t* g_isOnlineFlag;

bool ApplyOfflinePatches();
void LogOfflinePatches();
// Frontend requests load the PU (on) or the game's main menu (off). False if the request site wasn't found.
bool SetBootIntoPU(bool on);
