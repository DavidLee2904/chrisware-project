#include "character.h"
#include "cvars.h"
#include "teleport.h"
#include "spawnpicker.h"
#include "hooks.h"
#include "loadout.h"
#include <cstdarg>
#include <string>
#include <vector>
#include <share.h>

// Looks the game ships (Data.p4k, CharacterCustomization xml - the format ca_applyCustomHeadFile parses).
static const struct { const char* id; const char* title; const char* detail; const char* gamePath; } kPresets[] = {
    { "preset:masculine", "Default masculine", "The game's own default (MasculineDefault.xml)", "Libs/CharacterCustomizer/MasculineDefault.xml" },
    { "preset:feminine",  "Default feminine",  "The game's own default (FeminineDefault.xml)",  "Libs/CharacterCustomizer/FeminineDefault.xml" },
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

static SRWLOCK                  g_lock = SRWLOCK_INIT;   // guards everything below
static std::vector<std::string> g_userLooks;             // *.xml in data\characters
static char                     g_choice[128] = "";      // "" = the game's default look
static bool                     g_auto = true;
static char                     g_status[256] = "";
static bool                     g_applyRequested = false;
static bool                     g_loaded = false;
static volatile LONG            g_inGame = 0;

static void SetStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_lock);
    strcpy_s(g_status, buf);
    ReleaseSRWLockExclusive(&g_lock);
    Log("[char] %s", buf);
}

// ---- Files: data\character.txt ("look <id>", "auto 0|1") and data\characters\*.chf|xml ----------

static void TrimEnd(char* s) {
    for (size_t n = strlen(s); n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' '); ) s[--n] = 0;
}

// Caller holds g_lock (or it's before anyone else can see the state).
static void LoadChoice() {
    char path[MAX_PATH];
    FILE* f = DataFilePath("character.txt", path, sizeof(path)) ? _fsopen(path, "r", _SH_DENYNO) : nullptr;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        TrimEnd(line);
        if (strncmp(line, "auto ", 5) == 0) g_auto = atoi(line + 5) != 0;
        else if (strncmp(line, "look ", 5) == 0) strncpy_s(g_choice, line + 5, _TRUNCATE);
    }
    fclose(f);
}

// Caller holds g_lock.
static void SaveChoice() {
    char path[MAX_PATH];
    FILE* f = DataFilePath("character.txt", path, sizeof(path)) ? _fsopen(path, "w", _SH_DENYNO) : nullptr;
    if (!f) { Log("[char] could not write character.txt"); return; }
    fprintf(f, "# ChrisWareOffline character look (set it in the lobby, F9).\n");
    fprintf(f, "# look preset:masculine | preset:feminine | <file>.xml from data\\characters. No look line = the game's default.\n");
    fprintf(f, "auto %d\n", g_auto ? 1 : 0);
    if (g_choice[0]) fprintf(f, "look %s\n", g_choice);
    fclose(f);
}

