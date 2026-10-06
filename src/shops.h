#pragma once
#include "common.h"

// Offline shops, stage 1: listings. Shop kiosks, ship dealers and rental screens get their items and prices from
// CIG's shop service over Diffusion ("sss-shop.*" RPCs), which offline answers "not connected", so they load
// forever. We answer those RPCs from data\shop_catalog.txt (tools\re\gen_shop_catalog.py).
void ResolveShopsApi(const Section& text, const Section& rdata);
