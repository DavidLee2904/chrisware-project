#pragma once
#include "common.h"

// Moving between the universe and the game's own main menu (where its character creator lives) without
// restarting. The game's "return to main menu" request is turned into a PU request by the boot patch; we
// switch that patch off to reach the main menu and on again to come back (patches.cpp SetBootIntoPU).
void ResolveSessionApi(const Section& text, const Section& rdata);
void ProcessSession();             // main thread

// ---- For the lobby window (any thread) ----
bool Session_Available();
void Session_OpenCreator();        // universe -> the game's main menu
void Session_ReturnToUniverse();   // main menu -> universe (the spawn picker asks where, as at launch)
