// testbot: a stand-in second player for testing multiplayer on one PC.
// It joins a ChrisWareOffline session and walks in a circle around the first player it hears from, so the
// game should show it as a stand-in next to you.
//   testbot.exe [address] [name] [radius]      defaults: 127.0.0.1  TestBot  2.5
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "net.h"

// net.cpp's dependencies from the mod.
void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}
bool DataFilePath(const char*, char*, size_t) { return false; }

static void Cross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static bool Normalize(double v[3]) {
    const double len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-9) return false;
    for (int i = 0; i < 3; ++i) v[i] /= len;
    return true;
}

// Offset along a circle. Far from the zone's origin (on a planet), the circle lies flat on the ground:
// perpendicular to the direction away from the planet's centre. Close to it (rooms, stations), it's the x/y plane.
static void CircleOffset(const double pos[3], double radius, double angle, double out[3]) {
    double up[3] = { pos[0], pos[1], pos[2] };
    double t1[3] = { 1, 0, 0 }, t2[3] = { 0, 1, 0 };
    if (sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]) > 100000.0 && Normalize(up)) {
        const double z[3] = { 0, 0, 1 }, x[3] = { 1, 0, 0 };
        Cross(up, fabs(up[2]) < 0.9 ? z : x, t1);
        Normalize(t1);
        Cross(up, t1, t2);
    }
    for (int i = 0; i < 3; ++i) out[i] = pos[i] + radius * (cos(angle) * t1[i] + sin(angle) * t2[i]);
}

int main(int argc, char** argv) {
    const char* address = argc > 1 ? argv[1] : "127.0.0.1";
    const char* name = argc > 2 ? argv[2] : "TestBot";
    const double radius = argc > 3 ? atof(argv[3]) : 2.5;

    Net_Start();
    Sleep(300);
    Net_SetName(name);
    Net_Join(address);
    printf("testbot: joining %s as %s (Ctrl+C to quit)\n", address, name);
    for (int i = 0; i < 50 && Net_Mode() == NetMode_Idle; ++i) Sleep(100);   // the join runs on the network thread

    double angle = 0;
    char followed[32] = "";
    bool greeted = false;
    for (DWORD lastPrint = 0;; Sleep(100)) {
        const int mode = Net_Mode();
        if (mode == NetMode_Idle) {
            char status[256];
            Net_GetStatus(status, sizeof(status));
            printf("testbot: not in a session: %s\n", status);
            return 1;
        }
        if (mode != NetMode_Joined) continue;
        if (!greeted) { greeted = true; Net_SendChat("TestBot here - I'll walk circles around the first player I see."); }

        NetPlayerState others[16];
        const int n = Net_RemoteStates(others, 16);
        const NetPlayerState* target = nullptr;
        for (int i = 0; i < n && !target; ++i)
            if (others[i].zones > 0 && others[i].ageMs < 3000) target = &others[i];
        if (!target) {
            Net_SetLocalState(nullptr, 0);
            if (GetTickCount() - lastPrint > 3000) { lastPrint = GetTickCount(); printf("testbot: waiting for someone in the universe\n"); }
            continue;
        }
        NetZonePos me[kNetMaxZones];
        for (int i = 0; i < target->zones; ++i) {
            me[i] = target->z[i];
            CircleOffset(target->z[i].pos, radius, angle, me[i].pos);
        }
        Net_SetLocalState(me, target->zones);
        angle += 0.05;   // a lap every ~12 s
        if (strcmp(followed, target->name) != 0 || GetTickCount() - lastPrint > 5000) {
            strcpy_s(followed, target->name);
            lastPrint = GetTickCount();
            printf("testbot: circling %s in %s at (%.1f, %.1f, %.1f)\n", target->name, target->z[0].zone,
                   me[0].pos[0], me[0].pos[1], me[0].pos[2]);
        }
    }
}
