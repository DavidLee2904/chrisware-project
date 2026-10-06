#pragma once
#include <windows.h>

// Local multiplayer groundwork: a LAN lobby between ChrisWareOffline players - host a session, find sessions on
// the network (UDP broadcast), join by address, see who's in, chat. TCP, one line per message.
// It doesn't put anyone into the other player's game world yet; this is the session layer that will carry that.
// All functions are thread-safe; the network runs on its own thread (Net_Start).

constexpr int kNetPort = 27015;            // TCP, the session
constexpr int kNetDiscoveryPort = 27016;   // UDP, LAN discovery

enum NetMode { NetMode_Idle, NetMode_Hosting, NetMode_Connecting, NetMode_Joined };

struct NetSession { char host[32]; char address[64]; int players; };

void Net_Start();
int  Net_Mode();
void Net_GetName(char* out, size_t n);
void Net_SetName(const char* name);             // saved in data\multiplayer.txt
void Net_Host(int port);
void Net_Join(const char* address);             // "ip" or "ip:port"
void Net_Leave();
void Net_Refresh();                             // look for sessions on the local network
int  Net_SessionCount();
bool Net_Session(int index, NetSession& out);
int  Net_PlayerCount();
bool Net_Player(int index, char* name, size_t n);
int  Net_ChatCount();
bool Net_ChatLine(int index, char* line, size_t n);
void Net_SendChat(const char* text);
void Net_GetStatus(char* out, size_t n);

// ---- Where everyone is (presence.cpp) ----
// A position is the player's zone chain, innermost first, with their position inside each zone. Another
// player is shown in your world through the innermost zone you both have (zones are matched by name).
constexpr int kNetMaxZones = 6;
struct NetZonePos { char zone[48]; double pos[3]; };
struct NetPlayerState { char name[32]; int zones; NetZonePos z[kNetMaxZones]; DWORD ageMs; };
void Net_SetLocalState(const NetZonePos* zones, int count);   // game thread; count 0 = not in the universe
int  Net_RemoteStates(NetPlayerState* out, int max);           // other players' latest positions
