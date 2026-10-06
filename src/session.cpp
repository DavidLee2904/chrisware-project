#include "session.h"
#include "patches.h"
#include "spawnpicker.h"
#include "character.h"

// The game's own way back to the main menu, e.g. in CGameRulesPUSpawning:
//   mov rcx,[holder]; call getter; lea rdx,"ReturnToFrontendGameMode"; mov rcx,[rax]; mov r8,[rcx+slot]; ...jmp r8
// i.e. getter(holder)->RequestFrontEnd(reason). The session manager then loads the frontend game mode, which the
// boot patch decides: the main menu (patch off) or the PU (patch on).
using GetterFn = uintptr_t(__fastcall*)(uintptr_t);

static struct {
    bool       ok = false;
    uintptr_t* holder = nullptr;
    GetterFn   getter = nullptr;
    uint32_t   requestSlot = 0;
} g_api;

static volatile LONG g_request = 0;   // 1 = to the main menu, 2 = to the universe

void ResolveSessionApi(const Section& text, const Section& rdata) {
    const uint8_t* reason = FindCString(rdata, "ReturnToFrontendGameMode");
    const uint8_t* lea = reason ? FindRipLea(text, 0x48, 0x8D, 0x15, reason) : nullptr;
    const uint8_t* mov = lea ? lea - 12 : nullptr;
    if (!lea || !BytesMatch(mov, "48 8B 0D") || !BytesMatch(lea - 5, "E8") || !BytesMatch(lea + 7, "48 8B 08 4C 8B 81 ?? ?? 00 00")) {
        Log("[!] session: the game's return-to-main-menu call wasn't found; the lobby can't open the character creator");
        return;
    }
    g_api.holder      = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(mov + 7 + Rel32(mov + 3)));
    g_api.getter      = reinterpret_cast<GetterFn>(const_cast<uint8_t*>(lea + Rel32(lea - 4)));
    g_api.requestSlot = *reinterpret_cast<const uint32_t*>(lea + 13);
    g_api.ok = true;
}

static bool RequestFrontEnd(const char* reason) {
    __try {
        const uintptr_t holder = *g_api.holder;
        const uintptr_t game = holder ? g_api.getter(holder) : 0;
        if (!game) return false;
        VCall<void>(game, g_api.requestSlot, reason);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ProcessSession() {
    const LONG request = InterlockedExchange(&g_request, 0);
    if (!request || !g_api.ok) return;
    const bool toUniverse = request == 2;
    if (!SetBootIntoPU(toUniverse)) { Log("[session] couldn't switch the boot patch"); return; }
    if (!toUniverse) {
        Spawn_LeaveUniverse();      // the PU spawning module goes away with the level
        Character_NewSession();     // the next body is a first spawn again
    }
    const char* reason = toUniverse ? "ChrisWareOffline: back to the universe" : "ChrisWareOffline: open the character creator";
    if (RequestFrontEnd(reason)) Log("[session] %s", toUniverse ? "loading the universe" : "loading the game's main menu (character creator)");
    else Log("[session] the request faulted");
}

bool Session_Available() { return g_api.ok; }
void Session_OpenCreator() { InterlockedExchange(&g_request, 1); }
void Session_ReturnToUniverse() { InterlockedExchange(&g_request, 2); }
