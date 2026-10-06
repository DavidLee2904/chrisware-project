#include "shops.h"
#include "contracts.h"
#include "hooks.h"
#include "spawner.h"
#include "teleport.h"
#include <share.h>
#include <mutex>
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

// CShopManager::RegisterShop(manager, IEntityPtr shop, const char* superGuid): adds crc32(superGuid) -> shop EntityId
// to the map the inventory reply is routed through (only if that key is new), then calls UpdateShopInventory.
// Offline every shop's super GUID is "", so only the first shop got in and every reply went to it. Its first
// "CShopManager::RegisterShop" lea r8 is at +0x73.
static const char* const kRegisterShopPrologue = "48 89 5C 24 08 48 89 74 24 18 48 89 54 24 10 55 57 41 54 41 56 41 57";

// CShopInventory::LoadInventoryFromJSON(inventory, shop, jansson array, ...) reads each entry's "ID" as a GUID
// string with the game's CryGUID-from-string (called at +0x33B); its first "CShopInventory::LoadInventoryFromJSON"
// lea r8 is at +0x8A. We use that parser to check our GUID text.
static const char* const kLoadInventoryPrologue = "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 89 48 08";

using RpcFn = uint64_t(__fastcall*)(uintptr_t self, const char* const* topic, const char* const* payload, uintptr_t callback);
using UpdateInventoryFn = void(__fastcall*)(uintptr_t manager, uint64_t shopId, const char* superGuid);
using VoidFn = void(__fastcall*)(void*);
using AssignFn = void(__fastcall*)(void*, const char*);
using GuidParseFn = const uint64_t*(__fastcall*)(uint64_t* out, const char* text);
using RegisterShopFn = void(__fastcall*)(uintptr_t manager, uintptr_t shopEntityPtr, const char* superGuid);

static RpcFn             g_origRpc = nullptr;
static UpdateInventoryFn g_origUpdateInventory = nullptr;
static RegisterShopFn    g_origRegisterShop = nullptr;
static VoidFn            g_responseInit = nullptr, g_strCtor = nullptr, g_strDtor = nullptr;
static AssignFn          g_strAssign = nullptr;
static GuidParseFn       g_guidParse = nullptr;
static uint64_t          g_askingShop = 0;   // set while UpdateShopInventory runs (it calls the RPC synchronously)

// ---- Catalog (data\shop_catalog.txt) -------------------------------------------------------------

struct CatalogEntry { int kind; std::string cls, guid; double price; };
enum { Kind_Personal, Kind_Ship, Kind_Vehicle };
static std::vector<CatalogEntry> g_catalog;
static std::unordered_map<std::string, double> g_priceOf;   // class name (lower case) -> price
static bool g_catalogLoaded = false;
static std::string g_itemsJson, g_vehiclesJson, g_pricesJson;

static void FormatGuid(char (&s)[40], uint64_t first, uint64_t second) {
    snprintf(s, sizeof(s), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(first >> 32), static_cast<unsigned>((first >> 16) & 0xFFFF),
             static_cast<unsigned>(first & 0xFFFF), static_cast<unsigned>(second >> 48), static_cast<unsigned long long>(second & 0xFFFFFFFFFFFFull));
}

static std::string GuidString(uint64_t first, uint64_t second) {
    char s[40];
    FormatGuid(s, first, second);
    return s;
}

