"""gen_shop_catalog.py: build data\\shop_catalog.txt (what offline shops sell, and for how much) from the game's
own data, exported with StarBreaker (https://github.com/diogotr7/StarBreaker):

  starbreaker dcb query "EntityClassDefinition.Components[SCItemPurchasableParams]" > purchasable.txt
  starbreaker dcb extract -o veh --format json --filter "**/entities/spaceships/*"     (and groundvehicles)

  python gen_shop_catalog.py <purchasable.txt> <veh dir> [seed prices tsv] [out]

The game has no prices of its own (they come from CIG's shop service), so ships use a seed list where we have one
(old Data\\Scripts\\ShopInventories prices, class<TAB>price) and everything else a price by its display type.
Output lines: <kind>\\t<class>\\t<price> with kind = vehicle | ship (ship parts) | personal; plus one
"check\\t<class>\\t<guid>" line the mod uses to confirm it formats class GUIDs like the game."""
import glob
import json
import os
import re
import sys

VEHICLE_PRICE = {
    'lightfighter': 1_600_000, 'heavyfighter': 4_000_000, 'mediumfighter': 2_500_000, 'mediumfreight': 4_000_000,
    'dropship': 8_000_000, 'luxury': 6_000_000, 'mediumfightermediumfreight': 3_500_000, 'racing': 1_500_000,
    'expedition': 10_000_000, 'pathfinder': 2_000_000, 'stealthfighter': 3_000_000, 'medical': 5_000_000,
    'corvette': 25_000_000, 'gunship': 10_000_000, 'lightfreight': 1_800_000, 'courier': 1_200_000,
    'interceptor': 2_000_000, 'heavyrefuelling': 15_000_000, 'starterlightfreight': 900_000, 'ground': 150_000,
}
# display type token (after the last '_' or the whole thing) -> (kind, price)
ITEM_PRICE = {
    'Helmet': ('personal', 2500), 'Jacket': ('personal', 900), 'ArmorCore': ('personal', 4500),
    'ArmorLegs': ('personal', 3000), 'ArmorArms': ('personal', 2500), 'Pants': ('personal', 500),
    'Shirt': ('personal', 350), 'Shoes': ('personal', 450), 'Undersuit': ('personal', 1800), 'Hat': ('personal', 250),
    'ArmorBackpack': ('personal', 2500), 'Gloves': ('personal', 300), 'Rifle': ('personal', 6500),
    'Pistol': ('personal', 1500), 'SMG': ('personal', 4200), 'Shotgun': ('personal', 5200), 'Sniper': ('personal', 9500),
    'Magazine': ('personal', 80), 'MeleeWeapon': ('personal', 600), 'Device': ('personal', 800),
    'TypeGadget': ('personal', 800), 'OpticsSlot': ('personal', 900), 'BarrelSlot': ('personal', 700),
    'TypeMobiGlas': ('personal', 1200), 'food': ('personal', 8), 'drink': ('personal', 6), 'vice': ('personal', 40),
    'TypeNovelty': ('personal', 150), 'description': ('personal', 300), 'Consumables': ('personal', 1500),
    'MiningLaser': ('ship', 25000), 'Cannon': ('ship', 22000), 'Repeater': ('ship', 20000), 'Gatling': ('ship', 24000),
    'Beam': ('ship', 30000), 'MissileRack': ('ship', 12000), 'TypeOrdinance': ('ship', 2500), 'Cooler': ('ship', 15000),
    'TypePowerPlant': ('ship', 25000), 'ShieldGenerator': ('ship', 25000), 'QuantumDrive': ('ship', 30000),
    'TypeRadar': ('ship', 9000), 'TypeFuelTank': ('ship', 6000), 'MountFluid': ('ship', 4000),
    'TypePaints': ('ship', 15000), 'SubTypeFlightBlade': ('ship', 12000),
}
SKIP_TYPES = ('@LOC_PLACEHOLDER', '')
# AI, mission and event variants of ships that no dealer sells
VEHICLE_EXCLUDE = re.compile(r'_AI_|_AI$|Unmanned|Template|Wreck|Derelict|Hijacked|Destroyed|S42|Boarded|Tutorial|_NPC|Fake|'
                             r'Damaged|_SM_|Swarm|Prop|Dummy|Indestructible|NoInterior|Mission|Showdown|Collector|_EA_|_EA$|'
                             r'GameMaster|_Test|Advocacy|Pirate|_FW', re.I)


def item_kind_price(display_type):
    token = display_type.lstrip('@')
    for key, value in ITEM_PRICE.items():
        if token.endswith(key):
            return value
    return ('personal', 1000)


def main():
    purchasable, veh_dir = sys.argv[1], sys.argv[2]
    seeds_path = sys.argv[3] if len(sys.argv) > 3 else None
    out_path = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(__file__), '..', '..', 'data', 'shop_catalog.txt')

    seeds = {}
    if seeds_path and os.path.exists(seeds_path):
        for line in open(seeds_path, encoding='utf-8'):
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 2:
                seeds[parts[0]] = max(seeds.get(parts[0], 0), int(float(parts[1])))

    check = None
    for path in glob.glob(os.path.join(veh_dir, '**', '*.json'), recursive=True):
        if path.lower().endswith('aegs_gladius.json'):
            d = json.load(open(path, encoding='utf-8'))
            check = ('AEGS_Gladius', d['_RecordId_'].lower())
            break

    rows, skipped = [], 0
    for line in open(purchasable, encoding='utf-8'):
        if '\t' not in line:
            continue
        name, js = line.rstrip('\n').split('\t', 1)
        try:
            d = json.loads(js)
        except ValueError:
            continue
        cls = name.split('.', 1)[1] if '.' in name else name
        display_type = d.get('displayType', '')
        if display_type in SKIP_TYPES or display_type.startswith('@items_commodities_type_') and display_type.split('_')[-1] not in ('food', 'drink', 'vice'):
            skipped += 1
            continue
        if display_type.startswith('@vehicle_'):
            if VEHICLE_EXCLUDE.search(cls):
                skipped += 1
                continue
            vclass = display_type.split('_', 2)[-1]
            rows.append(('vehicle', cls, seeds.get(cls, VEHICLE_PRICE.get(vclass, 3_000_000))))
        else:
            kind, price = item_kind_price(display_type)
            rows.append((kind, cls, price))

    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# ChrisWareOffline shop catalog: <kind>\\t<entity class>\\t<price aUEC>. kind = vehicle | ship | personal.\n')
        f.write('# Generated by tools\\re\\gen_shop_catalog.py from the game data (StarBreaker export). Edit prices freely.\n')
        if check:
            f.write(f'check\t{check[0]}\t{check[1]}\n')
        for kind, cls, price in sorted(rows):
            f.write(f'{kind}\t{cls}\t{price}\n')
    kinds = {}
    for kind, _, _ in rows:
        kinds[kind] = kinds.get(kind, 0) + 1
    print(f'{len(rows)} entries ({kinds}), {skipped} skipped, {len(seeds)} seed prices -> {out_path}')


if __name__ == '__main__':
    main()
