#include "presence.h"
#include "net.h"
#include "teleport.h"
#include "spawner.h"
#include "npc.h"
#include "spawnpicker.h"
#include <cmath>
#include <share.h>
#include <string>
#include <vector>

// A civilian with nothing to do; change it with "puppet <entity class>" in data\multiplayer.txt.
static char g_puppetClass[128] = "PU_Human-Hurston-DeskWorker-Male-DistroHub_Civilian_01";

constexpr DWORD  kStaleMs = 3000;        // no position for this long = gone
constexpr double kMaxShowDistance = 2000.0;   // further than this they aren't streamed in for you anyway
constexpr double kMoveThreshold = 0.15;  // metres before we move a stand-in

struct Puppet {
    std::string name;
    uint64_t    id = 0;
    double      pos[3] = {};
    DWORD       lastSpawn = 0;
    bool        canMove = true;   // false: moving faulted / no teleport component, so we respawn instead
    bool        seen = false;
};
static std::vector<Puppet> g_puppets;

static void LoadPuppetClass() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    char path[MAX_PATH];
    FILE* f = DataFilePath("multiplayer.txt", path, sizeof(path)) ? _fsopen(path, "r", _SH_DENYNO) : nullptr;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "puppet ", 7) == 0 && line[7]) strncpy_s(g_puppetClass, line + 7, _TRUNCATE);
    }
    fclose(f);
}

// ---- Our zone chain ------------------------------------------------------------------------------

struct ChainZone { uintptr_t zone; char name[48]; double pos[3]; };

