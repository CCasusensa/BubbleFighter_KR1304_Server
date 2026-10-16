"""Extract the server's compiled catalog for the standalone editor."""

import json
import re
from pathlib import Path


def cpp_text(value):
    return json.loads('"' + value + '"')


def build_reference(server):
    server = Path(server)
    generated = server / "src/features/shop/generated"
    shop = (generated / "shop_catalog_data.h").read_text(encoding="utf-8")
    items = {}
    pattern = (
        r'\{(\d+)U, u"((?:\\.|[^"\\])*)", (\d+)U, (\d+)U, (\d+)U, '
        r'ShopCurrency::k(\w+), \d+ULL, \d+ULL, (\d+)U, -?\d+\}'
    )
    for match in re.finditer(pattern, shop):
        ident, name, main, sub, order, currency, maximum = match.groups()
        items[ident] = dict(name=cpp_text(name), main=int(main), sub=int(sub),
                            order=int(order), currency=9 if currency != "Lucci" else 1,
                            maximum=int(maximum))
    inventory = (server / "src/features/inventory/generated/item_catalog_data.h").read_text(
        encoding="utf-8")
    for ident, usage in re.findall(r"\{(\d+)U, InventoryUsage::k(\w+),", inventory):
        if ident in items:
            items[ident]["usage"] = usage.lower()
    stocks = {}
    for filename in ("shop_stock_catalog_data.h", "shop_fallback_stock_data.h"):
        text = (generated / filename).read_text(encoding="utf-8")
        for values in re.findall(
                r"\{(\d+)U, (\d+)U, (true|false), (\d+)U, (\d+)U, (\d+)U, (\d+)U\}", text):
            ident, item, listed, currency, duration, show, sell = values
            stocks[ident] = dict(item=int(item), duration=int(duration), currency=int(currency),
                                 show=int(show), sell=int(sell), listed=listed == "true")
    sets = {}
    text = (generated / "stock_set_catalog_data.h").read_text(encoding="utf-8")
    pattern = (
        r'\{(\d+)U, u"((?:\\.|[^"\\])*)", ShopCurrency::k(\w+), '
        r'(\d+)ULL, (\d+)ULL, (\d+)U, (true|false), (true|false), (true|false), '
        r'(\d+)U, (\d+)U, "([^"]*)", "([^"]*)", "([^"]*)", "([^"]*)", '
        r'\d+U, \d+ULL, \d+ULL, \{\{([^}]*)\}\}\}'
    )
    for match in re.finditer(pattern, text):
        (ident, name, currency, show, sell, bonus, listed, refundable, present,
         kind, level, allowed, denied, main, sub, members) = match.groups()
        sets[ident] = dict(
            name=cpp_text(name), currency=1 if currency == "Lucci" else 9,
            show=int(show), sell=int(sell), bonus=int(bonus), kind=int(kind),
            denied=denied, main_token=main, sub_token=sub,
            members=[int(n) for n in re.findall(r"(\d+)U", members) if int(n)])
    mains = {ident: cpp_text(name) for ident, name in re.findall(
        r'\{(\d+)U, u"((?:\\.|[^"\\])*)", \d+U, true\}', shop)}
    subs = {f"{main}/{ident}": cpp_text(name) for ident, main, name in re.findall(
        r'\{(\d+)U, (\d+)U, u"((?:\\.|[^"\\])*)", \d+U, true, \d+U\}', shop)}
    mains.update({"0": "未分类", "600": "网吧", "800": "礼包", "1100": "奢华俱乐部"})
    subs.update({"0/0": "未分类", "600/600": "全部", "600/601": "武器",
                 "600/602": "服装", "600/603": "饰品", "600/604": "强化武器",
                 "600/605": "强化服装", "800/800": "全部礼包", "800/801": "礼包",
                 "1100/1100": "全部", "1100/1101": "奢华武器", "1100/1102": "奢华服装",
                 "1100/1103": "其他", "200/603": "饰品", "500/502": "聊天颜色"})
    expected = re.search(r"array<ShopItemDefinition, (\d+)>", shop)
    if len(items) != int(expected[1]) or any("usage" not in row for row in items.values()):
        raise ValueError("Compiled item catalog could not be fully extracted.")
    if len(sets) != int(re.search(r"array<StockSetDefinition, (\d+)>", text)[1]):
        raise ValueError("Compiled bundle catalog could not be fully extracted.")
    return dict(items=items, stocks=stocks, sets=sets, mains=mains, subs=subs,
                fingerprint=re.search(r"SHA-256: ([0-9a-f]{64})", shop)[1])
