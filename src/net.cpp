#include <winsock2.h>
#include <ws2tcpip.h>
#include "net.h"
#include "common.h"
#include <share.h>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

// Protocol (TCP, one "VERB payload" line per message, \n-terminated):
//   joiner -> host:  HELLO <version> <name>      CHAT <text>                 STATE <position>
//   host -> joiner:  WELCOME <host name>          PLAYERS <name>|<name>|...   CHAT <name>|<text>   REFUSE <reason>
//                    PSTATE <name>|<position>     (every player's position, the host's own included)
// <position> = <zone>,<x>,<y>,<z>|<zone>,... innermost zone first, or "-" when not in the universe.
// LAN discovery (UDP broadcast to kNetDiscoveryPort): "CWO?"  ->  "CWO! <port> <players> <host name>"
static const char* const kVersion = "CWO2";
constexpr size_t kMaxName = 24, kMaxChat = 200, kMaxLine = 512;

// ---- State the lobby reads (g_lock) --------------------------------------------------------------
static SRWLOCK                  g_lock = SRWLOCK_INIT;
static int                      g_mode = NetMode_Idle;
static std::string              g_name, g_status;
static std::vector<NetSession>  g_sessions;
static std::vector<std::string> g_players, g_chat;

struct RemoteState { std::string name, position; DWORD received; };
static std::vector<RemoteState> g_remote;          // other players' latest positions
static std::string              g_localPosition;   // ours, set by the game thread ("" = none yet)

enum Command { Cmd_Host, Cmd_Join, Cmd_Leave, Cmd_Refresh, Cmd_Chat };
static std::vector<std::pair<int, std::string>> g_commands;

static void SetStatus(const std::string& text) {
    AcquireSRWLockExclusive(&g_lock);
    g_status = text;
    ReleaseSRWLockExclusive(&g_lock);
    Log("[net] %s", text.c_str());
}

static void AddChat(const std::string& line) {
    AcquireSRWLockExclusive(&g_lock);
    g_chat.push_back(line);
    if (g_chat.size() > kMaxChat) g_chat.erase(g_chat.begin());
    ReleaseSRWLockExclusive(&g_lock);
}

static void SetMode(int mode) {
    AcquireSRWLockExclusive(&g_lock);
    g_mode = mode;
    ReleaseSRWLockExclusive(&g_lock);
}

static std::string MyName() {
    AcquireSRWLockShared(&g_lock);
    std::string n = g_name;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

// Names travel inside "a|b|c" lists and lines: no separators, no control characters, bounded length.
static std::string CleanName(const std::string& in) {
    std::string out;
    for (char c : in)
        if (static_cast<unsigned char>(c) >= 32 && c != '|' && out.size() < kMaxName) out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? "Player" : out;
}

static std::string CleanText(const std::string& in) {
    std::string out;
    for (char c : in)
        if (static_cast<unsigned char>(c) >= 32 && out.size() < 300) out += c;
    return out;
}

// ---- data\multiplayer.txt ------------------------------------------------------------------------

static void LoadName() {
    char user[64] = "Player";
    DWORD n = sizeof(user);
    GetUserNameA(user, &n);
    std::string name = user;
    char path[MAX_PATH];
    if (FILE* f = DataFilePath("multiplayer.txt", path, sizeof(path)) ? _fsopen(path, "r", _SH_DENYNO) : nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strncmp(line, "name ", 5) == 0) name = line + 5;
        }
        fclose(f);
    }
    AcquireSRWLockExclusive(&g_lock);
    g_name = CleanName(name);
    ReleaseSRWLockExclusive(&g_lock);
}

static void SaveName(const std::string& name) {
    char path[MAX_PATH];
    FILE* f = DataFilePath("multiplayer.txt", path, sizeof(path)) ? _fsopen(path, "w", _SH_DENYNO) : nullptr;
    if (!f) return;
    fprintf(f, "# ChrisWareOffline multiplayer settings\nname %s\n", name.c_str());
    fclose(f);
}

// ---- Sockets (network thread only) ---------------------------------------------------------------

struct Conn { SOCKET s = INVALID_SOCKET; std::string in, name; bool hello = false; };

