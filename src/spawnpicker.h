#pragma once
#include "common.h"

// Spawn location picker. The game's PU spawning module has a server-side override,
// sv_OverridePlayerPersistSpawnLocation: a location name it looks up when the module starts,
// just before the player is spawned. We hook that lookup, show the lobby window while the game waits,
// and hand it the location you chose.
void ResolveSpawnPickerApi(const Section& text, const Section& rdata);
void ProcessSpawnPicker(DWORD now);

// ---- For the lobby window (any thread) ----
enum SpawnSystem { SpawnSys_Stanton, SpawnSys_Pyro, SpawnSys_Nyx, SpawnSys_Count };
struct SpawnFeatured {
    const char* name;    // the game's location name (what the override takes)
    const char* title;   // in-game name, e.g. "Checkmate"
    const char* where;   // e.g. "Monox L4"
    int         system;  // SpawnSystem
};
bool                 Spawn_Available();
bool                 Spawn_PickerActive();                    // the game is waiting for a pick right now
bool                 Spawn_InUniverse();                      // the PU spawning module has started (not the main menu)
void                 Spawn_LeaveUniverse();                   // main thread: the PU level is being left
int                  Spawn_FeaturedCount();
const SpawnFeatured& Spawn_Featured(int index);
int                  Spawn_PointsSeen(const char* name);      // most spawn points seen in game; -1 = not seen yet
void                 Spawn_GetChoice(char* name, size_t n, bool& askEveryLaunch);
void                 Spawn_GetStatus(char* out, size_t n);
void                 Spawn_SetAsk(bool askEveryLaunch);
void                 Spawn_Choose(const char* name);          // "" = the game's own spawn
