#include "lobby.h"
#include "spawnpicker.h"
#include "character.h"
#include "session.h"
#include "net.h"
#include <d3d11.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_impl_win32.h"
#include "third_party/imgui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")

// Each UI thread (M menu, lobby) has its own ImGui context; see imconfig.h.
thread_local ImGuiContext* g_ImGuiTls = nullptr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND                    g_game;
static HWND                    g_wnd;
static ID3D11Device*           g_device;
static ID3D11DeviceContext*    g_context;
static IDXGISwapChain*         g_swap;
static ID3D11RenderTargetView* g_rtv;
static UINT                    g_resizeW, g_resizeH;
static volatile bool           g_running = false;

bool Lobby_IsRunning() { return g_running; }

// ---- Window and device ---------------------------------------------------------------------------

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void ReleaseRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDevice() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static LRESULT CALLBACK LobbyWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { g_resizeW = LOWORD(l); g_resizeH = HIWORD(l); }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

static bool OurProcessHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool g_clipped = false;

static void KeepCursorInside() {
    if (GetForegroundWindow() != g_wnd) {
        if (g_clipped) { ClipCursor(nullptr); g_clipped = false; }
        return;
    }
    RECT r = {};
    GetWindowRect(g_wnd, &r);
    ClipCursor(&r);
    g_clipped = true;
}

// Covers the game window's client area.
static void ShowLobby(bool show) {
    if (show) {
        RECT r = {};
        GetClientRect(g_game, &r);
        POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
        ClientToScreen(g_game, &tl);
        ClientToScreen(g_game, &br);
        if (br.x - tl.x < 640 || br.y - tl.y < 480) { tl = { 0, 0 }; br = { 1280, 800 }; }
        SetWindowPos(g_wnd, HWND_TOPMOST, tl.x, tl.y, br.x - tl.x, br.y - tl.y, SWP_SHOWWINDOW);
        SetForegroundWindow(g_wnd);
        SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
        KeepCursorInside();
    } else {
        ClipCursor(nullptr);
        g_clipped = false;
        ShowWindow(g_wnd, SW_HIDE);
        SetForegroundWindow(g_game);
    }
}

// ---- Look ----------------------------------------------------------------------------------------

static ImFont* g_body = nullptr;   // Segoe UI (falls back to ImGui's default font)
static ImFont* g_bold = nullptr;   // Segoe UI Bold
static float   g_ui = 1.0f;        // scale for a 1080p-high window

static float S(float v) { return v * g_ui; }

static const ImU32 kAccent     = IM_COL32(64, 160, 255, 255);
static const ImU32 kAccentSoft = IM_COL32(64, 160, 255, 40);
static const ImU32 kPanel      = IM_COL32(255, 255, 255, 10);
static const ImU32 kPanelHover = IM_COL32(255, 255, 255, 22);
static const ImU32 kLine       = IM_COL32(255, 255, 255, 24);
static const ImU32 kText       = IM_COL32(230, 236, 245, 255);
static const ImU32 kDim        = IM_COL32(140, 154, 175, 255);
static const ImU32 kGood       = IM_COL32(80, 200, 120, 255);
static const ImU32 kWarn       = IM_COL32(235, 170, 60, 255);

static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    char dir[MAX_PATH], path[MAX_PATH];
    if (GetWindowsDirectoryA(dir, sizeof(dir))) {
        snprintf(path, sizeof(path), "%s\\Fonts\\segoeui.ttf", dir);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) g_body = io.Fonts->AddFontFromFileTTF(path, 20.0f);
        snprintf(path, sizeof(path), "%s\\Fonts\\segoeuib.ttf", dir);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) g_bold = io.Fonts->AddFontFromFileTTF(path, 20.0f);
    }
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    if (!g_bold) g_bold = g_body;
    io.FontDefault = g_body;
}