static SOCKET            g_listen = INVALID_SOCKET;     // host: TCP
static SOCKET            g_answer = INVALID_SOCKET;     // host: UDP discovery answers
static SOCKET            g_server = INVALID_SOCKET;     // joiner: connection to the host
static SOCKET            g_search = INVALID_SOCKET;     // discovery requests in flight
static std::vector<Conn> g_clients;                     // host: joiners
static std::string       g_serverIn;
static int               g_hostPort = 0;
static DWORD             g_searchUntil = 0;

static void CloseSocket(SOCKET& s) {
    if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
}

static bool SendLine(SOCKET s, const std::string& line) {
    const std::string data = line + "\n";
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Reads what's there; calls onLine for each complete line. False = closed or broken.
template <typename F> static bool ReadLines(SOCKET s, std::string& buffer, F onLine) {
    char chunk[1024];
    const int n = recv(s, chunk, sizeof(chunk), 0);
    if (n <= 0) return false;
    buffer.append(chunk, static_cast<size_t>(n));
    for (size_t nl; (nl = buffer.find('\n')) != std::string::npos; ) {
        std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        onLine(line);
    }
    return buffer.size() <= kMaxLine * 4;
}

static void Split(const std::string& line, std::string& verb, std::string& rest) {
    const size_t sp = line.find(' ');
    verb = line.substr(0, sp);
    rest = sp == std::string::npos ? "" : line.substr(sp + 1);
}

// ---- Host ----------------------------------------------------------------------------------------

// Caller doesn't hold g_lock.
static void StoreRemote(const std::string& name, const std::string& position) {
    AcquireSRWLockExclusive(&g_lock);
    bool found = false;
    for (RemoteState& r : g_remote)
        if (r.name == name) { r.position = position; r.received = GetTickCount(); found = true; }
    if (!found) g_remote.push_back({ name, position, GetTickCount() });
    ReleaseSRWLockExclusive(&g_lock);
}

// Drops positions of players who aren't in `names` any more.
static void KeepRemotes(const std::vector<std::string>& names) {
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_remote.size(); ) {
        bool keep = false;
        for (const std::string& n : names) keep |= n == g_remote[i].name;
        if (keep) ++i; else g_remote.erase(g_remote.begin() + static_cast<ptrdiff_t>(i));
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static std::vector<std::string> HostPlayerNames() {
    std::vector<std::string> names = { MyName() };
    for (const Conn& c : g_clients)
        if (c.hello) names.push_back(c.name);
    return names;
}

static void HostBroadcast(const std::string& line) {
    for (Conn& c : g_clients)
        if (c.hello) SendLine(c.s, line);
}

static void HostPublishPlayers() {
    const std::vector<std::string> names = HostPlayerNames();
    std::string list;
    for (const std::string& n : names) list += (list.empty() ? "" : "|") + n;
    HostBroadcast("PLAYERS " + list);
    AcquireSRWLockExclusive(&g_lock);
    g_players = names;
    ReleaseSRWLockExclusive(&g_lock);
    KeepRemotes(names);
}

static std::string UniqueName(std::string name) {
    const std::vector<std::string> taken = HostPlayerNames();
    const std::string base = name;
    for (int i = 2; ; ++i) {
        bool clash = false;
        for (const std::string& t : taken) clash |= _stricmp(t.c_str(), name.c_str()) == 0;
        if (!clash) return name;
        name = base.substr(0, kMaxName - 3) + " " + std::to_string(i);
    }
}

static void HostOnLine(Conn& c, const std::string& line) {
    std::string verb, rest;
    Split(line, verb, rest);
    if (!c.hello) {
        std::string version, name;
        Split(rest, version, name);
        if (verb != "HELLO" || version != kVersion) {
            SendLine(c.s, "REFUSE this session runs ChrisWareOffline protocol " + std::string(kVersion));
            CloseSocket(c.s);
            return;
        }
        c.name = UniqueName(CleanName(name));
        c.hello = true;
        SendLine(c.s, "WELCOME " + MyName());
        HostPublishPlayers();
        AddChat("* " + c.name + " joined");
        HostBroadcast("CHAT *|" + c.name + " joined");
        return;
    }
    if (verb == "CHAT") {
        const std::string text = CleanText(rest);
        if (text.empty()) return;
        AddChat(c.name + ": " + text);
        HostBroadcast("CHAT " + c.name + "|" + text);
    } else if (verb == "STATE") {
        const std::string position = CleanText(rest);
        StoreRemote(c.name, position);
        for (Conn& other : g_clients)
            if (other.hello && &other != &c) SendLine(other.s, "PSTATE " + c.name + "|" + position);
    }
}

static void StartHosting(int port) {
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    char bindTo[64];   // SC_OFFLINE_NET_BIND: host on one address only (e.g. one network adapter, or 127.0.0.1)
    const DWORD bl = GetEnvironmentVariableA("SC_OFFLINE_NET_BIND", bindTo, sizeof(bindTo));
    if (bl && bl < sizeof(bindTo)) inet_pton(AF_INET, bindTo, &addr.sin_addr);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (g_listen == INVALID_SOCKET || bind(g_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(g_listen, 8) != 0) {
        SetStatus("Couldn't host on port " + std::to_string(port) + " (error " + std::to_string(WSAGetLastError()) + "). Is it in use?");
        CloseSocket(g_listen);
        return;
    }
    g_hostPort = port;
    g_answer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(g_answer, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    addr.sin_port = htons(static_cast<u_short>(kNetDiscoveryPort));
    const bool discoverable = g_answer != INVALID_SOCKET && bind(g_answer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (!discoverable) CloseSocket(g_answer);
    SetMode(NetMode_Hosting);
    AcquireSRWLockExclusive(&g_lock);
    g_players = { g_name };
    g_chat.clear();
    ReleaseSRWLockExclusive(&g_lock);
    SetStatus("Hosting on port " + std::to_string(port) + (discoverable ? "." : " (LAN discovery unavailable; others join by address)."));
}

static void AnswerDiscovery() {
    char buf[64];
    sockaddr_in from = {};
    int len = sizeof(from);
    const int n = recvfrom(g_answer, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 4 || strncmp(buf, "CWO?", 4) != 0) return;
    const std::string reply = "CWO! " + std::to_string(g_hostPort) + " " + std::to_string(HostPlayerNames().size()) + " " + MyName();
    sendto(g_answer, reply.c_str(), static_cast<int>(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), len);
}

// ---- Joiner --------------------------------------------------------------------------------------

static void JoinerOnLine(const std::string& line) {
    std::string verb, rest;
    Split(line, verb, rest);
    if (verb == "WELCOME") {
        SetMode(NetMode_Joined);
        SetStatus("Joined " + CleanName(rest) + "'s session.");
    } else if (verb == "PLAYERS") {
        std::vector<std::string> names;
        for (size_t start = 0; start <= rest.size(); ) {
            const size_t bar = rest.find('|', start);
            const std::string n = rest.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!n.empty()) names.push_back(n);
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        AcquireSRWLockExclusive(&g_lock);
        g_players = names;
        ReleaseSRWLockExclusive(&g_lock);
        KeepRemotes(names);
    } else if (verb == "PSTATE") {
        const size_t bar = rest.find('|');
        if (bar == std::string::npos) return;
        const std::string who = rest.substr(0, bar);
        if (who != MyName()) StoreRemote(who, rest.substr(bar + 1));
    } else if (verb == "CHAT") {
        const size_t bar = rest.find('|');
        if (bar == std::string::npos) return;
        const std::string who = rest.substr(0, bar), text = CleanText(rest.substr(bar + 1));
        AddChat(who == "*" ? "* " + text : who + ": " + text);
    } else if (verb == "REFUSE") {
        SetStatus("The host refused: " + CleanText(rest));
        CloseSocket(g_server);
        SetMode(NetMode_Idle);
    }
}

static bool ParseAddress(const std::string& text, sockaddr_in& addr) {
    std::string host = text;
    int port = kNetPort;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) { host = text.substr(0, colon); port = atoi(text.c_str() + colon + 1); }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (port <= 0 || port > 65535 || getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    addr = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    addr.sin_port = htons(static_cast<u_short>(port));
    freeaddrinfo(res);
    return true;
}

static void StartJoining(const std::string& address) {
    sockaddr_in addr = {};
    if (!ParseAddress(address, addr)) { SetStatus("Can't read the address '" + address + "' (use ip or ip:port)."); return; }
    g_server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nonBlocking = 1;
    ioctlsocket(g_server, FIONBIO, &nonBlocking);
    connect(g_server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    fd_set wr, ex;
    FD_ZERO(&wr); FD_ZERO(&ex);
    FD_SET(g_server, &wr); FD_SET(g_server, &ex);
    timeval tv = { 4, 0 };
    const bool connected = select(0, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(g_server, &wr);
    nonBlocking = 0;
    ioctlsocket(g_server, FIONBIO, &nonBlocking);
    if (!connected) { SetStatus("Couldn't reach " + address + "."); CloseSocket(g_server); return; }
    g_serverIn.clear();
    AcquireSRWLockExclusive(&g_lock);
    g_chat.clear();
    g_players.clear();
    ReleaseSRWLockExclusive(&g_lock);
    SetMode(NetMode_Connecting);
    SetStatus("Connecting to " + address + "...");
    SendLine(g_server, std::string("HELLO ") + kVersion + " " + MyName());
}

static void StartSearch() {
    CloseSocket(g_search);
    AcquireSRWLockExclusive(&g_lock);
    g_sessions.clear();
    ReleaseSRWLockExclusive(&g_lock);
    g_search = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(g_search, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<u_short>(kNetDiscoveryPort));
    for (const char* target : { "255.255.255.255", "127.0.0.1" }) {
        inet_pton(AF_INET, target, &to.sin_addr);
        sendto(g_search, "CWO?", 4, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    g_searchUntil = GetTickCount() + 1500;
}

static void ReadSearchReply() {
    char buf[128];
    sockaddr_in from = {};
    int len = sizeof(from);
    const int n = recvfrom(g_search, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 5 || strncmp(buf, "CWO! ", 5) != 0) return;
    buf[n] = 0;
    int port = 0, players = 0, used = 0;
    if (sscanf_s(buf + 5, "%d %d %n", &port, &players, &used) < 2) return;
    NetSession s = {};
    strncpy_s(s.host, CleanName(buf + 5 + used).c_str(), _TRUNCATE);
    char ip[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    snprintf(s.address, sizeof(s.address), "%s:%d", ip, port);
    s.players = players;
    AcquireSRWLockExclusive(&g_lock);
    bool seen = false;
    for (const NetSession& e : g_sessions) seen |= strcmp(e.address, s.address) == 0 || (strcmp(e.host, s.host) == 0 && strstr(s.address, "127.0.0.1"));
    if (!seen) g_sessions.push_back(s);
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- Thread --------------------------------------------------------------------------------------

static void LeaveSession(const char* why) {
    for (Conn& c : g_clients) CloseSocket(c.s);
    g_clients.clear();
    CloseSocket(g_listen);
    CloseSocket(g_answer);
    CloseSocket(g_server);
    AcquireSRWLockExclusive(&g_lock);
    g_players.clear();
    g_remote.clear();
    ReleaseSRWLockExclusive(&g_lock);
    SetMode(NetMode_Idle);
    if (why) SetStatus(why);
}

static void RunCommands() {
    AcquireSRWLockExclusive(&g_lock);
    std::vector<std::pair<int, std::string>> commands;
    commands.swap(g_commands);
    const int mode = g_mode;
    ReleaseSRWLockExclusive(&g_lock);
    for (const auto& [cmd, arg] : commands) {
        switch (cmd) {
        case Cmd_Host:    if (mode == NetMode_Idle) StartHosting(atoi(arg.c_str())); break;
        case Cmd_Join:    if (mode == NetMode_Idle) StartJoining(arg); break;
        case Cmd_Leave:   if (g_mode != NetMode_Idle) LeaveSession("You left the session."); break;
        case Cmd_Refresh: StartSearch(); break;
        case Cmd_Chat: {
            const std::string text = CleanText(arg);
            if (text.empty()) break;
            if (g_mode == NetMode_Hosting) { AddChat(MyName() + ": " + text); HostBroadcast("CHAT " + MyName() + "|" + text); }
            else if (g_mode == NetMode_Joined) SendLine(g_server, "CHAT " + text);
            break;
        }
        }
    }
}

static void SendPosition() {
    static DWORD nextSend = 0, nextBeat = 0;
    static std::string lastSent;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - nextSend) < 0) return;
    nextSend = now + 100;
    AcquireSRWLockShared(&g_lock);
    const std::string position = g_localPosition;
    ReleaseSRWLockShared(&g_lock);
    if (position.empty() || (position == lastSent && static_cast<LONG>(now - nextBeat) < 0)) return;
    lastSent = position;
    nextBeat = now + 1000;
    if (g_mode == NetMode_Hosting) HostBroadcast("PSTATE " + MyName() + "|" + position);
    else if (g_mode == NetMode_Joined) SendLine(g_server, "STATE " + position);
}

static DWORD WINAPI NetThread(LPVOID) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { SetStatus("Networking (Winsock) failed to start."); return 0; }
    LoadName();
    for (;;) {
        RunCommands();
        SendPosition();
        fd_set rd;
        FD_ZERO(&rd);
        for (SOCKET s : { g_listen, g_answer, g_server, g_search })
            if (s != INVALID_SOCKET) FD_SET(s, &rd);
        for (const Conn& c : g_clients) FD_SET(c.s, &rd);
        if (!rd.fd_count) { Sleep(100); continue; }
        timeval tv = { 0, 100000 };
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) {
            if (g_search != INVALID_SOCKET && static_cast<LONG>(GetTickCount() - g_searchUntil) >= 0) CloseSocket(g_search);
            continue;
        }
        if (g_listen != INVALID_SOCKET && FD_ISSET(g_listen, &rd)) {
            Conn c;
            c.s = accept(g_listen, nullptr, nullptr);
            if (c.s != INVALID_SOCKET) g_clients.push_back(c);
        }
        if (g_answer != INVALID_SOCKET && FD_ISSET(g_answer, &rd)) AnswerDiscovery();
        if (g_search != INVALID_SOCKET && FD_ISSET(g_search, &rd)) ReadSearchReply();
        if (g_search != INVALID_SOCKET && static_cast<LONG>(GetTickCount() - g_searchUntil) >= 0) CloseSocket(g_search);
        for (size_t i = 0; i < g_clients.size(); ) {
            Conn& c = g_clients[i];
            bool alive = c.s != INVALID_SOCKET;
            if (alive && FD_ISSET(c.s, &rd)) alive = ReadLines(c.s, c.in, [&](const std::string& l) { if (c.s != INVALID_SOCKET) HostOnLine(c, l); }) && c.s != INVALID_SOCKET;
            if (alive) { ++i; continue; }
            const bool announced = c.hello;
            const std::string who = c.name;
            CloseSocket(c.s);
            g_clients.erase(g_clients.begin() + static_cast<ptrdiff_t>(i));
            if (announced) { HostPublishPlayers(); AddChat("* " + who + " left"); HostBroadcast("CHAT *|" + who + " left"); }
        }
        if (g_server != INVALID_SOCKET && FD_ISSET(g_server, &rd) && !ReadLines(g_server, g_serverIn, JoinerOnLine))
            LeaveSession(g_mode == NetMode_Joined ? "The host closed the session." : "Couldn't join (the connection closed).");
    }
}

// ---- Lobby side ----------------------------------------------------------------------------------

static void Queue(int cmd, const std::string& arg = std::string()) {
    AcquireSRWLockExclusive(&g_lock);
    g_commands.emplace_back(cmd, arg);
    ReleaseSRWLockExclusive(&g_lock);
}

void Net_Start() {
    static volatile LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;
    if (HANDLE t = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr)) CloseHandle(t);
}

int Net_Mode() {
    AcquireSRWLockShared(&g_lock);
    const int m = g_mode;
    ReleaseSRWLockShared(&g_lock);
    return m;
}

void Net_GetName(char* out, size_t n) {
    AcquireSRWLockShared(&g_lock);
    strncpy_s(out, n, g_name.c_str(), _TRUNCATE);
    ReleaseSRWLockShared(&g_lock);
}

void Net_SetName(const char* name) {
    const std::string clean = CleanName(name ? name : "");
    AcquireSRWLockExclusive(&g_lock);
    g_name = clean;
    ReleaseSRWLockExclusive(&g_lock);
    SaveName(clean);
}

void Net_Host(int port) { Queue(Cmd_Host, std::to_string(port > 0 && port < 65536 ? port : kNetPort)); }
void Net_Join(const char* address) { Queue(Cmd_Join, address ? address : ""); }
void Net_Leave() { Queue(Cmd_Leave); }
void Net_Refresh() { Queue(Cmd_Refresh); }
void Net_SendChat(const char* text) { Queue(Cmd_Chat, text ? text : ""); }

int Net_SessionCount() {
    AcquireSRWLockShared(&g_lock);
    const int n = static_cast<int>(g_sessions.size());
    ReleaseSRWLockShared(&g_lock);
    return n;
}

bool Net_Session(int index, NetSession& out) {
    AcquireSRWLockShared(&g_lock);
    const bool ok = index >= 0 && index < static_cast<int>(g_sessions.size());
    if (ok) out = g_sessions[static_cast<size_t>(index)];
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

int Net_PlayerCount() {
    AcquireSRWLockShared(&g_lock);
    const int n = static_cast<int>(g_players.size());
    ReleaseSRWLockShared(&g_lock);
    return n;
}

bool Net_Player(int index, char* name, size_t n) {
    AcquireSRWLockShared(&g_lock);
    const bool ok = index >= 0 && index < static_cast<int>(g_players.size());
    if (ok) strncpy_s(name, n, g_players[static_cast<size_t>(index)].c_str(), _TRUNCATE);
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

int Net_ChatCount() {
    AcquireSRWLockShared(&g_lock);
    const int n = static_cast<int>(g_chat.size());
    ReleaseSRWLockShared(&g_lock);
    return n;
}

bool Net_ChatLine(int index, char* line, size_t n) {
    AcquireSRWLockShared(&g_lock);
    const bool ok = index >= 0 && index < static_cast<int>(g_chat.size());
    if (ok) strncpy_s(line, n, g_chat[static_cast<size_t>(index)].c_str(), _TRUNCATE);
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void Net_GetStatus(char* out, size_t n) {
    AcquireSRWLockShared(&g_lock);
    strncpy_s(out, n, g_status.c_str(), _TRUNCATE);
    ReleaseSRWLockShared(&g_lock);
}

// ---- Positions -----------------------------------------------------------------------------------

static std::string ZoneToken(const char* zone) {
    std::string out;
    for (const char* c = zone; *c && out.size() < 47; ++c) out += (*c == ',' || *c == '|' || *c == ' ') ? '_' : *c;
    return out.empty() ? "?" : out;
}

void Net_SetLocalState(const NetZonePos* zones, int count) {
    std::string position;
    if (count <= 0) position = "-";
    for (int i = 0; i < count && i < kNetMaxZones; ++i) {
        char xyz[96];
        snprintf(xyz, sizeof(xyz), ",%.3f,%.3f,%.3f", zones[i].pos[0], zones[i].pos[1], zones[i].pos[2]);
        position += (i ? "|" : "") + ZoneToken(zones[i].zone) + xyz;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_localPosition = position;
    ReleaseSRWLockExclusive(&g_lock);
}

static void DecodePosition(const std::string& position, NetPlayerState& out) {
    out.zones = 0;
    for (size_t start = 0; start < position.size() && out.zones < kNetMaxZones; ) {
        const size_t bar = position.find('|', start);
        const std::string part = position.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        NetZonePos& z = out.z[out.zones];
        const size_t comma = part.find(',');
        if (comma != std::string::npos && comma > 0 && comma < sizeof(z.zone) &&
            sscanf_s(part.c_str() + comma + 1, "%lf,%lf,%lf", &z.pos[0], &z.pos[1], &z.pos[2]) == 3) {
            memcpy(z.zone, part.data(), comma);
            z.zone[comma] = 0;
            ++out.zones;
        }
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
}

int Net_RemoteStates(NetPlayerState* out, int max) {
    AcquireSRWLockShared(&g_lock);
    int n = 0;
    const DWORD now = GetTickCount();
    for (const RemoteState& r : g_remote) {
        if (n >= max) break;
        NetPlayerState& st = out[n++];
        strncpy_s(st.name, r.name.c_str(), _TRUNCATE);
        st.ageMs = now - r.received;
        DecodePosition(r.position, st);
    }
    ReleaseSRWLockShared(&g_lock);
    return n;
}
