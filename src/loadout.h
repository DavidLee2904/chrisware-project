#pragma once
#include "common.h"

bool ResolveLoadoutApi(const Section& text, const Section& rdata);
void ProcessLoadout();
bool Loadout_EquipOutfit();   // main thread: your outfit (last equipped gear); false if no loadout loader