static void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 0;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text]          = ImGui::ColorConvertU32ToFloat4(kText);
    c[ImGuiCol_TextDisabled]  = ImGui::ColorConvertU32ToFloat4(kDim);
    c[ImGuiCol_Button]        = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ButtonActive]  = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_FrameBg]       = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_CheckMark]     = ImGui::ColorConvertU32ToFloat4(kAccent);
}

static void Text(ImFont* font, float size, ImU32 col, ImVec2 pos, const char* text) {
    ImGui::GetWindowDrawList()->AddText(font, S(size), pos, col, text);
}

static ImVec2 TextSize(ImFont* font, float size, const char* text) {
    return font->CalcTextSizeA(S(size), FLT_MAX, 0.0f, text);
}

// A big accent (primary) or plain button in the bold font.
static bool BigButton(const char* label, ImVec2 size, bool primary, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.72f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.52f, 0.92f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.02f, 0.05f, 0.10f, 1.0f));
    }
    ImGui::PushFont(g_bold, S(20));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopFont();
    if (primary) ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    return clicked;
}

// ---- Pages ---------------------------------------------------------------------------------------

enum Page { Page_Spawn, Page_Character, Page_Multiplayer };
static int  g_page = Page_Spawn;
static int  g_system = SpawnSys_Stanton;
static char g_selected[128] = "";

static bool NavItem(const char* id, const char* label, const char* sub, bool active, bool enabled, float width) {
    const ImVec2 p = ImGui::GetCursorScreenPos(), size(width, S(64));
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || (hovered && enabled)) dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), active ? kAccentSoft : kPanelHover, S(8));
    if (active) dl->AddRectFilled(p, ImVec2(p.x + S(4), p.y + size.y), kAccent, S(2));
    Text(g_bold, 20, enabled ? kText : kDim, ImVec2(p.x + S(18), p.y + S(10)), label);
    Text(g_body, 16, kDim, ImVec2(p.x + S(18), p.y + S(36)), sub);
    ImGui::Dummy(ImVec2(0, S(6)));
    return clicked && enabled;
}

// A selectable tile: title, detail line and an optional coloured badge. Returns 1 on click, 2 on double click.
static int Card(const char* id, const char* title, const char* detail, const char* badge, ImU32 badgeCol, bool selected, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool dbl = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, selected ? kAccentSoft : hovered ? kPanelHover : kPanel, S(10));
    dl->AddRect(p, q, selected ? kAccent : kLine, S(10), 0, selected ? S(2) : S(1));
    Text(g_bold, 26, kText, ImVec2(p.x + S(20), p.y + S(18)), title);
    Text(g_body, 18, kDim, ImVec2(p.x + S(20), p.y + S(54)), detail);
    if (badge) {
        const ImVec2 b(p.x + S(20), q.y - S(34));
        dl->AddCircleFilled(ImVec2(b.x + S(5), b.y + S(11)), S(4), badgeCol);
        Text(g_body, 16, badgeCol, ImVec2(b.x + S(16), b.y), badge);
    }
    if (dbl) return 2;
    return clicked ? 1 : 0;
}

static int LocationCard(const SpawnFeatured& f, bool selected, ImVec2 size) {
    const bool found = Spawn_PointsSeen(f.name) > 0;
    return Card(f.name, f.title, f.where, found ? "Spawn points found" : "Not checked yet", found ? kGood : kWarn, selected, size);
}

// Lays out tiles in rows: Next() before each tile, Done() after it.
struct CardGrid {
    float  gap;
    int    perRow, column = 0;
    ImVec2 size;
    CardGrid(float width, float height, float gapPx) : gap(gapPx) {
        perRow = width > S(900) ? 3 : 2;
        size = ImVec2((width - gap * (perRow - 1)) / perRow, height);
    }
    void Next() { if (column) ImGui::SameLine(0, gap); }
    void Done() {
        column = (column + 1) % perRow;
        if (!column) ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    }
};

static const char* FeaturedTitle(const char* name) {
    for (int i = 0; i < Spawn_FeaturedCount(); ++i)
        if (_stricmp(Spawn_Featured(i).name, name) == 0) return Spawn_Featured(i).title;
    return nullptr;
}

