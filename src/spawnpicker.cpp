#include "spawnpicker.h"
#include "hooks.h"
#include "teleport.h"
#include "lobby.h"
#include <algorithm>
#include <cstdarg>
#include <string>
#include <vector>
#include <share.h>

// CGameRulesPUSpawning's override lookup: zeroes the override location id, walks every location the
// location manager knows, and stores the id of the one whose name matches. The module calls it from its
// constructor with sv_OverridePlayerPersistSpawnLocation (empty unless set); the cvar's change callback
// calls it too. GetSpawnPointForUnspawnedPlayerId then uses that id instead of the player's own location.
static const char* const kLookupPattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B D9 C7 81 ?? ?? 00 00 00 00 00 00 "
    "48 8B 0D ?? ?? ?? ?? 48 8B F2 E8 ?? ?? ?? ?? 48 8B C8 4C 8B 00 41 FF 90 ?? ?? 00 00 48 8B F8 48 8B 08 "
    "48 8B A9 ?? ?? 00 00";
// The name-matching lambda it passes to the location manager: name = manager->vfunc(locationId).
static const char* const kLambdaPattern =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 59 10 8B FA 48 8B 4B 10 48 8B 01 FF 50";

// CGameRulesPUSpawning::DumpPotentialSpawnPoints walking the spawn-point map (std::map<locationId, vector<0xF8>>)
// and printing "Location %s has %zu spawn points". Its output never reaches Game.log offline, so we walk the
// map ourselves with the offsets read from here. Starts 0x44 bytes before the lea of that string.
static const char* const kDumpWalkPattern =
    "48 8B B5 ?? ?? ?? ?? 48 8B 1E 48 3B DE 0F 84 ?? ?? ?? ?? 4C 89 7C 24 30 49 BF 09 21 84 10 42 08 21 84 66 90 "
    "49 8B 06 49 8B CE 8B 53 ?? FF 50 ?? 48 8B 4B ?? 4C 8B C8 48 2B 4B ?? 49 8B C7 48 F7 E9 48 03 D1 48 8D 0D";
static const size_t kSpawnPointSize = 0xF8;   // the magic 0x8421084210842109 + sar 7 in that pattern = / 0xF8

using LookupFn = void(__fastcall*)(uintptr_t spawning, const char* name);
using GetterFn = uintptr_t(__fastcall*)(uintptr_t);

static struct {
    bool       ok = false;
    LookupFn   original = nullptr;
    uintptr_t* locGlobal = nullptr;
    GetterFn   locGetter = nullptr;
    uint32_t   managerSlot = 0, iterateSlot = 0, nameSlot = 0, overrideOff = 0;
    uint32_t   mapOff = 0, keyOff = 0, beginOff = 0, endOff = 0;   // spawn-point map (0 = not found)
} g_api;

struct SpawnLoc { std::string name; int points; };   // points: most spawn points seen in game, -1 = not seen yet

static SRWLOCK               g_lock = SRWLOCK_INIT;   // guards everything below
static std::vector<SpawnLoc> g_locs;
static char                  g_choice[128] = "";     // "" = the game's own spawn
static bool                  g_ask = true;
static char                  g_status[256] = "";
static uintptr_t             g_module = 0;            // the live CGameRulesPUSpawning
static bool                  g_applyPending = false;  // apply g_choice to the live module (main thread)

static volatile LONG g_pickerActive = 0;   // the game is waiting in the lookup hook for a pick
static volatile LONG g_picked = 0;

static void SetStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_lock);
    strcpy_s(g_status, buf);
    ReleaseSRWLockExclusive(&g_lock);
    Log("[spawn] %s", buf);
}

// ---- Files (next to the spawn file, i.e. the data folder) -------------------------------------

static void TrimEnd(char* s) {
    for (size_t n = strlen(s); n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' '); ) s[--n] = 0;
}