// Runs our text form through the game's own GUID parser: true if it gives back these exact halves.
static bool GameParsesBack(uint64_t first, uint64_t second, uint64_t textFirst, uint64_t textSecond) {
    char s[40];
    FormatGuid(s, textFirst, textSecond);
    __try {
        uint64_t out[2] = {};
        const uint64_t* g = g_guidParse(out, s);
        return g && g[0] == first && g[1] == second;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

static std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
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
        const std::string id = "{\"ID\":\"" + e.guid + "\",\"BuyPrice\":" + JsonNumber(e.price) +
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
    // The game's own parser has the final say on the text order (it's what reads the shop JSON).
    if (g_guidParse && !g_catalog.empty()) {
        const uintptr_t cls = VCall<uintptr_t>(registry, 0x20, g_catalog.front().cls.c_str());
        const auto it = guids.find(cls);
        const uint64_t a = it->second.first, b = it->second.second;
        if (GameParsesBack(a, b, a, b)) swapped = false;
        else if (GameParsesBack(a, b, b, a)) swapped = true;
        Log("[shop] game's GUID parser: %s reads back %s", g_catalog.front().cls.c_str(),
            GameParsesBack(a, b, swapped ? b : a, swapped ? a : b) ? (swapped ? "with halves swapped (using that)" : "exactly") : "WRONG either way - shops will stay empty");
    }
    // Second pass now that the GUID order is known.
    size_t i = 0;
    for (const Line& l : lines) {
        const uintptr_t cls = VCall<uintptr_t>(registry, 0x20, l.cls.c_str());
        const auto it = cls ? guids.find(cls) : guids.end();
        if (it == guids.end()) continue;
        g_catalog[i++].guid = swapped ? GuidString(it->second.second, it->second.first) : GuidString(it->second.first, it->second.second);
    }
    for (const CatalogEntry& e : g_catalog) g_priceOf[Lower(e.cls)] = e.price;
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
    const std::string n = Lower(name);
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
        static bool payloadLogged = false;
        if (!payloadLogged) { payloadLogged = true; Log("[shop] first shop request payload: %.160s", payload ? payload : "(none)"); }
        static std::unordered_set<uint64_t> logged;
        if (logged.insert(g_askingShop).second)
            Log("[shop] %s (super guid '%s'): %s", name ? name : "unknown shop", super.c_str(), commodity ? "commodity kiosk - empty for now" : "item list sent");
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

// Stand-in super GUIDs for shops that have none offline, one per shop EntityId (stable: shops have persistent ids,
// so a shop that streams out and back in gets the same one). Node-based map: c_str() stays valid.
static std::mutex                             g_superMutex;
static std::unordered_map<uint64_t, std::string> g_standInSuper;

static const char* SuperGuidFor(uint64_t shopId, const char* superGuid) {
    if ((superGuid && *superGuid) || !shopId) return superGuid;
    std::lock_guard<std::mutex> lock(g_superMutex);
    auto it = g_standInSuper.find(shopId);
    if (it == g_standInSuper.end()) {
        char s[40];
        snprintf(s, sizeof(s), "%08x-0000-4000-8000-%012llx", static_cast<unsigned>(shopId >> 48), static_cast<unsigned long long>(shopId & 0xFFFFFFFFFFFFull));
        it = g_standInSuper.emplace(shopId, s).first;
    }
    return it->second.c_str();
}

// IEntityPtr is a tagged pointer (low 48 bits); entity vfunc +0x08 writes/returns a pointer to its EntityId.
static uint64_t EntityIdOf(uintptr_t entityPtr) {
    __try {
        const uintptr_t e = entityPtr & 0xFFFFFFFFFFFFull;
        if (!e) return 0;
        uint64_t tmp = 0;
        const uint64_t* id = VCall<const uint64_t*>(e, 0x8, &tmp);
        return id ? *id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void __fastcall RegisterShopHook(uintptr_t manager, uintptr_t shopEntityPtr, const char* superGuid) {
    const char* use = SuperGuidFor(EntityIdOf(shopEntityPtr), superGuid);
    static bool logged = false;
    if (!logged && use != superGuid) { logged = true; Log("[shop] shops have no super GUID offline; each gets its own (first: %s)", use); }
    g_origRegisterShop(manager, shopEntityPtr, use);
}

static void __fastcall UpdateInventoryHook(uintptr_t manager, uint64_t shopId, const char* superGuid) {
    superGuid = SuperGuidFor(shopId, superGuid);
    g_askingShop = shopId;
    g_origUpdateInventory(manager, shopId, superGuid);
    g_askingShop = 0;
}

// ---- Buying (stage 2) ----------------------------------------------------------------------------
// CEntityComponentShop::ProcessStandardItemBuyRequest(shop component, const SShopBuyRequest*, const callback*): racks
// (CEntityComponentShoppingProvider::RmStandardItemBuyShopRequest) and terminals (ShopUIProvider::RmAuthorityProcessBuy)
// both land here. Offline it asks the transaction service and fails with TransactionServiceError. We take the money
// ourselves and drop the item at the player's feet.
// SShopBuyRequest: +0x00 player EntityId, +0x08 shop EntityId, +0x10 kiosk id, +0x18 client price (double),
// +0x20 item class GUID (16 bytes), +0x48 quantity. The callback is a small functor: +0x00 invoke(functor, EShopFlowResult).
static const char* const kBuyPrologue = "4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57";
enum : uint8_t { Flow_Success = 0, Flow_InternalError = 14, Flow_InvalidItem = 29, Flow_InsufficentFunds = 31 };

using BuyFn = void(__fastcall*)(uintptr_t shop, uintptr_t request, uintptr_t callback);
using FlowCallbackFn = void(__fastcall*)(uintptr_t callback, uint8_t result);
static BuyFn g_origBuy = nullptr;

struct Delivery { std::string cls; int count; };
static std::mutex            g_deliveryMutex;
static std::vector<Delivery> g_deliveries;

static bool ReportFlow(uintptr_t callback, uint8_t result) {
    __try {
        const uintptr_t invoke = callback ? Rd<uintptr_t>(callback) : 0;
        if (!invoke) return false;
        reinterpret_cast<FlowCallbackFn>(invoke)(callback, result);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The item class the request names, found the way the game does it: class registry vfunc +0x18 (by GUID), name +0x18.
static const char* RequestedClass(uintptr_t request) {
    __try {
        const uintptr_t registry = *g_tp.entitySystem ? VCall<uintptr_t>(*g_tp.entitySystem, 0xC0) : 0;
        uint64_t guid[2] = { Rd<uint64_t>(request + 0x20), Rd<uint64_t>(request + 0x28) };
        const uintptr_t cls = registry ? VCall<uintptr_t>(registry, 0x18, guid) : 0;
        return cls ? VCall<const char*>(cls, 0x18) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool ReadRequest(uintptr_t request, uint64_t& player, uint32_t& quantity, double& clientPrice) {
    __try {
        player = Rd<uint64_t>(request);
        quantity = Rd<uint32_t>(request + 0x48);
        clientPrice = Rd<double>(request + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool HandleBuy(uintptr_t request, uintptr_t callback) {
    uint64_t player = 0;
    uint32_t quantity = 0;
    double clientPrice = 0;
    if (!ReadRequest(request, player, quantity, clientPrice)) return false;
    const uint64_t me = LocalPlayerEntityId();
    if (player != me) {   // someone else's request: leave it to the game
        Log("[shop] buy request for player %llu, not you (%llu) - left to the game", static_cast<unsigned long long>(player), static_cast<unsigned long long>(me));
        return false;
    }
    const char* name = RequestedClass(request);
    if (!name) { Log("[shop] buy: the item's class isn't known to this build"); return ReportFlow(callback, Flow_InvalidItem); }
    const std::string cls = name;
    SafeLoadCatalog();
    const auto it = g_priceOf.find(Lower(cls));
    if (quantity < 1) quantity = 1;
    if (quantity > 50) quantity = 50;
    const double unit = it != g_priceOf.end() ? it->second : clientPrice;
    const int64_t total = static_cast<int64_t>(unit * quantity + 0.5);
    const int64_t balance = Wallet_Balance();
    if (balance < 0) { Log("[shop] buy %s: no wallet", cls.c_str()); return ReportFlow(callback, Flow_InternalError); }
    if (balance < total) {
        Log("[shop] buy %s x%u: %lld aUEC needed, you have %lld", cls.c_str(), quantity, static_cast<long long>(total), static_cast<long long>(balance));
        return ReportFlow(callback, Flow_InsufficentFunds);
    }
    if (total > 0 && !Wallet_Pay(total)) { Log("[shop] buy %s: the wallet refused the payment", cls.c_str()); return ReportFlow(callback, Flow_InternalError); }
    {
        std::lock_guard<std::mutex> lock(g_deliveryMutex);
        g_deliveries.push_back({ cls, static_cast<int>(quantity) });
    }
    Log("[shop] bought %s x%u for %lld aUEC (shop price %.0f each, screen said %.0f; wallet now %lld)", cls.c_str(), quantity,
        static_cast<long long>(total), unit, clientPrice, static_cast<long long>(Wallet_Balance()));
    if (!ReportFlow(callback, Flow_Success)) Log("[shop] buy: couldn't tell the shop screen it worked");
    return true;
}

static void __fastcall BuyHook(uintptr_t shop, uintptr_t request, uintptr_t callback) {
    if (!HandleBuy(request, callback)) g_origBuy(shop, request, callback);
}

// Main thread: bought items appear at your feet.
void ProcessShops() {
    std::vector<Delivery> todo;
    {
        std::lock_guard<std::mutex> lock(g_deliveryMutex);
        todo.swap(g_deliveries);
    }
    for (const Delivery& d : todo) {
        for (int i = 0; i < d.count; ++i) {
            const double offset[3] = { 0.25 * (i % 4), 0.25 * (i / 4), 0.6 };
            uint64_t id = 0;
            if (const char* err = SpawnEntityNearPlayer(d.cls.c_str(), offset, id)) {
                Log("[shop] couldn't hand over %s: %s", d.cls.c_str(), err);
                break;
            }
            if (i == 0) Log("[shop] %s is at your feet (entity %llu)", d.cls.c_str(), static_cast<unsigned long long>(id));
        }
    }
}

static uint8_t* CallTarget(uint8_t* call) { return call[0] == 0xE8 ? call + 5 + Rel32(call + 1) : nullptr; }

// ---- A display of our own: the Crusader Morozov-SH on Cubby Blast's heavy-armor mannequins (Area18) ----------
// Mannequins are racks: CEntityComponentShop::PopulateStandardRackItems(shop, ctx, empty rack ports) fills each empty
// port with an inventory item that passes the item-port check sub_00508f50(port def, attach def, 0) (0 = fits; it calls
// at +0x53 the RequiredPortTags test sub_00530b30, whose port def +0x68 is the tags string). The heavy marine
// mannequins want "Marine_Heavy Set_01 Color_01"; while Cubby Blast fills its racks those ports take only the Morozov
// pieces, and for them the tags test is skipped (size and type checks still run). Display items get the shop's buy
// interaction, so buying them goes through BuyHook.
static const char* const kRackFillPrologue = "48 8B C4 4C 89 40 18 48 89 50 10 48 89 48 08 55";
static const char* const kPortCheckPrologue = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20";
static const char* const kPortTagsPrologue = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57";
static const char* const kDisplayShop = "SCShop_Entity_CubbyBlast_Area18";
static const char* const kDisplayPortTags = "Marine_Heavy Set_01 Color_01";
static const char* const kDisplayItems[] = {
    "rrs_specialist_heavy_core_01_crus01_01", "rrs_specialist_light_arms_01_crus01_01",
    "rrs_specialist_heavy_legs_01_crus01_01", "rrs_specialist_heavy_helmet_03_crus01_01" };

using RackFillFn = void(__fastcall*)(uintptr_t shop, uintptr_t ctx, uintptr_t ports);
using PortCheckFn = int(__fastcall*)(uintptr_t portDef, uintptr_t attachDef, uint8_t flag);
using PortTagsFn = bool(__fastcall*)(uintptr_t portDef, uintptr_t attachDef, uint8_t flag);
using AttachDefFn = uintptr_t(__fastcall*)(uintptr_t registry, uintptr_t cls);
static RackFillFn  g_origRackFill = nullptr;
static PortCheckFn g_origPortCheck = nullptr;
static PortTagsFn  g_origPortTags = nullptr;
static AttachDefFn g_attachDefOf = nullptr;
static uintptr_t*  g_attachRegistry = nullptr;
static uintptr_t   g_displayDefs[4] = {};
static volatile DWORD g_displayThread = 0;   // thread filling Cubby Blast's racks, 0 if none
static volatile DWORD g_skipTagsThread = 0;  // thread whose next tags test passes
static int         g_displayPlaced = 0;

static bool ResolveDisplayDefs() {
    if (g_displayDefs[0]) return true;
    __try {
        const uintptr_t registry = *g_tp.entitySystem ? VCall<uintptr_t>(*g_tp.entitySystem, 0xC0) : 0;
        for (int i = 0; i < 4 && registry; ++i) {
            const uintptr_t cls = VCall<uintptr_t>(registry, 0x20, kDisplayItems[i]);
            g_displayDefs[i] = cls ? g_attachDefOf(*g_attachRegistry, cls) : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(g_displayDefs, 0, sizeof(g_displayDefs));
    }
    for (uintptr_t d : g_displayDefs) if (!d) { memset(g_displayDefs, 0, sizeof(g_displayDefs)); return false; }
    return true;
}

static bool IsDisplayShop(uintptr_t shop) {
    __try {
        const uintptr_t entity = Rd<uintptr_t>(shop + 8) & 0xFFFFFFFFFFFFull;
        const char* name = entity ? VCall<const char*>(entity, 0x78) : nullptr;
        return name && strcmp(name, kDisplayShop) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool IsDisplayPort(uintptr_t portDef) {
    __try {
        const char* tags = Rd<const char*>(portDef + 0x68);
        return tags && strcmp(tags, kDisplayPortTags) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int __fastcall PortCheckHook(uintptr_t portDef, uintptr_t attachDef, uint8_t flag) {
    if (g_displayThread != GetCurrentThreadId() || !IsDisplayPort(portDef)) return g_origPortCheck(portDef, attachDef, flag);
    bool ours = false;
    for (uintptr_t d : g_displayDefs) ours |= d == attachDef;
    if (!ours) return 4;   // "tags don't match": only the Morozov goes on these mannequins
    g_skipTagsThread = GetCurrentThreadId();
    const int result = g_origPortCheck(portDef, attachDef, flag);
    g_skipTagsThread = 0;
    if (result == 0) ++g_displayPlaced;
    return result;
}

static bool __fastcall PortTagsHook(uintptr_t portDef, uintptr_t attachDef, uint8_t flag) {
    if (g_skipTagsThread == GetCurrentThreadId()) return true;
    return g_origPortTags(portDef, attachDef, flag);
}

static void __fastcall RackFillHook(uintptr_t shop, uintptr_t ctx, uintptr_t ports) {
    const bool display = IsDisplayShop(shop) && ResolveDisplayDefs();
    if (!display) { g_origRackFill(shop, ctx, ports); return; }
    const int before = g_displayPlaced;
    g_displayThread = GetCurrentThreadId();
    g_origRackFill(shop, ctx, ports);
    g_displayThread = 0;
    if (g_displayPlaced != before)
        Log("[shop] %s: the Crusader Morozov-SH is on display (%d piece(s) placed)", kDisplayShop, g_displayPlaced - before);
}

static void HookMorozovDisplay(const Section& text, const Section& rdata) {
    const uint8_t* name = FindCString(rdata, "CEntityComponentShop::PopulateStandardRackItems");
    uint8_t* lea = name ? FindRipLea(text, 0x4C, 0x8D, 0x05, name) : nullptr;
    uint8_t* fill = lea ? lea - 0x97 : nullptr;
    uint8_t* check = fill && fill[0x3AA] == 0xE8 ? CallTarget(fill + 0x3AA) : nullptr;
    uint8_t* tags = check && check[0x53] == 0xE8 ? CallTarget(check + 0x53) : nullptr;
    if (!fill || !BytesMatch(fill, kRackFillPrologue) || !BytesMatch(fill + 0x379, "48 8B 0D") || fill[0x383] != 0xE8 ||
        !check || !BytesMatch(check, kPortCheckPrologue) || !tags || !BytesMatch(tags, kPortTagsPrologue)) {
        Log("[!] shops: rack filling not found; no Morozov display at Cubby Blast");
        return;
    }
    g_attachRegistry = reinterpret_cast<uintptr_t*>(fill + 0x379 + 7 + Rel32(fill + 0x379 + 3));
    g_attachDefOf = reinterpret_cast<AttachDefFn>(CallTarget(fill + 0x383));
    if (HookFunction(tags, 10, reinterpret_cast<void*>(&PortTagsHook), reinterpret_cast<void**>(&g_origPortTags)) &&
        HookFunction(check, 10, reinterpret_cast<void*>(&PortCheckHook), reinterpret_cast<void**>(&g_origPortCheck)) &&
        HookFunction(fill, 15, reinterpret_cast<void*>(&RackFillHook), reinterpret_cast<void**>(&g_origRackFill)))
        Log("[+] shops: Cubby Blast (Area18) puts the Crusader Morozov-SH on its heavy-armor mannequins");
}

// ---- Resolve -------------------------------------------------------------------------------------


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

    const uint8_t* regName = FindCString(rdata, "CShopManager::RegisterShop");
    uint8_t* regLea = regName ? FindRipLea(text, 0x4C, 0x8D, 0x05, regName) : nullptr;
    uint8_t* reg = regLea ? regLea - 0x73 : nullptr;
    if (!reg || !BytesMatch(reg, kRegisterShopPrologue) ||
        !HookFunction(reg, 10, reinterpret_cast<void*>(&RegisterShopHook), reinterpret_cast<void**>(&g_origRegisterShop)))
        Log("[!] shops: RegisterShop not found; every shop's list goes to the first shop");

    const uint8_t* buyName = FindCString(rdata, "CEntityComponentShop::ProcessStandardItemBuyRequest");
    uint8_t* buyLea = buyName ? FindRipLea(text, 0x4C, 0x8D, 0x05, buyName) : nullptr;
    uint8_t* buy = buyLea ? buyLea - 0xC6 : nullptr;
    if (buy && BytesMatch(buy, kBuyPrologue) &&
        HookFunction(buy, 15, reinterpret_cast<void*>(&BuyHook), reinterpret_cast<void**>(&g_origBuy)))
        Log("[+] shops: buying offline (wallet pays, the item drops at your feet)");
    else
        Log("[!] shops: the buy request handler wasn't found; buying fails with Transaction Service Error");

    HookMorozovDisplay(text, rdata);

    const uint8_t* loadName = FindCString(rdata, "CShopInventory::LoadInventoryFromJSON");
    uint8_t* loadLea = loadName ? FindRipLea(text, 0x4C, 0x8D, 0x05, loadName) : nullptr;
    uint8_t* load = loadLea ? loadLea - 0x8A : nullptr;
    if (load && BytesMatch(load, kLoadInventoryPrologue) && load[0x33B] == 0xE8)
        g_guidParse = reinterpret_cast<GuidParseFn>(CallTarget(load + 0x33B));
    else
        Log("[!] shops: the game's GUID parser wasn't found; GUID order comes from the catalog's check line only");

    if (!HookFunction(rpc, 10, reinterpret_cast<void*>(&RpcHook), reinterpret_cast<void**>(&g_origRpc))) {
        Log("[!] shops: couldn't hook the Diffusion RPC");
        return;
    }
    Log("[+] shops: answering shop listings offline from data\\shop_catalog.txt");
}