// Returns true when the lobby should close.
static bool DrawSpawnPage(bool waiting) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    Text(g_bold, 38, kText, origin, waiting ? "Choose where to spawn" : "Where you spawn");
    Text(g_body, 18, kDim, ImVec2(origin.x, origin.y + S(50)),
         waiting ? "The game is waiting for you. Pick a location and press Spawn."
                 : "Used the next time the game loads (and maybe when you respawn - not tested yet).");
    ImGui::Dummy(ImVec2(0, S(92)));

    if (!Spawn_Available()) {
        ImGui::TextWrapped("The spawn picker isn't available in this game version (see mod.log).");
        return false;
    }

    // System tabs
    static const char* const kSystems[SpawnSys_Count] = { "STANTON", "PYRO", "NYX" };
    for (int i = 0; i < SpawnSys_Count; ++i) {
        if (i) ImGui::SameLine(0, S(10));
        const bool active = g_system == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccentSoft));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(active ? kAccent : kDim));
        ImGui::PushFont(g_bold, S(18));
        if (ImGui::Button(kSystems[i], ImVec2(S(150), S(44)))) g_system = i;
        ImGui::PopFont();
        ImGui::PopStyleColor(active ? 2 : 1);
    }
    ImGui::Dummy(ImVec2(0, S(10)));

    // Location cards
    bool close = false;
    CardGrid grid(width, S(150), S(16));
    for (int i = 0; i < Spawn_FeaturedCount(); ++i) {
        const SpawnFeatured& f = Spawn_Featured(i);
        if (f.system != g_system) continue;
        grid.Next();
        const int hit = LocationCard(f, _stricmp(f.name, g_selected) == 0, grid.size);
        if (hit) strcpy_s(g_selected, f.name);
        if (hit == 2) { Spawn_Choose(g_selected); close = waiting; }
        grid.Done();
    }

    // Footer: choice, buttons, options
    char current[128];
    bool ask = true;
    Spawn_GetChoice(current, sizeof(current), ask);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - S(170)));
    ImGui::GetWindowDrawList()->AddLine(ImGui::GetCursorScreenPos(), ImVec2(origin.x + width, ImGui::GetCursorScreenPos().y), kLine);
    ImGui::Dummy(ImVec2(0, S(14)));

    const char* currentTitle = current[0] ? FeaturedTitle(current) : nullptr;
    char line[256];
    if (!current[0]) snprintf(line, sizeof(line), "Saved choice: the game's own spawn");
    else if (currentTitle) snprintf(line, sizeof(line), "Saved choice: %s", currentTitle);
    else snprintf(line, sizeof(line), "Saved choice: %s (custom, from spawn_location.txt)", current);
    ImGui::TextDisabled("%s", line);

    const char* selTitle = g_selected[0] ? FeaturedTitle(g_selected) : nullptr;
    char label[160];
    if (!selTitle) strcpy_s(label, waiting ? "SPAWN" : "USE THIS LOCATION");
    else if (waiting) snprintf(label, sizeof(label), "SPAWN AT %s", selTitle);
    else snprintf(label, sizeof(label), "USE %s NEXT TIME", selTitle);
    for (char* c = label; *c; ++c) *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    if (BigButton(label, ImVec2(S(380), S(56)), true, selTitle != nullptr)) { Spawn_Choose(g_selected); close = waiting; }
    ImGui::SameLine(0, S(12));
    if (BigButton("GAME'S OWN SPAWN", ImVec2(S(240), S(56)), false)) { g_selected[0] = 0; Spawn_Choose(""); close = waiting; }
    ImGui::SameLine(0, S(24));
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, S(12)));
    if (ImGui::Checkbox("Ask every time the game loads", &ask)) Spawn_SetAsk(ask);
    ImGui::EndGroup();

    char status[256];
    Spawn_GetStatus(status, sizeof(status));
    if (status[0]) ImGui::TextDisabled("%s", status);
    return close;
}

