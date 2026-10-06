#pragma once
#include <windows.h>

// The lobby: a window of its own, separate from the M menu, for everything before you play - where you
// spawn now, the character creator next, and later hosting/joining a local multiplayer session.
// It opens itself when the game waits for a spawn pick; F9 opens it in game.
void Lobby_Start(HWND gameWindow);
bool Lobby_IsRunning();