// data\spawn_location.txt: "ask 0|1" and "location <name>".
static void LoadChoice() {
    char path[MAX_PATH];
    FILE* f = DataFilePath("spawn_location.txt", path, sizeof(path)) ? _fsopen(path, "r", _SH_DENYNO) : nullptr;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        TrimEnd(line);
        if (strncmp(line, "ask ", 4) == 0) g_ask = atoi(line + 4) != 0;
        else if (strncmp(line, "location ", 9) == 0) strncpy_s(g_choice, line + 9, _TRUNCATE);
    }
    fclose(f);
}

static void SaveChoice() {
    char path[MAX_PATH];
    FILE* f = DataFilePath("spawn_location.txt", path, sizeof(path)) ? _fsopen(path, "w", _SH_DENYNO) : nullptr;
    if (!f) { Log("[spawn] could not write spawn_location.txt"); return; }
    fprintf(f, "# ChrisWareOffline spawn location (set it in the spawn picker).\n");
    fprintf(f, "# ask 1 = show the picker every time the game loads. No location line = the game's own spawn.\n");
    fprintf(f, "ask %d\n", g_ask ? 1 : 0);
    if (g_choice[0]) fprintf(f, "location %s\n", g_choice);
    fclose(f);
}

// ---- Featured locations ---------------------------------------------------------------------------
// What the lobby lists, like LIVE: Stanton's cities, Pyro's and Nyx's major stations. Names are the game's
// location names (all_locations.txt); titles are the game's own (Data.p4k global.ini, e.g. RR_P2_L4=Checkmate).
static const SpawnFeatured kFeatured[] = {
    { "Stanton1_Lorville",   "Lorville",        "Hurston",                 SpawnSys_Stanton },
    { "Stanton3_Area18",     "Area18",          "ArcCorp",                 SpawnSys_Stanton },
    { "Stanton2_Orison",     "Orison",          "Crusader",                SpawnSys_Stanton },
    { "Stanton4_NewBabbage", "New Babbage",     "microTech",               SpawnSys_Stanton },
    { "RR_P6_LEO",           "Ruin Station",    "Terminus orbit",          SpawnSys_Pyro },
    { "RR_P2_L4",            "Checkmate",       "Monox L4",                SpawnSys_Pyro },
    { "RR_P3_LEO",           "Orbituary",       "Bloom orbit",             SpawnSys_Pyro },
    { "RR_JP_PyroStanton",   "Stanton Gateway", "Pyro - Stanton jump point", SpawnSys_Pyro },
    { "RR_JP_PyroNyx",       "Nyx Gateway",     "Pyro - Nyx jump point",   SpawnSys_Pyro },
    { "Nyx_Levski",          "Levski",          "Delamar",                 SpawnSys_Nyx },
    { "RR_JP_NyxPyro",       "Pyro Gateway",    "Nyx - Pyro jump point",   SpawnSys_Nyx },
};

// Caller holds g_lock.
static SpawnLoc* FindLoc(const char* name) {
    for (SpawnLoc& l : g_locs)
        if (_stricmp(l.name.c_str(), name) == 0) return &l;
    return nullptr;
}

// Caller holds g_lock.
static void AddLoc(const char* name, int points) {
    if (SpawnLoc* l = FindLoc(name)) { if (points > l->points) l->points = points; return; }
    g_locs.push_back({ name, points });
}

// data\spawn_locations.txt: "<spawn points>\t<name>" for every location seen with spawn points in game.
static void LoadCache() {
    char path[MAX_PATH];
    FILE* f = DataFilePath("spawn_locations.txt", path, sizeof(path)) ? _fsopen(path, "r", _SH_DENYNO) : nullptr;
    if (!f) return;
    char line[512];
    AcquireSRWLockExclusive(&g_lock);
    while (fgets(line, sizeof(line), f)) {
        TrimEnd(line);
        char* tab = strchr(line, '\t');
        if (line[0] == '#' || !tab || !tab[1]) continue;
        *tab = 0;
        AddLoc(tab + 1, atoi(line));
    }
    ReleaseSRWLockExclusive(&g_lock);
    fclose(f);
}

