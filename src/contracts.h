#pragma once
#include "common.h"

void ResolveContractsApi(const Section& text, const Section& rdata);
void ProcessContracts();

int64_t Wallet_Balance();          // your aUEC, or -1 if you have no wallet yet
bool    Wallet_Pay(int64_t uec);   // takes uec from your wallet (and saves wallet.txt); false if the wallet refused
