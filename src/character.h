#pragma once
#include "common.h"
#include <string>

// Your look (head, hair, DNA, skin), applied with the game's own console command
// ca_applyCustomHeadFile <CharacterCustomization xml>. Offline nothing saves it on your character, so it's
// put on again after every spawn, followed by your outfit carrying the look's head (see ProcessCharacter).
void ResolveCharacterApi(const Section& text, const Section& rdata);   // hooks the creator's save
void ProcessCharacter(DWORD now);   // main thread
std::string Character_HeadXml();    // main thread: the current look's Head_ItemPort loadout xml ("" = stock head)
void Character_NewSession();        // main thread: leaving the universe; the next body is a first spawn

// ---- For the lobby window (any thread) ----
struct CharacterLook {
    char id[128];      // "preset:<name>" or a file name (.chf / .xml) in data\characters
    char title[64];
    char detail[128];
};
int  Character_LookCount();
bool Character_Look(int index, CharacterLook& out);
void Character_GetChoice(char* id, size_t n, bool& autoApply);
void Character_Choose(const char* id);   // "" = the game's default look (nothing applied)
void Character_SetAuto(bool autoApply);
void Character_ApplyNow();
void Character_RescanLooks();            // pick up new files in data\characters
bool Character_InGame();
void Character_GetStatus(char* out, size_t n);
