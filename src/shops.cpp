#include "shops.h"
#include "hooks.h"
#include "teleport.h"
#include <share.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---- The Diffusion RPC ---------------------------------------------------------------------------
// CDiffusionGameClientBridgeAPI RPC(this, CryString* topic, CryString* payloadJson, callback*). When not connected
// it builds an RPC response (code 0x8005 "Not Connected To Diffusion") on the stack, calls the callback
// ([cb+0x38] -> vfunc +0x10 (callable, response*)) and returns 0. We build the same response with code 0 and
// our JSON in it. Response: +0x10 object (init 0042a790), +0x30 int code, +0x38 CryString error, +0x40 CryString data.
static const char* const kRpcPattern =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 70 49 8B D9 49 8B F8 48 8B F2 48 8B 01 FF 50 68 84 C0 0F 85";

// CShopManager::UpdateShopInventory(manager, shop EntityId, const char* shopSuperGuid): sends
// "sss-shop.inventory.get" {"ShopSuperGuid":"%s"} (that format string is referenced at +0x2B).
static const char* const kUpdateInventoryPrologue = "48 89 5C 24 10 55 56 57 41 56 41 57 48 8B EC";

using RpcFn = uint64_t(__fastcall*)(uintptr_t self, const char* const* topic, const char* const* payload, uintptr_t callback);
using UpdateInventoryFn = void(__fastcall*)(uintptr_t manager, uint64_t shopId, const char* superGuid);
using VoidFn = void(__fastcall*)(void*);
using AssignFn = void(__fastcall*)(void*, const char*);

static RpcFn             g_origRpc = nullptr;
static UpdateInventoryFn g_origUpdateInventory = nullptr;
static VoidFn            g_responseInit = nullptr, g_strCtor = nullptr, g_strDtor = nullptr;
static AssignFn          g_strAssign = nullptr;
static uint64_t          g_askingShop = 0;   // set while UpdateShopInventory runs (it calls the RPC synchronously)

// ---- Catalog (data\shop_catalog.txt) -------------------------------------------------------------

struct CatalogEntry { int kind; std::string cls, guid; double price; };
enum { Kind_Personal, Kind_Ship, Kind_Vehicle };
static std::vector<CatalogEntry> g_catalog;
static bool g_catalogLoaded = false;
static std::string g_itemsJson, g_vehiclesJson, g_pricesJson;

static std::string GuidString(uint64_t first, uint64_t second) {
    char s[40];
    snprintf(s, sizeof(s), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(first >> 32), static_cast<unsigned>((first >> 16) & 0xFFFF),
             static_cast<unsigned>(first & 0xFFFF), static_cast<unsigned>(second >> 48), static_cast<unsigned long long>(second & 0xFFFFFFFFFFFFull));
    return s;
}

// Class pointer -> GUID, from the class registry's GUID map (registry+0x48; node key +0x20/+0x28, class +0x30),
// the same map hooks.cpp reads for the fleet's ships.
static void ReadClassGuids(uintptr_t registry, std::unordered_map<uintptr_t, std::pair<uint64_t, uint64_t>>& out) {
    const uintptr_t head = Rd<uintptr_t>(registry + 0x48);
    std::vector<uintptr_t> stack;
    const uintptr_t root = Rd<uintptr_t>(head + 0x08);
    if (root && !Rd<uint8_t>(root + 0x19)) stack.push_back(root);
    while (!stack.empty() && out.size() < 500000) {
        const uintptr_t n = stack.back();
        stack.pop_back();
        out[Rd<uintptr_t>(n + 0x30)] = { Rd<uint64_t>(n + 0x20), Rd<uint64_t>(n + 0x28) };
        for (size_t off : { size_t(0x00), size_t(0x10) }) {
            const uintptr_t c = Rd<uintptr_t>(n + off);
            if (c && !Rd<uint8_t>(c + 0x19)) stack.push_back(c);
        }
    }
}

static std::string JsonNumber(double v) {
    char s[32];
    snprintf(s, sizeof(s), "%.2f", v);
    return s;
}

static void BuildJson() {
    g_itemsJson.clear();
    g_vehiclesJson.clear();
    g_pricesJson.clear();
    for (const CatalogEntry& e : g_catalog) {
        const std::string id = "{\"ID\":{\"ID\":[\"" + e.guid + "\"]},\"BuyPrice\":" + JsonNumber(e.price) +
                               ",\"SellPrice\":0,\"CurrentInventory\":100,\"MaxInventory\":100";
        if (e.kind == Kind_Vehicle) {
            std::string rentals;
            for (int days : { 1, 3, 7, 30 })
                rentals += (rentals.empty() ? "" : ",") + std::string("{\"Price\":") + JsonNumber(e.price * 0.004 * days) + ",\"Days\":" + std::to_string(days) + "}";
            g_vehiclesJson += (g_vehiclesJson.empty() ? "" : ",") + id + ",\"RentalOfferings\":[" + rentals + "]}";
        } else {
            g_itemsJson += (g_itemsJson.empty() ? "" : ",") + id + "}";
        }
        g_pricesJson += (g_pricesJson.empty() ? "" : ",") + std::string("{\"itemId\":\"") + e.guid + "\",\"basePrice\":" + JsonNumber(e.price) + "}";
    }
}

