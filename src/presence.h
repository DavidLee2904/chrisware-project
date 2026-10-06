#pragma once
#include "common.h"

// Other players in your world (multiplayer phase 1): your position goes out over the session (net.cpp), and
// everyone else who shares a zone with you is shown as a stand-in NPC moved to where they are.
void ProcessPresence(DWORD now);   // main thread