static int ReadChain(uintptr_t entity, ChainZone* out, int max) {
    __try {
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return 0;
        double local[3], world[3];
        Vec3Out(entity, 0x2B8, local);
        LocalToWorld(zone, local, world);
        int n = 0;
        for (uintptr_t z = zone; z && n < max; z = ZoneParent(z)) {
            const char* name = ZoneName(z);
            if (!name || !*name) continue;
            ChainZone& c = out[n];
            if (z == zone) memcpy(c.pos, local, sizeof(local));
            else if (!WorldToLocal(z, world, c.pos)) continue;
            c.zone = z;
            strncpy_s(c.name, name, _TRUNCATE);
            ++n;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The innermost zone of theirs that we have too - but not the solar system itself, which is too coarse to place
// anyone in. Returns the index into `mine`, and their position in that zone.
static int SharedZone(const ChainZone* mine, int mineCount, const NetPlayerState& st, double pos[3]) {
    for (int i = 0; i < st.zones; ++i) {
        if (_strnicmp(st.z[i].zone, "SolarSystem", 11) == 0 || _stricmp(st.z[i].zone, "Root") == 0) return -1;
        for (int j = 0; j < mineCount; ++j)
            if (_stricmp(st.z[i].zone, mine[j].name) == 0) {
                memcpy(pos, st.z[i].pos, sizeof(double) * 3);
                return j;
            }
    }
    return -1;
}

static double Distance(const double a[3], const double b[3]) {
    const double d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// ---- Moving a stand-in ---------------------------------------------------------------------------
// The same request teleport.cpp makes for you (actor -> teleport component +0x9D8 -> +0x158), on the NPC's
// actor (its "Actor" component). Checked once against our own actor; see LogActorCheck.

static const char* MoveEntity(uint64_t id, uintptr_t zone, const double local[3]) {
    __try {
        const uintptr_t es = *g_tp.entitySystem;
        const uintptr_t e = es ? VCall<uintptr_t>(es, 0x120, id) : 0;
        if (!e) return "gone";
        const uintptr_t actor = EntityComponent(e, "Actor");
        const uintptr_t comp = actor ? VCall<uintptr_t>(actor, 0x9D8) : 0;
        if (!comp) return "no teleport component";
        const uint64_t zoneId = ZoneId(zone);
        if (!zoneId) return "no zone id";
        double world[3];
        LocalToWorld(zone, local, world);
        alignas(16) uint8_t params[0x80] = {};
        *reinterpret_cast<uint64_t*>(params + 0x00) = zoneId;
        reinterpret_cast<double*>(params + 0x08)[3] = 1.0;
        memcpy(params + 0x28, local, 3 * sizeof(double));
        *reinterpret_cast<double*>(params + 0x40) = 1.0;
        memcpy(params + 0x48, world, sizeof(world));
        reinterpret_cast<float*>(params + 0x60)[3] = 1.0f;
        VCall<void>(comp, 0x158, params);
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault";
    }
}

static void LogActorCheck(uintptr_t actor, uintptr_t entity) {
    static bool done = false;
    if (done) return;
    done = true;
    uintptr_t viaComponent = 0;
    __try { viaComponent = EntityComponent(entity, "Actor"); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log("[mp] actor check: your actor %p, your entity's Actor component %p (%s)", reinterpret_cast<void*>(actor),
        reinterpret_cast<void*>(viaComponent), viaComponent == actor ? "same - stand-ins can be moved" : "different - stand-ins may only respawn");
}

static void RemovePuppet(Puppet& p) {
    if (p.id) RemoveEntityById(p.id);
    p.id = 0;
}

static void RemoveAll() {
    for (Puppet& p : g_puppets) RemovePuppet(p);
    g_puppets.clear();
}

static void SpawnPuppet(Puppet& p, uintptr_t zone, const double pos[3], DWORD now) {
    p.lastSpawn = now;
    uint64_t id = 0;
    const char* err = nullptr;
    __try { err = SpawnEntityInZone(g_puppetClass, ZoneId(zone), pos, nullptr, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { err = "fault"; }
    if (err) { Log("[mp] couldn't show %s (%s: %s)", p.name.c_str(), g_puppetClass, err); return; }
    p.id = id;
    memcpy(p.pos, pos, sizeof(p.pos));
    Log("[mp] showing %s as %s (entity %llu) in %s", p.name.c_str(), g_puppetClass, static_cast<unsigned long long>(id), ZoneName(zone));
}

static void UpdatePuppet(Puppet& p, uintptr_t zone, const double pos[3], DWORD now) {
    if (!p.id) {
        if (now - p.lastSpawn > 2000) SpawnPuppet(p, zone, pos, now);
        return;
    }
    if (Distance(p.pos, pos) < kMoveThreshold) return;
    if (p.canMove) {
        if (const char* err = MoveEntity(p.id, zone, pos)) {
            Log("[mp] moving %s's stand-in failed (%s); respawning it instead from now on", p.name.c_str(), err);
            p.canMove = false;
        } else {
            memcpy(p.pos, pos, sizeof(p.pos));
            return;
        }
    }
    if (now - p.lastSpawn > 1000) { RemovePuppet(p); SpawnPuppet(p, zone, pos, now); }
}

static bool InUniverse(uintptr_t& actor, uintptr_t& entity) {
    bool live = false;
    if (g_tp.ok) __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) { live = false; }
    return live && !(Spawn_Available() && !Spawn_InUniverse());   // the main menu has a player too
}

void ProcessPresence(DWORD now) {
    static DWORD next = 0;
    if (static_cast<LONG>(now - next) < 0) return;
    next = now + 100;

    const int mode = Net_Mode();
    const bool inSession = mode == NetMode_Hosting || mode == NetMode_Joined;
    uintptr_t actor = 0, entity = 0;
    const bool live = InUniverse(actor, entity);
    if (!inSession || !live) {
        Net_SetLocalState(nullptr, 0);
        if (live) RemoveAll();       // left the session: take the stand-ins away
        else g_puppets.clear();      // left the universe: they went with the level
        return;
    }
    LoadPuppetClass();
    LogActorCheck(actor, entity);

    ChainZone mine[kNetMaxZones];
    const int mineCount = ReadChain(entity, mine, kNetMaxZones);
    NetZonePos send[kNetMaxZones];
    for (int i = 0; i < mineCount; ++i) {
        strcpy_s(send[i].zone, mine[i].name);
        memcpy(send[i].pos, mine[i].pos, sizeof(send[i].pos));
    }
    Net_SetLocalState(send, mineCount);

    NetPlayerState others[16];
    const int count = Net_RemoteStates(others, 16);
    for (Puppet& p : g_puppets) p.seen = false;
    for (int i = 0; i < count; ++i) {
        const NetPlayerState& st = others[i];
        double pos[3];
        const int shared = st.ageMs < kStaleMs ? SharedZone(mine, mineCount, st, pos) : -1;
        if (shared < 0 || Distance(pos, mine[shared].pos) > kMaxShowDistance) continue;
        Puppet* p = nullptr;
        for (Puppet& q : g_puppets) if (q.name == st.name) p = &q;
        if (!p) { g_puppets.push_back({}); p = &g_puppets.back(); p->name = st.name; }
        p->seen = true;
        UpdatePuppet(*p, mine[shared].zone, pos, now);
    }
    for (size_t i = 0; i < g_puppets.size(); ) {
        if (g_puppets[i].seen) { ++i; continue; }
        if (g_puppets[i].id) Log("[mp] %s is out of view", g_puppets[i].name.c_str());
        RemovePuppet(g_puppets[i]);
        g_puppets.erase(g_puppets.begin() + static_cast<ptrdiff_t>(i));
    }
}