// Main thread (the shop RPCs come from the game's update).
static void LoadCatalog() {
    if (g_catalogLoaded) return;
    g_catalogLoaded = true;
    char path[MAX_PATH];
    FILE* f = DataFilePath("shop_catalog.txt", path, sizeof(path)) ? _fsopen(path, "r", _SH_DENYNO) : nullptr;
    if (!f) { Log("[shop] no data\\shop_catalog.txt; shops stay empty (make it with tools\\re\\gen_shop_catalog.py)"); return; }
    struct Line { int kind; std::string cls; double price; };
    std::vector<Line> lines;
    std::string checkClass, checkGuid;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char* a = strchr(line, '\t');
        char* b = a ? strchr(a + 1, '\t') : nullptr;
        if (!b) continue;
        *a = *b = 0;
        if (strcmp(line, "check") == 0) { checkClass = a + 1; checkGuid = b + 1; continue; }
        const int kind = strcmp(line, "vehicle") == 0 ? Kind_Vehicle : strcmp(line, "ship") == 0 ? Kind_Ship : Kind_Personal;
        lines.push_back({ kind, a + 1, atof(b + 1) });
    }
    fclose(f);

    std::unordered_map<uintptr_t, std::pair<uint64_t, uint64_t>> guids;
    const uintptr_t registry = *g_tp.entitySystem ? VCall<uintptr_t>(*g_tp.entitySystem, 0xC0) : 0;   // faults: SafeLoadCatalog
    if (!registry) { Log("[shop] no class registry; shops stay empty"); return; }
    ReadClassGuids(registry, guids);

    bool swapped = false;   // GUID halves: the game's text form starts with the first stored half (DataCore order)
    int unknown = 0;
    for (const Line& l : lines) {
        const uintptr_t cls = VCall<uintptr_t>(registry, 0x20, l.cls.c_str());
        const auto it = cls ? guids.find(cls) : guids.end();
        if (it == guids.end()) { ++unknown; continue; }
        if (!checkClass.empty() && l.cls == checkClass) {
            const std::string straight = GuidString(it->second.first, it->second.second);
            swapped = straight != checkGuid && GuidString(it->second.second, it->second.first) == checkGuid;
            Log("[shop] GUID check %s: %s (%s)", checkClass.c_str(), straight.c_str(),
                straight == checkGuid ? "matches the game data" : swapped ? "halves swapped - using the other order" : "DOESN'T match the game data");
        }
        g_catalog.push_back({ l.kind, l.cls, std::string(), l.price });
    }
    // Second pass now that the GUID order is known.
    size_t i = 0;
    for (const Line& l : lines) {
        const uintptr_t cls = VCall<uintptr_t>(registry, 0x20, l.cls.c_str());
        const auto it = cls ? guids.find(cls) : guids.end();
        if (it == guids.end()) continue;
        g_catalog[i++].guid = swapped ? GuidString(it->second.second, it->second.first) : GuidString(it->second.first, it->second.second);
    }
    BuildJson();
    Log("[shop] catalog: %zu items for sale (%d names unknown to this build)", g_catalog.size(), unknown);
}