static char g_lookSelected[128] = "";

// Returns true when the lobby should close.
static bool DrawCharacterPage() {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool inGame = Character_InGame();
    Text(g_bold, 38, kText, origin, "Character");
    Text(g_body, 18, kDim, ImVec2(origin.x, origin.y + S(50)),
         "Your look is put on with the game's own appearance loader, again after every spawn and gear change.");
    ImGui::Dummy(ImVec2(0, S(92)));

    char current[128];
    bool autoApply = true;
    Character_GetChoice(current, sizeof(current), autoApply);

    CardGrid grid(width, S(130), S(16));
    char currentTitle[64] = "";
    CharacterLook sel = {};
    bool haveSel = false;
    for (int i = 0; i < Character_LookCount(); ++i) {
        CharacterLook look;
        if (!Character_Look(i, look)) continue;
        if (_stricmp(look.id, current) == 0) strcpy_s(currentTitle, look.title);
        const bool isSel = _stricmp(look.id, g_lookSelected) == 0;
        if (isSel) { sel = look; haveSel = true; }
        grid.Next();
        const int hit = Card(look.id, look.title, look.detail, nullptr, 0, isSel, grid.size);
        if (hit) strcpy_s(g_lookSelected, look.id);
        if (hit == 2) { Character_Choose(g_lookSelected); if (inGame) Character_ApplyNow(); }
        grid.Done();
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::TextDisabled("Looks you save in the game's creator land in data\\characters and show up here (reopen the lobby).");

    // The native creator (next step)
    ImGui::Dummy(ImVec2(0, S(18)));
    const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + width, p.y + S(96));
    ImGui::GetWindowDrawList()->AddRectFilled(p, q, kPanel, S(10));
    bool close = false;
    const bool inUniverse = Spawn_InUniverse();
    Text(g_bold, 22, kText, ImVec2(p.x + S(20), p.y + S(16)), "Character creator");
    Text(g_body, 17, kDim, ImVec2(p.x + S(20), p.y + S(52)),
         inUniverse && !inGame ? "Available once you've spawned (the game ignores it while the universe loads)."
         : inUniverse ? "Opens the game's own creator in its main menu. Make your character, save, then F9 > Back to the universe."
                    : "In the main menu: Character Customization, make your character and save. Then come back here.");
    ImGui::SetCursorScreenPos(ImVec2(q.x - S(300), p.y + S(24)));
    // While the universe is still loading the game answers "RequestFrontEnd already in progress" and ignores it.
    const bool canSwitch = Session_Available() && (!inUniverse || inGame);
    if (BigButton(inUniverse ? "OPEN CREATOR" : "BACK TO THE UNIVERSE", ImVec2(S(280), S(48)), true, canSwitch)) {
        if (inUniverse) Session_OpenCreator(); else Session_ReturnToUniverse();
        close = true;
    }

    // Footer
    ImGui::SetCursorScreenPos(ImVec2(origin.x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - S(170)));
    ImGui::GetWindowDrawList()->AddLine(ImGui::GetCursorScreenPos(), ImVec2(origin.x + width, ImGui::GetCursorScreenPos().y), kLine);
    ImGui::Dummy(ImVec2(0, S(14)));
    if (!current[0]) ImGui::TextDisabled("Saved look: the game's default");
    else ImGui::TextDisabled("Saved look: %s", currentTitle[0] ? currentTitle : current);

    char label[160];
    if (!haveSel) strcpy_s(label, "USE THIS LOOK");
    else if (inGame) snprintf(label, sizeof(label), "WEAR %s NOW", sel.title);
    else snprintf(label, sizeof(label), "USE %s", sel.title);
    for (char* c = label; *c; ++c) *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    if (BigButton(label, ImVec2(S(380), S(56)), true, haveSel)) { Character_Choose(g_lookSelected); if (inGame) Character_ApplyNow(); }
    ImGui::SameLine(0, S(12));
    if (BigButton("GAME'S DEFAULT LOOK", ImVec2(S(260), S(56)), false)) { g_lookSelected[0] = 0; Character_Choose(""); }
    ImGui::SetItemTooltip("Stops applying a look. Your current look stays until you respawn.");
    ImGui::SameLine(0, S(24));
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, S(12)));
    if (ImGui::Checkbox("Put it on after every spawn", &autoApply)) Character_SetAuto(autoApply);
    ImGui::EndGroup();
    char status[256];
    Character_GetStatus(status, sizeof(status));
    if (status[0]) ImGui::TextDisabled("%s", status);
    else if (!inGame) ImGui::TextDisabled("You're not in game yet: the look goes on a few seconds after you spawn.");
    return close;
}