// Caller holds g_lock.
static void SaveCache() {
    char path[MAX_PATH];
    FILE* f = DataFilePath("spawn_locations.txt", path, sizeof(path)) ? _fsopen(path, "w", _SH_DENYNO) : nullptr;
    if (!f) return;
    fprintf(f, "# Locations seen with spawn points in game (filled in automatically). <spawn points><TAB><name>\n");
    for (const SpawnLoc& l : g_locs)
        if (l.points > 0) fprintf(f, "%d\t%s\n", l.points, l.name.c_str());
    fclose(f);
}

static void SortLocs() {
    std::sort(g_locs.begin(), g_locs.end(), [](const SpawnLoc& a, const SpawnLoc& b) {
        if ((a.points > 0) != (b.points > 0)) return a.points > 0;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

// ---- Every location name, from the game's location manager ------------------------------------

// Same shape as the game's CigFunction: the iterator calls invoker(this, locationId) and stops on 0.
struct CigFunction { void* invoker; uintptr_t manager; void* ctx; };
struct Collect { uintptr_t locations; std::vector<std::string>* names; };

static int __fastcall CollectName(CigFunction* fn, uint32_t locationId) {
    const Collect* c = static_cast<const Collect*>(fn->ctx);
    const char* name = VCall<const char*>(c->locations, g_api.nameSlot, locationId);
    if (name && *name) c->names->push_back(name);
    return 1;
}

static uintptr_t LocationManager() {
    const uintptr_t holder = *g_api.locGlobal;
    const uintptr_t obj = holder ? g_api.locGetter(holder) : 0;
    return obj ? VCall<uintptr_t>(obj, g_api.managerSlot) : 0;
}

static bool IterateLocations(Collect& c) {
    __try {
        c.locations = LocationManager();
        if (!c.locations) return false;
        CigFunction fn = { reinterpret_cast<void*>(&CollectName), 0, &c };
        VCall<void>(c.locations, g_api.iterateSlot, &fn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void CollectAllLocations() {
    if (!g_api.nameSlot) return;
    std::vector<std::string> names;
    Collect c = { 0, &names };
    if (!IterateLocations(c)) { Log("[spawn] couldn't list the game's locations"); return; }
    AcquireSRWLockExclusive(&g_lock);
    for (const std::string& n : names) AddLoc(n.c_str(), -1);
    SortLocs();
    ReleaseSRWLockExclusive(&g_lock);
    Log("[spawn] the game knows %zu locations", names.size());

    char path[MAX_PATH];
    if (FILE* f = DataFilePath("all_locations.txt", path, sizeof(path)) ? _fsopen(path, "w", _SH_DENYNO) : nullptr) {
        fprintf(f, "# Every location name the game knows (written each launch, for reference).\n");
        for (const std::string& n : names) fprintf(f, "%s\n", n.c_str());
        fclose(f);
    }
}

// ---- The hook ----------------------------------------------------------------------------------

static uint32_t OverrideId(uintptr_t spawning) {
    __try { return Rd<uint32_t>(spawning + g_api.overrideOff); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void ReportOverride(uintptr_t spawning, const char* name) {
    if (!*name) return;
    if (const uint32_t id = OverrideId(spawning)) SetStatus("Spawn location set to '%s' (location %u).", name, id);
    else SetStatus("The game has no location called '%s' - you'll spawn at its default spot.", name);
}

static void WaitForPick() {
    for (int i = 0; i < 300 && !Lobby_IsRunning(); ++i) Sleep(100);
    if (!Lobby_IsRunning()) { Log("[spawn] the lobby window isn't up; not asking"); return; }
    InterlockedExchange(&g_picked, 0);
    InterlockedExchange(&g_pickerActive, 1);
    Log("[spawn] waiting for you to pick a spawn location");
    const DWORD start = GetTickCount();
    while (!g_picked && GetTickCount() - start < 15 * 60 * 1000) Sleep(50);
    InterlockedExchange(&g_pickerActive, 0);
    if (!g_picked) Log("[spawn] no pick after 15 minutes; carrying on");
}

static void __fastcall LookupHook(uintptr_t spawning, const char* name) {
    AcquireSRWLockExclusive(&g_lock);
    const bool moduleStart = spawning != g_module;   // the constructor's call (a new spawning module)
    g_module = spawning;
    const bool ask = g_ask;
    ReleaseSRWLockExclusive(&g_lock);
    if (!moduleStart) { g_api.original(spawning, name); return; }

    Log("[spawn] spawning module started (override cvar: '%s')", name ? name : "");
    CollectAllLocations();
    if (ask) WaitForPick();

    char use[128];
    AcquireSRWLockShared(&g_lock);
    strcpy_s(use, g_choice);
    ReleaseSRWLockShared(&g_lock);
    if (!use[0]) { g_api.original(spawning, name); Log("[spawn] using the game's own spawn"); return; }
    g_api.original(spawning, use);
    ReportOverride(spawning, use);
}

void ResolveSpawnPickerApi(const Section& text, const Section& rdata) {
    const uint8_t* msg = FindCString(rdata, "Couldn't find override spawn location with name %s");
    uint8_t* lea = msg ? FindRipLea(text, 0x48, 0x8D, 0x0D, msg) : nullptr;
    uint8_t* fn = lea ? lea - 0xBC : nullptr;
    if (!fn || !BytesMatch(fn, kLookupPattern)) { Log("[!] spawn picker: override lookup not found (game updated?)"); return; }
    g_api.overrideOff = *reinterpret_cast<const uint32_t*>(fn + 0x19);
    g_api.locGlobal   = reinterpret_cast<uintptr_t*>(fn + 0x28 + Rel32(fn + 0x24));
    g_api.locGetter   = reinterpret_cast<GetterFn>(fn + 0x30 + Rel32(fn + 0x2C));
    g_api.managerSlot = *reinterpret_cast<const uint32_t*>(fn + 0x39);
    g_api.iterateSlot = *reinterpret_cast<const uint32_t*>(fn + 0x46);
    const uint8_t* lambda = fn + 0x7B + Rel32(fn + 0x77);
    if (BytesMatch(fn + 0x74, "48 8D 05") && lambda > text.base && lambda + 0x20 < text.base + text.size && BytesMatch(lambda, kLambdaPattern))
        g_api.nameSlot = lambda[0x19];
    else
        Log("[!] spawn picker: location names unavailable; only names from spawn_locations.txt are listed");
    if (!HookFunction(fn, 10, reinterpret_cast<void*>(&LookupHook), reinterpret_cast<void**>(&g_api.original))) {
        Log("[!] spawn picker: couldn't hook the override lookup");
        return;
    }
    const uint8_t* dumpMsg = FindCString(rdata, "Location %s has %zu spawn points");
    const uint8_t* dumpLea = dumpMsg ? FindRipLea(text, 0x48, 0x8D, 0x0D, dumpMsg) : nullptr;
    const uint8_t* walk = dumpLea ? dumpLea - 0x44 : nullptr;
    if (walk && BytesMatch(walk, kDumpWalkPattern)) {
        g_api.mapOff   = *reinterpret_cast<const uint32_t*>(walk + 3);
        g_api.keyOff   = walk[44];
        g_api.endOff   = walk[51];
        g_api.beginOff = walk[58];
        if (!g_api.nameSlot) g_api.nameSlot = walk[47];
    } else {
        Log("[!] spawn picker: spawn-point map not found; can't tell which locations have spawn points");
    }

    g_api.ok = true;
    LoadChoice();
    LoadCache();
    AcquireSRWLockExclusive(&g_lock);
    SortLocs();
    ReleaseSRWLockExclusive(&g_lock);
    Log("[+] spawn picker: ready (%s; next spawn: %s)", g_ask ? "asks every launch" : "doesn't ask",
        g_choice[0] ? g_choice : "game's own spawn");
}

// ---- In game: learn which locations have spawn points ------------------------------------------
// Walks the module's spawn-point map the way DumpPotentialSpawnPoints does (MSVC std::map: node
// _Left +0, _Parent +8, _Right +0x10, _Isnil +0x19). Main thread only, without m_spawnPointsLock; every
// read is under __try and the walk is capped, so a map changing under us gives a bad count at worst.

struct MapEntry { uint32_t id; size_t points; };

static bool IsNil(uintptr_t node) { return !node || Rd<uint8_t>(node + 0x19) != 0; }

static int WalkSpawnPointMap(uintptr_t module, MapEntry* out, int max) {
    __try {
        const uintptr_t head = Rd<uintptr_t>(module + g_api.mapOff);
        if (!head || !IsNil(head)) return -1;   // not a map head: wrong object (or the module is gone)
        int n = 0;
        uintptr_t node = Rd<uintptr_t>(head);   // leftmost = begin()
        for (int guard = 0; node != head && guard < 100000; ++guard) {
            if (!node) return -1;
            const uintptr_t b = Rd<uintptr_t>(node + g_api.beginOff), e = Rd<uintptr_t>(node + g_api.endOff);
            if (n < max && e >= b) out[n++] = { Rd<uint32_t>(node + g_api.keyOff), (e - b) / kSpawnPointSize };
            const uintptr_t right = Rd<uintptr_t>(node + 0x10);
            if (!IsNil(right)) {
                node = right;
                while (!IsNil(Rd<uintptr_t>(node))) node = Rd<uintptr_t>(node);
            } else {
                uintptr_t parent = Rd<uintptr_t>(node + 8);
                while (!IsNil(parent) && node == Rd<uintptr_t>(parent + 0x10)) { node = parent; parent = Rd<uintptr_t>(parent + 8); }
                node = parent;
            }
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static const char* LocationName(uintptr_t locations, uint32_t id) {
    __try { return VCall<const char*>(locations, g_api.nameSlot, id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static uintptr_t SafeLocationManager() {
    __try { return LocationManager(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Returns how many locations currently have spawn points (-1 = couldn't read the map).
static int ReadSpawnPoints(uintptr_t module, bool logAll) {
    if (!module || !g_api.mapOff || !g_api.nameSlot) return -1;
    static MapEntry entries[4096];
    const int n = WalkSpawnPointMap(module, entries, 4096);
    if (n < 0) return -1;
    const uintptr_t locations = SafeLocationManager();
    if (!locations) return -1;

    struct Named { std::string name; int points; };
    std::vector<Named> seen;
    for (int i = 0; i < n; ++i) {
        if (!entries[i].points) continue;
        const char* name = LocationName(locations, entries[i].id);
        if (name && *name) seen.push_back({ name, static_cast<int>(entries[i].points) });
        else if (logAll) Log("[spawn]   location %u (no name) has %zu spawn points", entries[i].id, entries[i].points);
    }
    int added = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (const Named& s : seen) {
        const SpawnLoc* before = FindLoc(s.name.c_str());
        if (!before || before->points <= 0) ++added;
        AddLoc(s.name.c_str(), s.points);
    }
    if (added) { SortLocs(); SaveCache(); }
    ReleaseSRWLockExclusive(&g_lock);

    if (logAll || added) {
        Log("[spawn] spawn-point map: %d locations with spawn points right now (%d new, saved to spawn_locations.txt)",
            static_cast<int>(seen.size()), added);
        for (const Named& s : seen) Log("[spawn]   %s: %d", s.name.c_str(), s.points);
    }
    return static_cast<int>(seen.size());
}

void ProcessSpawnPicker(DWORD now) {
    if (!g_api.ok) return;

    AcquireSRWLockExclusive(&g_lock);
    const bool apply = g_applyPending && g_module;
    g_applyPending = false;
    const uintptr_t module = g_module;
    char choice[128];
    strcpy_s(choice, g_choice);
    ReleaseSRWLockExclusive(&g_lock);
    if (apply) {
        __try { g_api.original(module, choice); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[spawn] fault applying the spawn location"); }
        ReportOverride(module, choice);
    }

    static DWORD liveSince = 0, nextRead = 0;
    static bool zonesLogged = false, firstRead = true;
    uintptr_t actor, entity;
    bool live = false;
    __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!live) { liveSince = 0; return; }
    if (!liveSince) { liveSince = now; nextRead = now + 20000; }

    if (!zonesLogged && now - liveSince > 10000) {
        zonesLogged = true;
        char chain[512];
        if (PlayerZoneChain(chain, sizeof(chain))) Log("[spawn] you spawned in: %s", chain);
    }

    // Spawn points register as locations stream in, so keep checking while you travel.
    if (static_cast<LONG>(now - nextRead) >= 0) {
        nextRead = now + 60000;
        const int n = ReadSpawnPoints(module, firstRead);
        if (n < 0 && firstRead) Log("[spawn] couldn't read the spawn-point map");
        firstRead = false;
    }
}

// ---- Lobby side ---------------------------------------------------------------------------------

bool Spawn_Available() { return g_api.ok; }
bool Spawn_PickerActive() { return g_pickerActive != 0; }

void Spawn_LeaveUniverse() {
    AcquireSRWLockExclusive(&g_lock);
    g_module = 0;
    g_applyPending = false;
    ReleaseSRWLockExclusive(&g_lock);
}

bool Spawn_InUniverse() {
    AcquireSRWLockShared(&g_lock);
    const bool started = g_module != 0;
    ReleaseSRWLockShared(&g_lock);
    return started;
}

int Spawn_FeaturedCount() { return static_cast<int>(sizeof(kFeatured) / sizeof(kFeatured[0])); }
const SpawnFeatured& Spawn_Featured(int index) { return kFeatured[index]; }

int Spawn_PointsSeen(const char* name) {
    AcquireSRWLockShared(&g_lock);
    const SpawnLoc* l = FindLoc(name);
    const int points = l ? l->points : -1;
    ReleaseSRWLockShared(&g_lock);
    return points;
}

void Spawn_GetChoice(char* name, size_t n, bool& ask) {
    AcquireSRWLockShared(&g_lock);
    strncpy_s(name, n, g_choice, _TRUNCATE);
    ask = g_ask;
    ReleaseSRWLockShared(&g_lock);
}

void Spawn_GetStatus(char* out, size_t n) {
    AcquireSRWLockShared(&g_lock);
    strncpy_s(out, n, g_status, _TRUNCATE);
    ReleaseSRWLockShared(&g_lock);
}

void Spawn_SetAsk(bool ask) {
    AcquireSRWLockExclusive(&g_lock);
    g_ask = ask;
    SaveChoice();
    ReleaseSRWLockExclusive(&g_lock);
}

void Spawn_Choose(const char* name) {
    AcquireSRWLockExclusive(&g_lock);
    strncpy_s(g_choice, name ? name : "", _TRUNCATE);
    SaveChoice();
    // Clear the active flag here, not only when the waiting thread wakes, so the window closing right after
    // a pick doesn't count as a second pick.
    const bool waiting = InterlockedExchange(&g_pickerActive, 0) != 0;
    if (!waiting && g_module) g_applyPending = true;   // in game: also used when you respawn (untested)
    ReleaseSRWLockExclusive(&g_lock);
    if (waiting) InterlockedExchange(&g_picked, 1);
    Log("[spawn] picked: %s", name && *name ? name : "game's own spawn");
}