static void SafeLoadCatalog() {
    __try { LoadCatalog(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[shop] fault while reading the catalog"); }
}

// ---- Answers -------------------------------------------------------------------------------------

static const char* ShopName(uint64_t id) {
    __try {
        const uintptr_t es = *g_tp.entitySystem;
        const uintptr_t e = es && id ? VCall<uintptr_t>(es, 0x120, id) : 0;
        return e ? VCall<const char*>(e, 0x78) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Commodity and cargo kiosks list commodities (resource ids), not items; they get an empty list for now.
static bool IsCommodityShop(const char* name) {
    if (!name) return false;
    std::string n = name;
    for (char& c : n) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (const char* k : { "commodit", "cargo", "refiner", "tdd", "trade" })
        if (n.find(k) != std::string::npos) return true;
    return false;
}

// Pulls "ShopSuperGuid":"<value>" out of the request payload.
static std::string SuperGuidFrom(const char* payload) {
    const char* key = payload ? strstr(payload, "\"ShopSuperGuid\":\"") : nullptr;
    if (!key) return std::string();
    const char* v = key + 17;
    const char* end = strchr(v, '"');
    return end ? std::string(v, end) : std::string();
}

static bool AnswerTopic(const char* topic, const char* payload, std::string& json) {
    if (strcmp(topic, "sss-shop.inventory.get") == 0) {
        SafeLoadCatalog();
        const std::string super = SuperGuidFrom(payload);
        const char* name = ShopName(g_askingShop);
        const bool commodity = IsCommodityShop(name);
        json = "{\"ShopID\":\"" + super + "\",\"AcceptsStolenGoods\":false,\"CargoAutoLoadPriceModifierPct\":0,"
               "\"CargoAutoLoadTimeModifierPct\":0,\"Collection\":{\"Inventory\":[" + (commodity ? std::string() : g_itemsJson) + "]}}";
        static std::unordered_set<std::string> logged;
        if (logged.insert(super).second)
            Log("[shop] %s (%s): %s", name ? name : "unknown shop", super.c_str(), commodity ? "commodity kiosk - empty for now" : "item list sent");
        return true;
    }
    if (strcmp(topic, "sss-shop.inventory.global.get") == 0) {
        SafeLoadCatalog();
        json = "{\"Inventory\":[" + g_vehiclesJson + "]}";
        static bool logged = false;
        if (!logged) { logged = true; Log("[shop] ship dealers and rentals: vehicle list sent"); }
        return true;
    }
    if (strcmp(topic, "sss-shop.GetGlobalCommoditiesData") == 0) {
        SafeLoadCatalog();
        json = "{\"commodities\":[" + g_pricesJson + "]}";
        return true;
    }
    return false;
}

// Builds the RPC response the way the original does and hands it to the callback. Plain types only (SEH).
static bool Respond(uintptr_t callback, const char* json) {
    alignas(16) uint8_t response[0x60] = {};
    __try {
        g_responseInit(response + 0x10);
        g_strCtor(response + 0x38);
        g_strCtor(response + 0x40);
        *reinterpret_cast<int32_t*>(response + 0x30) = 0;
        g_strAssign(response + 0x40, json);
        const uintptr_t callable = Rd<uintptr_t>(callback + 0x38);
        if (callable) VCall<void>(callable, 0x10, response);
        g_strDtor(response + 0x40);
        g_strDtor(response + 0x38);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool Connected(uintptr_t self) {
    __try { return VCall<bool>(self, 0x68); } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

static uint64_t __fastcall RpcHook(uintptr_t self, const char* const* topic, const char* const* payload, uintptr_t callback) {
    const char* t = topic ? *topic : nullptr;
    if (t && strncmp(t, "sss-shop.", 9) == 0 && !Connected(self)) {
        std::string json;
        if (AnswerTopic(t, payload ? *payload : nullptr, json)) {
            if (!Respond(callback, json.c_str())) Log("[shop] fault while answering %s", t);
            return 0;   // like the original: the caller stores this as its pending-request handle
        }
    }
    return g_origRpc(self, topic, payload, callback);
}

static void __fastcall UpdateInventoryHook(uintptr_t manager, uint64_t shopId, const char* superGuid) {
    g_askingShop = shopId;
    g_origUpdateInventory(manager, shopId, superGuid);
    g_askingShop = 0;
}

// ---- Resolve -------------------------------------------------------------------------------------

static uint8_t* CallTarget(uint8_t* call) { return call[0] == 0xE8 ? call + 5 + Rel32(call + 1) : nullptr; }

void ResolveShopsApi(const Section& text, const Section& rdata) {
    int matches = 0;
    uint8_t* rpc = FindUniquePattern(text, kRpcPattern, matches);
    if (!rpc || !BytesMatch(rpc + 0x5B, "C7 44 24 50 05 80 00 00")) { Log("[!] shops: Diffusion RPC not found (%d matches); shops stay empty", matches); return; }
    g_responseInit = reinterpret_cast<VoidFn>(CallTarget(rpc + 0x32));
    g_strCtor      = reinterpret_cast<VoidFn>(CallTarget(rpc + 0x4A));
    g_strAssign    = reinterpret_cast<AssignFn>(CallTarget(rpc + 0x6F));
    g_strDtor      = reinterpret_cast<VoidFn>(CallTarget(rpc + 0x8E));
    if (!g_responseInit || !g_strCtor || !g_strAssign || !g_strDtor) { Log("[!] shops: RPC response helpers not found"); return; }

    const uint8_t* fmt = FindCString(rdata, "{\"ShopSuperGuid\":\"%s\"}");
    uint8_t* lea = fmt ? FindRipLea(text, 0x48, 0x8D, 0x15, fmt) : nullptr;
    uint8_t* update = lea ? lea - 0x2B : nullptr;
    if (update && BytesMatch(update, kUpdateInventoryPrologue))
        HookFunction(update, 12, reinterpret_cast<void*>(&UpdateInventoryHook), reinterpret_cast<void**>(&g_origUpdateInventory));
    else
        Log("[!] shops: UpdateShopInventory not found; shop names won't be known (commodity kiosks get items)");

    if (!HookFunction(rpc, 10, reinterpret_cast<void*>(&RpcHook), reinterpret_cast<void**>(&g_origRpc))) {
        Log("[!] shops: couldn't hook the Diffusion RPC");
        return;
    }
    Log("[+] shops: answering shop listings offline from data\\shop_catalog.txt");
}