static void SectionTitle(const char* text) {
    ImGui::PushFont(g_bold, S(20));
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

static void DrawMultiplayerPage() {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    Text(g_bold, 38, kText, origin, "Multiplayer");
    Text(g_body, 18, kDim, ImVec2(origin.x, origin.y + S(50)),
         "Sessions between ChrisWareOffline players on your network. Hosting, joining and chat work now; "
         "seeing each other in the universe comes later.");
    ImGui::Dummy(ImVec2(0, S(92)));

    // Your name (saved when you finish editing it)
    static char name[32] = "";
    static bool nameLoaded = false;
    if (!nameLoaded) { Net_GetName(name, sizeof(name)); nameLoaded = name[0] != 0; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Your name");
    ImGui::SameLine(S(130));
    ImGui::SetNextItemWidth(S(320));
    ImGui::InputText("##mpName", name, sizeof(name));
    if (ImGui::IsItemDeactivatedAfterEdit()) { Net_SetName(name); Net_GetName(name, sizeof(name)); }
    ImGui::Dummy(ImVec2(0, S(10)));

    const int mode = Net_Mode();
    const float listH = ImGui::GetContentRegionAvail().y - S(60);
    if (mode == NetMode_Idle) {
        static bool searched = false;
        if (!searched) { searched = true; Net_Refresh(); }
        const float col = (width - S(24)) * 0.5f;

        ImGui::BeginChild("##mpHost", ImVec2(col, listH), ImGuiChildFlags_Borders);
        SectionTitle("Host a session");
        ImGui::TextDisabled("Others on your network can find and join it.");
        static int port = kNetPort;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine(S(80));
        ImGui::SetNextItemWidth(S(140));
        ImGui::InputInt("##mpPort", &port, 0);
        if (port < 1 || port > 65535) port = kNetPort;
        ImGui::Dummy(ImVec2(0, S(8)));
        if (BigButton("HOST", ImVec2(S(200), S(48)), true)) Net_Host(port);
        ImGui::TextDisabled("Windows may ask to allow the game through the firewall.");
        ImGui::EndChild();

        ImGui::SameLine(0, S(24));
        ImGui::BeginChild("##mpJoin", ImVec2(col, listH), ImGuiChildFlags_Borders);
        SectionTitle("Join a session");
        static char address[64] = "";
        static int picked = -1;
        if (BigButton("REFRESH", ImVec2(S(140), S(36)), false)) { picked = -1; Net_Refresh(); }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Sessions on your network:");
        const int sessions = Net_SessionCount();
        ImGui::BeginChild("##mpSessions", ImVec2(0, S(180)), ImGuiChildFlags_Borders);
        if (!sessions) ImGui::TextDisabled("None found yet.");
        for (int i = 0; i < sessions; ++i) {
            NetSession ns;
            if (!Net_Session(i, ns)) continue;
            char label[160];
            snprintf(label, sizeof(label), "%s   (%d player%s)   %s##s%d", ns.host, ns.players, ns.players == 1 ? "" : "s", ns.address, i);
            if (ImGui::Selectable(label, picked == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                picked = i;
                strcpy_s(address, ns.address);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Net_Join(address);
            }
        }
        ImGui::EndChild();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Address");
        ImGui::SameLine(S(100));
        ImGui::SetNextItemWidth(S(260));
        ImGui::InputTextWithHint("##mpAddress", "192.168.1.20 or ip:port", address, sizeof(address));
        ImGui::SameLine();
        if (BigButton("JOIN", ImVec2(S(120), 0), true, address[0] != 0)) Net_Join(address);
        ImGui::EndChild();
    } else {
        // In a session: players | chat
        if (BigButton("LEAVE", ImVec2(S(160), S(40)), false)) Net_Leave();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(mode == NetMode_Hosting ? "You're hosting." : mode == NetMode_Joined ? "You're in a session." : "Connecting...");
        const float h = listH - S(50);
        ImGui::BeginChild("##mpPlayers", ImVec2(S(280), h), ImGuiChildFlags_Borders);
        SectionTitle("Players");
        static NetPlayerState where[16];
        const int located = Net_RemoteStates(where, 16);
        for (int i = 0; i < Net_PlayerCount(); ++i) {
            char player[64];
            if (!Net_Player(i, player, sizeof(player))) continue;
            ImGui::Text("%s%s", player, i == 0 ? "   (host)" : "");
            for (int j = 0; j < located; ++j)   // where they are: their innermost zone
                if (strcmp(where[j].name, player) == 0)
                    ImGui::TextDisabled("    %s", where[j].zones && where[j].ageMs < 3000 ? where[j].z[0].zone : "not in the universe");
        }
        ImGui::EndChild();
        ImGui::SameLine(0, S(16));
        ImGui::BeginGroup();
        ImGui::BeginChild("##mpChat", ImVec2(0, h - S(50)), ImGuiChildFlags_Borders);
        static int lastCount = 0;
        const int count = Net_ChatCount();
        for (int i = 0; i < count; ++i) {
            char line[512];
            if (Net_ChatLine(i, line, sizeof(line))) ImGui::TextWrapped("%s", line);
        }
        if (count != lastCount) { ImGui::SetScrollHereY(1.0f); lastCount = count; }
        ImGui::EndChild();
        static char message[256] = "";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(130));
        const bool enter = ImGui::InputTextWithHint("##mpMessage", "Say something...", message, sizeof(message), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((BigButton("SEND", ImVec2(S(120), 0), true, message[0] != 0) || enter) && message[0]) {
            Net_SendChat(message);
            message[0] = 0;
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::EndGroup();
    }
    char status[256];
    Net_GetStatus(status, sizeof(status));
    if (status[0]) ImGui::TextDisabled("%s", status);
}

// Returns true when the lobby should close.
static bool DrawLobby(bool waiting) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    g_ui = size.y / 1080.0f;
    if (g_ui < 0.9f) g_ui = 0.9f;
    ImGui::GetStyle().FontScaleMain = 1.0f;

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(8, 12, 22, 255), IM_COL32(10, 16, 30, 255),
                                IM_COL32(14, 24, 44, 255), IM_COL32(8, 12, 22, 255));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##lobby", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    ImGui::PushFont(g_body, S(18));

    // Top bar
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float barH = S(72), rail = S(300), pad = S(40);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(size.x, barH), IM_COL32(0, 0, 0, 90));
    dl->AddLine(ImVec2(0, barH), ImVec2(size.x, barH), kLine);
    Text(g_bold, 26, kText, ImVec2(pad, S(20)), "CHRISWARE");
    Text(g_bold, 26, kAccent, ImVec2(pad + TextSize(g_bold, 26, "CHRISWARE ").x, S(20)), "OFFLINE");
    const char* state = waiting ? "Loading - waiting for your spawn choice" : "In game  -  F9 or Esc to close";
    Text(g_body, 16, kDim, ImVec2(size.x - pad - TextSize(g_body, 16, state).x, S(28)), state);

    // Left rail
    dl->AddRectFilled(ImVec2(0, barH), ImVec2(rail, size.y), IM_COL32(0, 0, 0, 60));
    dl->AddLine(ImVec2(rail, barH), ImVec2(rail, size.y), kLine);
    ImGui::SetCursorScreenPos(ImVec2(S(16), barH + S(24)));
    ImGui::BeginGroup();
    const float navW = rail - S(32);
    if (NavItem("##nav_spawn", "Spawn", "Where you start", g_page == Page_Spawn, true, navW)) g_page = Page_Spawn;
    if (NavItem("##nav_char", "Character", "Your look", g_page == Page_Character, true, navW)) g_page = Page_Character;
    if (NavItem("##nav_mp", "Multiplayer", "Local sessions", g_page == Page_Multiplayer, true, navW)) g_page = Page_Multiplayer;
    ImGui::EndGroup();

    // Page
    bool close = false;
    ImGui::SetCursorScreenPos(ImVec2(rail + pad, barH + pad));
    ImGui::BeginChild("##page", ImVec2(size.x - rail - pad * 2, size.y - barH - pad * 1.5f), 0, ImGuiWindowFlags_NoBackground);
    switch (g_page) {
    case Page_Spawn: close = DrawSpawnPage(waiting); break;
    case Page_Character: close = DrawCharacterPage(); break;
    case Page_Multiplayer: DrawMultiplayerPage(); break;
    }
    ImGui::EndChild();

    ImGui::PopFont();
    ImGui::End();
    return close;
}

// ---- Thread --------------------------------------------------------------------------------------

// Picks the system tab and card for the saved choice.
static void SelectSavedChoice() {
    bool ask, autoApply;
    Character_RescanLooks();
    Character_GetChoice(g_lookSelected, sizeof(g_lookSelected), autoApply);
    Spawn_GetChoice(g_selected, sizeof(g_selected), ask);
    for (int i = 0; i < Spawn_FeaturedCount(); ++i)
        if (_stricmp(Spawn_Featured(i).name, g_selected) == 0) g_system = Spawn_Featured(i).system;
}

// Hiding the lobby while the game waits keeps the saved choice and lets the game carry on.
static void ReleasePicker() {
    if (!Spawn_PickerActive()) return;
    char current[128];
    bool ask;
    Spawn_GetChoice(current, sizeof(current), ask);
    Spawn_Choose(current);
}

static DWORD WINAPI LobbyThread(LPVOID) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = LobbyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"chriswareoffline_lobby";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"ChrisWare Lobby", WS_POPUP,
                            0, 0, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_wnd || !CreateDevice()) return 0;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().MouseDrawCursor = true;
    LoadFonts();
    ApplyStyle();
    ImGui_ImplWin32_Init(g_wnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    Net_Start();

    g_running = true;
    bool visible = false, wasF9 = false;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const bool waiting = Spawn_PickerActive();
        if (waiting && !visible) { visible = true; SelectSavedChoice(); g_page = Page_Spawn; ShowLobby(true); }
        const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !wasF9 && !waiting && OurProcessHasFocus()) {
            visible = !visible;
            if (visible) SelectSavedChoice();
            ShowLobby(visible);
        }
        wasF9 = f9;
        if (visible && !IsWindowVisible(g_wnd)) { visible = false; ClipCursor(nullptr); g_clipped = false; ReleasePicker(); }
        if (!visible) { Sleep(50); continue; }
        KeepCursorInside();

        if (g_resizeW && g_resizeH) {
            ReleaseRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        bool close = DrawLobby(waiting);
        if (!waiting && ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
        ImGui::Render();
        const float clear[4] = { 0.03f, 0.05f, 0.09f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);

        if (close) { ReleasePicker(); visible = false; ShowLobby(false); }
    }
}

void Lobby_Start(HWND gameWindow) {
    g_game = gameWindow;
    if (HANDLE t = CreateThread(nullptr, 0, LobbyThread, nullptr, 0, nullptr)) CloseHandle(t);
}