// Caller holds g_lock.
static void ScanUserLooks() {
    g_userLooks.clear();
    for (const char* ext : { "characters\\*.chf", "characters\\*.xml" }) {
        char pattern[MAX_PATH];
        if (!DataFilePath(ext, pattern, sizeof(pattern))) return;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) g_userLooks.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static void EnsureLoaded() {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_loaded) {
        g_loaded = true;
        LoadChoice();
        ScanUserLooks();
        char dir[MAX_PATH];
        if (DataFilePath("characters", dir, sizeof(dir))) CreateDirectoryA(dir, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- Applying (main thread) ----------------------------------------------------------------------

// The path ca_applyCustomHeadFile gets for a look id. User files are copied into %USER% first: the
// game's file system reads from there (like the gear loadouts), and the console splits arguments on spaces.
static bool GamePathFor(const char* id, char* out, size_t n) {
    for (const auto& p : kPresets)
        if (strcmp(p.id, id) == 0) { strcpy_s(out, n, p.gamePath); return true; }
    char src[MAX_PATH], rel[MAX_PATH], userDir[MAX_PATH], dst[MAX_PATH];
    snprintf(rel, sizeof(rel), "characters\\%s", id);
    if (!DataFilePath(rel, src, sizeof(src))) return false;
    const DWORD len = GetEnvironmentVariableA("SC_USER", userDir, sizeof(userDir));
    if (!len || len >= sizeof(userDir)) { Log("[char] SC_USER isn't set (start the game with launch_offline.bat)"); return false; }
    const char* dot = strrchr(id, '.');
    const char* ext = dot ? dot : ".xml";
    snprintf(dst, sizeof(dst), "%s\\chriswareoffline_look%s", userDir, ext);
    if (!CopyFileA(src, dst, FALSE)) { Log("[char] couldn't copy %s to %s (%lu)", src, dst, GetLastError()); return false; }
    snprintf(out, n, "%%USER%%/chriswareoffline_look%s", ext);
    return true;
}

// ca_applyCustomHeadFile is registered dev-only (flags 4), so the console may refuse it in this build.
// We call its handler ourselves with a stand-in for IConsoleCmdArgs; it only asks for the argument count
// (+0x08) and an argument (+0x10). Two arguments = file, applied to the client actor.
using CommandFn = void(__fastcall*)(void* args);
static CommandFn g_applyHeadFile = nullptr;

struct FakeCmdArgs {
    void* const* vtable;
    const char*  argv[2];
    const char*  line;
};
static void __fastcall ArgsDestroy(FakeCmdArgs*) {}
static int __fastcall ArgsCount(FakeCmdArgs*) { return 2; }
static const char* __fastcall ArgsGet(FakeCmdArgs* a, int i) { return i >= 0 && i < 2 ? a->argv[i] : ""; }
static const char* __fastcall ArgsLine(FakeCmdArgs* a) { return a->line; }
static void* const kFakeArgsVtable[] = {
    reinterpret_cast<void*>(&ArgsDestroy), reinterpret_cast<void*>(&ArgsCount),
    reinterpret_cast<void*>(&ArgsGet), reinterpret_cast<void*>(&ArgsLine),
};

static bool CallApplyHeadFile(const char* gamePath, const char* line) {
    FakeCmdArgs args = { kFakeArgsVtable, { "ca_applyCustomHeadFile", gamePath }, line };
    __try { g_applyHeadFile(&args); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Apply() {
    char id[128];
    AcquireSRWLockShared(&g_lock);
    strcpy_s(id, g_choice);
    ReleaseSRWLockShared(&g_lock);
    if (!id[0]) return false;
    char gamePath[MAX_PATH], cmd[MAX_PATH + 32];
    if (!GamePathFor(id, gamePath, sizeof(gamePath))) { SetStatus("Couldn't find the look '%s'.", id); return false; }
    snprintf(cmd, sizeof(cmd), "ca_applyCustomHeadFile %s", gamePath);
    if (g_applyHeadFile) {
        if (CallApplyHeadFile(gamePath, cmd)) { SetStatus("Applied '%s' (%s, called directly). Check it in third person (F4).", id, gamePath); return true; }
        SetStatus("Fault while applying '%s'.", id);
    } else if (RunConsoleNow(cmd)) {
        SetStatus("Applied '%s' (console: %s). Check it in third person (F4).", id, cmd);
        return true;
    } else {
        SetStatus("Couldn't run the console command for '%s'.", id);
    }
    return false;
}

// ---- Saving a look made in the game's own creator (main menu) -----------------------------------
// The creator's Save sends the look to CIG's character service (CCharacterCustomizerComponent's
// SaveCharacterCustomizations), which fails offline. Its dev-mode "save head file" routine writes the same
// look to a .chf file instead - the format ca_applyCustomHeadFile and the creator's own loader read. We hook
// the online save, write the file first, then let the online save run (and fail) as before.

// Online save: CCharacterCustomizerComponent::SaveCharacterCustomizations(this). The lea of its log
// text "CharacterCustomizer - SaveCharacterCustomizations" sits at +0x6BF.
static const char* const kOnlineSavePattern =
    "48 8B C4 55 53 48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? 00 00 80 B9 ?? ?? 00 00 00 48 8B D9";
// The one call of the file save (this, path) in the component's event handler:
//   mov rdx,[rbp+path]; mov rcx,r12; call SaveHeadFile; xor edx,edx; mov rcx,r12; call ...
static const char* const kFileSaveCallPattern = "48 8B 95 ?? ?? 00 00 49 8B CC E8 ?? ?? ?? ?? 33 D2 49 8B CC E8";
static const char* const kFileSavePrologue = "40 55 53 56 41 56 41 57 48 8D 6C 24";
static const char* const kCreatedName = "chriswareoffline_created.chf";

using OnlineSaveFn = void(__fastcall*)(uintptr_t customizer);
using FileSaveFn = void(__fastcall*)(uintptr_t customizer, const char* path);
static OnlineSaveFn  g_origOnlineSave = nullptr;
static FileSaveFn    g_fileSave = nullptr;
static volatile LONG g_createdPending = 0;
static DWORD         g_createdSince = 0;

static bool UserFile(const char* name, char* path, size_t n) {
    char dir[MAX_PATH];
    const DWORD len = GetEnvironmentVariableA("SC_USER", dir, sizeof(dir));
    if (!len || len >= sizeof(dir)) return false;
    snprintf(path, n, "%s\\%s", dir, name);
    return true;
}

static bool SaveHeadFile(uintptr_t customizer, const char* gamePath) {
    __try { g_fileSave(customizer, gamePath); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void __fastcall OnlineSaveHook(uintptr_t customizer) {
    char file[MAX_PATH];
    if (UserFile(kCreatedName, file, sizeof(file))) DeleteFileA(file);
    char gamePath[64];
    snprintf(gamePath, sizeof(gamePath), "%%USER%%/%s", kCreatedName);
    if (SaveHeadFile(customizer, gamePath)) {
        Log("[char] the creator saved; writing your look to %s", gamePath);
        g_createdSince = GetTickCount();
        InterlockedExchange(&g_createdPending, 1);
    } else {
        Log("[char] fault while writing the creator's look to a file");
    }
    g_origOnlineSave(customizer);
}

static void ResolveApplyHeadFile(const Section& text, const Section& rdata);

void ResolveCharacterApi(const Section& text, const Section& rdata) {
    ResolveApplyHeadFile(text, rdata);
    const uint8_t* msg = FindCString(rdata, "CharacterCustomizer - SaveCharacterCustomizations");
    uint8_t* lea = msg ? FindRipLea(text, 0x4C, 0x8D, 0x05, msg) : nullptr;
    uint8_t* online = lea ? lea - 0x6BF : nullptr;
    int matches = 0;
    uint8_t* call = FindUniquePattern(text, kFileSaveCallPattern, matches);
    uint8_t* fileSave = call ? call + 15 + Rel32(call + 11) : nullptr;
    if (!online || !BytesMatch(online, kOnlineSavePattern) || !fileSave || !BytesMatch(fileSave, kFileSavePrologue)) {
        Log("[!] character: the creator's save wasn't found (game updated?); looks made in it won't be kept");
        return;
    }
    g_fileSave = reinterpret_cast<FileSaveFn>(fileSave);
    if (!HookFunction(online, 12, reinterpret_cast<void*>(&OnlineSaveHook), reinterpret_cast<void**>(&g_origOnlineSave))) {
        Log("[!] character: couldn't hook the creator's save");
        return;
    }
    Log("[+] character: looks saved in the game's creator are kept in data\\characters");
}

// The handler registered for ca_applyCustomHeadFile: lea rdx,"ca_applyCustomHeadFile"; mov [rsp+20h],r8;
// mov r9d,4 (flags); lea r8,handler; call [rax+120h] (console RegisterCommand).
static void ResolveApplyHeadFile(const Section& text, const Section& rdata) {
    const uint8_t* name = FindCString(rdata, "ca_applyCustomHeadFile");
    const uint8_t* lea = name ? FindRipLea(text, 0x48, 0x8D, 0x15, name) : nullptr;
    if (!lea || !BytesMatch(lea + 7, "4C 89 44 24 20 41 B9 ?? 00 00 00 4C 8D 05 ?? ?? ?? ?? FF 90 20 01 00 00")) {
        Log("[!] character: ca_applyCustomHeadFile's handler not found; using the console (may be refused)");
        return;
    }
    g_applyHeadFile = reinterpret_cast<CommandFn>(const_cast<uint8_t*>(lea + 0x19 + Rel32(lea + 0x15)));
    Log("[+] character: applying looks through ca_applyCustomHeadFile's handler");
}

// Main thread: once the creator's file shows up, keep it in data\characters and make it your look.
static void CollectCreatedLook(DWORD now) {
    if (!g_createdPending) return;
    char src[MAX_PATH];
    const bool haveSrc = UserFile(kCreatedName, src, sizeof(src));
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!haveSrc || !GetFileAttributesExA(src, GetFileExInfoStandard, &a) || (!a.nFileSizeLow && !a.nFileSizeHigh)) {
        if (now - g_createdSince > 15000) {
            InterlockedExchange(&g_createdPending, 0);
            SetStatus("The creator saved, but no look file appeared (%s).", kCreatedName);
        }
        return;
    }
    InterlockedExchange(&g_createdPending, 0);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64], rel[96], dst[MAX_PATH];
    snprintf(name, sizeof(name), "Created %04d-%02d-%02d %02d-%02d-%02d.chf", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    snprintf(rel, sizeof(rel), "characters\\%s", name);
    if (!DataFilePath(rel, dst, sizeof(dst)) || !CopyFileA(src, dst, FALSE)) {
        SetStatus("Couldn't copy your new look into data\\characters (%lu).", GetLastError());
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    strcpy_s(g_choice, name);
    SaveChoice();
    ScanUserLooks();
    ReleaseSRWLockExclusive(&g_lock);
    SetStatus("Saved the look you made as '%s'. It's your look in the universe now.", name);
}

// ---- Look + outfit ---------------------------------------------------------------------------------
// A look's loadout is Body_ItemPort with only the head on it, so putting it on takes the rest off the body; the
// gear loader's loadout brings a stock head. Started together they race and whichever loads last wins (Game.log
// AttachmentReceived). So it's done in order: put the look on (its face shape and skin stay on the actor), read
// the head items it attached from Game.log, then equip the outfit with that head (loadout.cpp HeadAndMobiGlas).

static std::string g_headXml;   // the current look's Head_ItemPort subtree; "" = the gear loader's stock head

std::string Character_HeadXml() { return g_headXml; }

static bool GameLogPath(char* path, size_t n) {
    const DWORD len = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(n));
    if (!len || len >= n) return false;
    for (int i = 0; i < 2; ++i) {   // ...\LIVE\Bin64\StarCitizen.exe -> ...\LIVE
        char* slash = strrchr(path, '\\');
        if (!slash) return false;
        *slash = 0;
    }
    strcat_s(path, n, "\\Game.log");
    return true;
}

static long long FileSize(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return -1;
    return (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

struct Attached { std::string port, item; };

// Head ports that don't sit directly on Head_ItemPort (layout of the game's MasculineDefault.xml).
static const char* HeadParent(const std::string& port) {
    static const struct { const char* port; const char* parent; } kNesting[] = {
        { "Lens_ItemPort", "Eyes_ItemPort" }, { "radar", "Lens_ItemPort" },
        { "Material_Variant", "Hair_ItemPort" }, { "Stubble_ItemPort", "Beard_ItemPort" } };
    for (const auto& n : kNesting)
        if (port == n.port) return n.parent;
    return "Head_ItemPort";
}

static bool IsHeadPort(const std::string& port) {
    if (port == "radar" || port == "Material_Variant" || port == "Skin_Variant") return true;
    return port != "Body_ItemPort" && port.size() > 9 && port.compare(port.size() - 9, 9, "_ItemPort") == 0;
}

// "...<AttachmentReceived> Player[name] Attachment[hair_13_2000, hair_13, 2000] Status[..] Port[Hair_ItemPort]..."
static bool ParseAttachment(const char* line, std::string& player, Attached& out) {
    const char* p = strstr(line, "<AttachmentReceived> Player[");
    const char* a = p ? strstr(p, "Attachment[") : nullptr;
    const char* c = a ? strstr(a, ", ") : nullptr;
    const char* ce = c ? strchr(c + 2, ',') : nullptr;
    const char* port = ce ? strstr(ce, "Port[") : nullptr;
    const char* pe = port ? strchr(port + 5, ']') : nullptr;
    if (!pe) return false;
    const char* name = p + 28;
    const char* ne = strchr(name, ']');
    if (!ne) return false;
    player.assign(name, ne);
    out.item.assign(c + 2, ce);
    out.port.assign(port + 5, pe);
    return true;
}

// The head the look attached: the last run of head-port attachments from a Head_ItemPort line on, after `from`.
static bool ReadAttachedHead(const char* logPath, long long from, std::vector<Attached>& head) {
    FILE* f = _fsopen(logPath, "rb", _SH_DENYNO);
    if (!f) return false;
    _fseeki64(f, from, SEEK_SET);
    std::vector<Attached> current;
    std::string headPlayer, player;
    bool inHead = false;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        Attached at;
        if (!ParseAttachment(line, player, at)) continue;
        if (at.port == "Head_ItemPort") { current.assign(1, at); headPlayer = player; inHead = true; continue; }
        if (!inHead) continue;
        if (player == headPlayer && IsHeadPort(at.port)) current.push_back(at);
        else { inHead = false; head = current; }
    }
    fclose(f);
    if (inHead) head = current;
    return !head.empty();
}

static std::string HeadNode(const std::vector<Attached>& head, size_t i) {
    std::string children;
    for (size_t j = 1; j < head.size(); ++j)
        if (j != i && head[i].port == HeadParent(head[j].port)) children += HeadNode(head, j);
    std::string s = "<Item portName=\"" + head[i].port + "\" itemName=\"" + head[i].item + "\"";
    return children.empty() ? s + "/>" : s + "><Items>" + children + "</Items></Item>";
}

static int       g_dressState = 0;   // 0 idle, 1 look applied, waiting for its head in Game.log
static DWORD     g_dressSince = 0;
static long long g_dressFrom = 0;

static void StartDressing(DWORD now) {
    char logPath[MAX_PATH];
    g_dressFrom = GameLogPath(logPath, sizeof(logPath)) ? FileSize(logPath) : -1;
    if (!Apply()) return;
    g_dressState = g_dressFrom >= 0 ? 1 : 0;
    g_dressSince = now;
}

static void ContinueDressing(DWORD now) {
    if (g_dressState != 1 || now - g_dressSince < 3000) return;
    char logPath[MAX_PATH];
    std::vector<Attached> head;
    const bool found = GameLogPath(logPath, sizeof(logPath)) && ReadAttachedHead(logPath, g_dressFrom, head);
    if (!found && now - g_dressSince < 10000) return;   // keep looking for a while
    g_dressState = 0;
    if (found) {
        g_headXml = HeadNode(head, 0);
        const char* hair = "no hair";
        for (const Attached& a : head)
            if (a.port == "Hair_ItemPort") hair = a.item.c_str();
        Log("[char] the look's head has %zu items (%s); putting the outfit on with it", head.size(), hair);
    } else {
        Log("[char] couldn't find the look's head in Game.log; the outfit gets the stock head");
    }
    if (!Loadout_EquipOutfit()) Log("[char] no loadout loader; the look's loadout stays as it is");
}

void Character_NewSession() { g_dressState = 0; }

void ProcessCharacter(DWORD now) {
    EnsureLoaded();
    CollectCreatedLook(now);
    static DWORD liveSince = 0, applyAt = 0;
    static uintptr_t lastEntity = 0;
    uintptr_t actor = 0, entity = 0;
    bool live = false;
    __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // The main menu (SC_Frontend) has a player too; leave it alone, it's where the game's own creator runs.
    // Without the spawn picker hook we can't tell, so then any live player counts.
    if (Spawn_Available() && !Spawn_InUniverse()) live = false;
    InterlockedExchange(&g_inGame, live ? 1 : 0);
    if (!live) { liveSince = applyAt = 0; lastEntity = 0; g_dressState = 0; return; }
    if (entity != lastEntity) { lastEntity = entity; liveSince = 0; g_dressState = 0; }   // a new body: spawn or respawn

    AcquireSRWLockExclusive(&g_lock);
    const bool autoApply = g_auto;
    const bool haveLook = g_choice[0] != 0;
    bool requested = g_applyRequested;
    g_applyRequested = false;
    ReleaseSRWLockExclusive(&g_lock);
    if (!haveLook) g_headXml.clear();   // the game's default look: gear gets the stock head again

    // Each spawn: wait for the body to finish loading before changing it.
    if (!liveSince) { liveSince = now; if (autoApply) applyAt = now + 8000; }
    if (applyAt && static_cast<LONG>(now - applyAt) >= 0) { applyAt = 0; requested = true; }
    if (requested && haveLook && g_dressState == 0) StartDressing(now);
    ContinueDressing(now);
}

// ---- Lobby side ----------------------------------------------------------------------------------

int Character_LookCount() {
    EnsureLoaded();
    AcquireSRWLockShared(&g_lock);
    const int n = kPresetCount + static_cast<int>(g_userLooks.size());
    ReleaseSRWLockShared(&g_lock);
    return n;
}

bool Character_Look(int index, CharacterLook& out) {
    if (index < 0) return false;
    if (index < kPresetCount) {
        strcpy_s(out.id, kPresets[index].id);
        strcpy_s(out.title, kPresets[index].title);
        strcpy_s(out.detail, kPresets[index].detail);
        return true;
    }
    AcquireSRWLockShared(&g_lock);
    const size_t i = static_cast<size_t>(index - kPresetCount);
    const bool ok = i < g_userLooks.size();
    if (ok) {
        strncpy_s(out.id, g_userLooks[i].c_str(), _TRUNCATE);
        const size_t dot = g_userLooks[i].rfind('.');
        std::string title = g_userLooks[i].substr(0, dot == std::string::npos ? g_userLooks[i].size() : dot);
        strncpy_s(out.title, title.c_str(), _TRUNCATE);
        strcpy_s(out.detail, "Your file in data\\characters");
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void Character_GetChoice(char* id, size_t n, bool& autoApply) {
    EnsureLoaded();
    AcquireSRWLockShared(&g_lock);
    strncpy_s(id, n, g_choice, _TRUNCATE);
    autoApply = g_auto;
    ReleaseSRWLockShared(&g_lock);
}

void Character_Choose(const char* id) {
    AcquireSRWLockExclusive(&g_lock);
    strncpy_s(g_choice, id ? id : "", _TRUNCATE);
    SaveChoice();
    ReleaseSRWLockExclusive(&g_lock);
    Log("[char] look: %s", id && *id ? id : "the game's default");
}

void Character_SetAuto(bool autoApply) {
    AcquireSRWLockExclusive(&g_lock);
    g_auto = autoApply;
    SaveChoice();
    ReleaseSRWLockExclusive(&g_lock);
}

void Character_ApplyNow() {
    AcquireSRWLockExclusive(&g_lock);
    g_applyRequested = true;
    ReleaseSRWLockExclusive(&g_lock);
}

void Character_RescanLooks() {
    EnsureLoaded();
    AcquireSRWLockExclusive(&g_lock);
    ScanUserLooks();
    ReleaseSRWLockExclusive(&g_lock);
}

bool Character_InGame() { return g_inGame != 0; }

void Character_GetStatus(char* out, size_t n) {
    AcquireSRWLockShared(&g_lock);
    strncpy_s(out, n, g_status, _TRUNCATE);
    ReleaseSRWLockShared(&g_lock);
}
