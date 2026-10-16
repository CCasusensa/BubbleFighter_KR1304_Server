"""Shop CSV editing and bounded-memory SpreadsheetML import."""

import copy
import csv
import hashlib
import json
import os
import shutil
import tempfile
import xml.etree.ElementTree as ET
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

NS = "{urn:schemas-microsoft-com:office:spreadsheet}"
FILES = {"items": "shop_items.csv", "offers": "shop_offers.csv", "sets": "shop_sets.csv"}
HEADERS = {
    "items": ["item_id", "name", "main_category", "sub_category", "display_order",
              "duration_kind", "money_type", "offers", "on_sale"],
    "offers": ["stock_id", "item_id", "currency", "duration", "quantity",
               "show_price", "sell_price"],
    "sets": ["stock_set_id", "name", "currency", "show_price", "sell_price",
             "bonus_lucci", "main_category", "sub_category", "members", "on_sale"],
}
PERMANENT = 0xFFFFFFFF
MAX_VALUE = 0x7FFFFFFF


def number(value, label, minimum=0, maximum=PERMANENT):
    text = str(value).strip()
    if not text.isascii() or not text.isdecimal():
        raise ValueError(f"{label} 必须是整数：{value}")
    result = int(text)
    if not minimum <= result <= maximum:
        raise ValueError(f"{label} 超出范围 {minimum} ~ {maximum}：{value}")
    return result


def flag(value):
    text = str(value).strip().lower()
    if text in ("1", "true", "yes"):
        return True
    if text in ("0", "false", "no"):
        return False
    raise ValueError(f"无效的上架状态：{value}")


def signature(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def worksheet_rows(path, wanted):
    """Honor sparse ss:Index cells/rows and release every completed XML row."""
    worksheet = None
    headers = None
    table = None
    with open(path, "rb") as stream:
        prefix = stream.read(8192)
        if b"<!DOCTYPE" in prefix.upper() or b"<!ENTITY" in prefix.upper():
            raise ValueError("不支持包含 DTD / ENTITY 的 XML。")
        stream.seek(0)
        for event, element in ET.iterparse(stream, events=("start", "end")):
            if event == "start":
                if element.tag == NS + "Worksheet":
                    worksheet = element.get(NS + "Name")
                    headers = None
                elif element.tag == NS + "Table":
                    table = element
                continue
            if element.tag == NS + "Row":
                record = None
                if worksheet in wanted:
                    values = []
                    for cell in element.findall(NS + "Cell"):
                        position = number(cell.get(NS + "Index", len(values) + 1),
                                          "XML 列序号", 1, 1024)
                        if position <= len(values):
                            raise ValueError(f"{worksheet} 包含倒序列索引。")
                        values.extend([""] * (position - len(values) - 1))
                        data = cell.find(NS + "Data")
                        values.append("".join(data.itertext()).strip() if data is not None else "")
                    if headers is None:
                        headers = values
                    elif any(values):
                        record = {key: values[index] if index < len(values) else ""
                                  for index, key in enumerate(headers) if key}
                if table is not None:
                    table.remove(element)
                element.clear()
                if record is not None:
                    yield worksheet, record
            elif element.tag == NS + "Worksheet":
                worksheet = None
                table = None
                element.clear()


class ShopDocument:
    def __init__(self, reference):
        self.reference = reference
        self.tables = {kind: [] for kind in FILES}
        self.headers = copy.deepcopy(HEADERS)
        self.metadata = {}
        self.directory = None
        self.fingerprints = {}
        self.dirty = False
        self.report = Counter()

    def clone(self):
        result = copy.copy(self)
        result.tables = copy.deepcopy(self.tables)
        result.headers = copy.deepcopy(self.headers)
        result.metadata = copy.deepcopy(self.metadata)
        return result

    @classmethod
    def load(cls, directory, reference):
        result = cls(reference)
        result.directory = Path(directory).resolve()
        for kind, filename in FILES.items():
            path = result.directory / filename
            with path.open("r", encoding="utf-8-sig", newline="") as stream:
                reader = csv.DictReader(stream)
                if not reader.fieldnames or len(set(reader.fieldnames)) != len(reader.fieldnames):
                    raise ValueError(f"{filename} 表头缺失或重复。")
                rows = []
                for line, row in enumerate(reader, 2):
                    if None in row or any(value is None for value in row.values()):
                        raise ValueError(f"{filename} 第 {line} 行的列数不正确。")
                    rows.append(row)
                required = {
                    "items": ("item_id", "name", "main_category", "sub_category", "display_order"),
                    "offers": ("stock_id", "item_id", "currency", "duration"),
                    "sets": ("stock_set_id", "name", "currency"),
                }[kind]
                if any(key not in reader.fieldnames for key in required):
                    raise ValueError(f"{filename} 缺少必要字段：{', '.join(required)}")
                result.headers[kind] = list(reader.fieldnames)
                if kind in ("offers", "sets"):
                    if not any(key in reader.fieldnames for key in ("price", "show_price", "sell_price")):
                        raise ValueError(f"{filename} 缺少价格字段。")
                    for row in rows:
                        show = row.get("price", row.get("show_price", row.get("sell_price")))
                        row["show_price"] = show
                        row["sell_price"] = row.get("sell_price", show)
                for row in rows:
                    if kind in ("items", "sets"):
                        row["on_sale"] = "1" if flag(row.get("on_sale", "1")) else "0"
                    if kind == "sets":
                        row.setdefault("main_category", "800")
                        row.setdefault("sub_category", "800")
                result.tables[kind] = rows
        meta = result.directory / "shop_table.json"
        if meta.exists():
            result.metadata = json.loads(meta.read_text(encoding="utf-8-sig"))
        result.remember_files()
        return result

    def remember_files(self):
        self.fingerprints = {name: signature(self.directory / name)
                             for name in (*FILES.values(), "shop_table.json")}

    def reference_item(self, ident):
        return self.reference["items"].get(str(ident))

    def bundle_problem(self, ident, currency):
        definition = self.reference["sets"].get(str(ident))
        if not definition:
            return "服务端没有此礼包"
        if definition["kind"] != 1 or definition["denied"] != "none":
            return "礼包类型或角色限制尚不支持"
        if definition["show"] > MAX_VALUE or definition["bonus"] > MAX_VALUE:
            return "礼包内置金额超出范围"
        if currency == 1 and definition["bonus"]:
            return "Lucci 礼包不能附赠 Lucci"
        if not definition["members"] and not definition["bonus"]:
            return "空礼包"
        for stock_id in definition["members"]:
            stock = self.reference["stocks"].get(str(stock_id))
            item = self.reference_item(stock["item"]) if stock else None
            if not 1000000 <= stock_id < 2000000 or not stock or not item or not item["maximum"]:
                return "礼包包含不支持的内置报价"
            duration = stock["duration"]
            if item["usage"] not in ("count", "duration", "life") or not duration:
                return "礼包物品用途或数量无效"
            if item["usage"] == "duration":
                if duration != PERMANENT and duration > 10000:
                    return "礼包期限超出范围"
            elif duration > MAX_VALUE:
                return "礼包数量超出范围"
        return None

    def validate(self):
        errors, warnings = [], []
        item_ids, stock_ids, set_ids = set(), set(), set()
        offers_per_item = Counter()
        for kind, rows in self.tables.items():
            primary = {"items": "item_id", "offers": "stock_id", "sets": "stock_set_id"}[kind]
            seen = {"items": item_ids, "offers": stock_ids, "sets": set_ids}[kind]
            for line, row in enumerate(rows, 2):
                try:
                    ident = number(row[primary], primary, 1)
                    if ident in seen:
                        raise ValueError(f"重复 ID：{ident}")
                    seen.add(ident)
                    if kind in ("items", "sets"):
                        name = row["name"].strip()
                        if not name or len(name.encode("utf-16-le")) // 2 > 128:
                            raise ValueError("名称必须为 1 ~ 128 个 UTF-16 单元")
                        if any(char in name for char in "\r\n\0"):
                            raise ValueError("名称不能包含换行或空字符")
                        main = number(row["main_category"], "主分类")
                        sub = number(row["sub_category"], "子分类")
                        if str(main) not in self.reference["mains"] or f"{main}/{sub}" not in self.reference["subs"]:
                            warnings.append(f"{primary} {ident} 的分类 {main}/{sub} 无已知页面")
                        flag(row["on_sale"])
                    if kind == "items":
                        number(row["display_order"], "显示顺序")
                        if not self.reference_item(ident):
                            raise ValueError("当前服务端未内置此物品；需要先更新服务端物品定义")
                    else:
                        currency = number(row["currency"], "货币")
                        if currency not in (1, 9):
                            raise ValueError("货币仅支持 1=Lucci、9=Cash")
                        for key in ("show_price", "sell_price"):
                            number(row[key], key, 0, MAX_VALUE)
                        if kind == "offers":
                            item_id = number(row["item_id"], "物品 ID", 1)
                            if item_id not in item_ids:
                                raise ValueError(f"报价引用了不存在的商品 {item_id}")
                            known_stock = self.reference["stocks"].get(str(ident))
                            if not 1000000 <= ident < 2000000 and not (
                                    known_stock and known_stock["item"] == item_id):
                                raise ValueError("新增报价 ID 必须在 1000000 ~ 1999999")
                            duration = number(row["duration"], "期限 / 数量", 1)
                            definition = self.reference_item(item_id)
                            if not definition or not definition["maximum"]:
                                raise ValueError("此物品不支持售卖")
                            if definition["usage"] == "duration":
                                if duration != PERMANENT and duration > min(10000, definition["maximum"]):
                                    raise ValueError(f"期限超过物品上限 {min(10000, definition['maximum'])} 天")
                            elif definition["usage"] not in ("count", "life"):
                                raise ValueError("不支持的物品用途")
                            elif duration > MAX_VALUE:
                                warnings.append(f"报价 {ident} 为计数 / 耐久物品，不能使用永久期限")
                            elif currency == 1 and definition["usage"] == "count" and duration > definition["maximum"]:
                                warnings.append(f"报价 {ident} 数量超过内置上限，Lucci 购买可能被拒绝")
                            offers_per_item[item_id] += 1
                        else:
                            problem = self.bundle_problem(ident, currency)
                            if problem:
                                warnings.append(f"礼包 {ident}：{problem}，购买会被拒绝")
                            if str(ident) not in self.reference["sets"]:
                                raise ValueError("服务端没有此礼包")
                            definition = self.reference["sets"][str(ident)]
                            if "bonus_lucci" in row and number(row["bonus_lucci"], "礼包赠送 Lucci") != definition["bonus"]:
                                raise ValueError("礼包赠送 Lucci 与服务端内置定义不符")
                            if "members" in row:
                                members = [number(value, "礼包成员", 1) for value in row["members"].split()]
                                if members != definition["members"]:
                                    raise ValueError("礼包成员与服务端内置定义不符")
                except (ValueError, KeyError) as error:
                    errors.append(f"{FILES[kind]} 第 {line} 行：{error}")
        for ident in item_ids:
            if not offers_per_item[ident]:
                errors.append(f"商品 {ident} 没有报价，服务端会丢弃此行")
            elif offers_per_item[ident] > 32:
                errors.append(f"商品 {ident} 报价超过 32 条")
        for ident in stock_ids & set_ids:
            errors.append(f"报价与礼包的 ID 冲突：{ident}")
        return errors, warnings

    def save(self, directory, force=False):
        directory = Path(directory).resolve()
        directory.mkdir(parents=True, exist_ok=True)
        lock = directory / ".shop-editor.lock"
        try:
            descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError as error:
            raise ValueError("另一个商城工具正在保存此目录，请稍后再试。") from error
        try:
            os.close(descriptor)
            return self._save_locked(directory, force)
        finally:
            lock.unlink()

    def _save_locked(self, directory, force=False):
        directory = Path(directory).resolve()
        errors, warnings = self.validate()
        if errors:
            raise ValueError("\n".join(errors[:25]))
        same = self.directory == directory
        if same and not force:
            for filename, original in self.fingerprints.items():
                if signature(directory / filename) != original:
                    raise ValueError(f"{filename} 已被外部修改，请重新打开后再保存。")
        directory.mkdir(parents=True, exist_ok=True)
        names = [*FILES.values(), "shop_table.json"]
        original_bytes = {name: (directory / name).read_bytes() if (directory / name).is_file()
                          else None for name in names}
        backup = None
        if any(value is not None for value in original_bytes.values()):
            backup = directory / "backups" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
            backup.mkdir(parents=True)
            for name, value in original_bytes.items():
                if value is not None:
                    (backup / name).write_bytes(value)
        counts = Counter(row["item_id"] for row in self.tables["offers"])
        staged = Path(tempfile.mkdtemp(prefix=".shop-editor-", dir=directory))
        replaced = []
        try:
            for kind, filename in FILES.items():
                fields = list(self.headers[kind])
                if kind in ("offers", "sets"):
                    fields = [key for key in fields if key not in ("price", "show_price", "sell_price")]
                    fields.extend(("show_price", "sell_price"))
                for key in HEADERS[kind]:
                    if key not in fields:
                        fields.append(key)
                with (staged / filename).open("w", encoding="utf-8", newline="") as stream:
                    writer = csv.DictWriter(stream, fields, extrasaction="raise", lineterminator="\n")
                    writer.writeheader()
                    for source in self.tables[kind]:
                        row = dict(source)
                        row.pop("price", None) if kind != "items" else None
                        if kind == "items":
                            row["offers"] = str(counts[row["item_id"]])
                            definition = self.reference_item(row["item_id"])
                            row.setdefault("duration_kind", "day" if definition["usage"] == "duration" else definition["usage"])
                            row.setdefault("money_type", "lucci" if definition["currency"] == 1 else "cash")
                        if kind == "offers":
                            definition = self.reference_item(row["item_id"])
                            row["quantity"] = row["duration"] if definition["usage"] != "duration" else "0"
                        if kind == "sets":
                            definition = self.reference["sets"][row["stock_set_id"]]
                            row.setdefault("bonus_lucci", str(definition["bonus"]))
                            row.setdefault("members", " ".join(map(str, definition["members"])))
                        writer.writerow(row)
            metadata = copy.deepcopy(self.metadata)
            metadata["counts"] = {kind: len(rows) for kind, rows in self.tables.items()}
            metadata["editor"] = {"version": 1, "saved_at": datetime.now(timezone.utc).isoformat(),
                                  "server_catalog_sha256": self.reference["fingerprint"]}
            (staged / "shop_table.json").write_text(
                json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            if same and not force:
                for filename, original in self.fingerprints.items():
                    if signature(directory / filename) != original:
                        raise ValueError(f"{filename} 在保存期间被外部修改，请重新打开。")
            for filename in names:
                os.replace(staged / filename, directory / filename)
                replaced.append(filename)
        except BaseException:
            for filename in reversed(replaced):
                original = original_bytes[filename]
                if original is None:
                    (directory / filename).unlink(missing_ok=True)
                else:
                    (staged / filename).write_bytes(original)
                    os.replace(staged / filename, directory / filename)
            raise
        finally:
            shutil.rmtree(staged)
        self.directory = directory
        self.metadata = metadata
        self.remember_files()
        self.dirty = False
        return backup, warnings

    @classmethod
    def from_xml(cls, path, reference, fixed=False, lucci=1000, cash=100,
                 permanent=False, quantities=(200, 500, 1000), include_off_sale=False):
        path = Path(path)
        result = cls(reference)
        main_tokens, sub_tokens = {}, {}
        raw_items, raw_stocks, raw_sets = {}, {}, {}
        wanted = {"ShopItem", "Stock", "StockSet", "MainCategory", "SubCategory"}
        for sheet, row in worksheet_rows(path, wanted):
            key = {"ShopItem": "itemId", "Stock": "stockId", "StockSet": "stockSetId",
                   "MainCategory": "id", "SubCategory": "id"}[sheet]
            if not row.get(key) or row[key].startswith("//"):
                continue
            try:
                ident = str(number(row[key], sheet + "." + key))
            except ValueError:
                result.report["无效 XML ID"] += 1
                continue
            target = {"ShopItem": raw_items, "Stock": raw_stocks, "StockSet": raw_sets,
                      "MainCategory": main_tokens, "SubCategory": sub_tokens}[sheet]
            if ident in target:
                raise ValueError(f"{sheet} 存在重复 ID {ident}，导入已取消。")
            target[ident] = row
        if not all((raw_items, raw_stocks, raw_sets, main_tokens, sub_tokens)):
            raise ValueError("XML 缺少 ShopItem / Stock / StockSet / MainCategory / SubCategory 数据。")
        main_ids = {row.get("name", ""): ident for ident, row in main_tokens.items()}
        sub_ids = {row.get("name", ""): ident for ident, row in sub_tokens.items()}
        lucci = number(lucci, "Lucci 价格", 1, MAX_VALUE)
        cash = number(cash, "Cash 价格", 1, MAX_VALUE)
        quantities = tuple(number(n, "数量", 1, MAX_VALUE) for n in quantities)
        offers = {}
        for ident, row in raw_stocks.items():
            if not flag(row.get("isOnSale", "0")) and not include_off_sale:
                result.report["跳过下架报价"] += 1
                continue
            offers.setdefault(row.get("itemId"), []).append((ident, row))
        for ident, source in raw_items.items():
            definition = result.reference_item(ident)
            if not definition or definition["usage"] not in ("duration", "count", "life") or not definition["maximum"]:
                result.report["服务端未支持物品"] += 1
                continue
            listed = flag(source.get("isOnSale", "0"))
            if not listed and not include_off_sale:
                result.report["跳过下架物品"] += 1
                continue
            money = source.get("moneyType", "").lower()
            if money not in ("lucci", "cash"):
                result.report["不支持货币"] += 1
                continue
            main = main_ids.get(source.get("mainCategory", ""))
            sub = sub_ids.get(source.get("subCategory", ""))
            if main not in reference["mains"] or f"{main}/{sub}" not in reference["subs"]:
                result.report["不支持分类"] += 1
                continue
            currency = 1 if money == "lucci" else 9
            valid = []
            candidates = offers.get(ident, [])
            listed_candidates = [(stock_id, stock) for stock_id, stock in candidates
                                 if flag(stock.get("isOnSale", "0"))]
            if listed_candidates:
                candidates = listed_candidates
            elif candidates:
                listed = False
            used_amounts = set()
            for stock_id, stock in candidates:
                if len(valid) >= 32:
                    result.report["超出报价条数"] += 1
                    continue
                try:
                    amount = number(stock["duration"], "duration", 1)
                    if permanent and definition["usage"] == "duration":
                        amount = PERMANENT
                    if fixed and definition["usage"] != "duration":
                        index = len(valid)
                        if index >= len(quantities):
                            continue
                        amount = quantities[index]
                    if definition["usage"] == "duration":
                        if amount != PERMANENT and amount > min(10000, definition["maximum"]):
                            raise ValueError("期限超限")
                    elif amount > MAX_VALUE:
                        raise ValueError("数量超限")
                    if amount in used_amounts:
                        continue
                    show = sell = lucci if currency == 1 else cash
                    if not fixed:
                        show = number(stock["showPrice"], "showPrice", 0, MAX_VALUE)
                        sell = number(stock["sellPrice"], "sellPrice", 0, MAX_VALUE)
                    used_amounts.add(amount)
                    valid.append(dict(stock_id=stock_id, item_id=ident, currency=str(currency),
                                      duration=str(amount), quantity=str(amount if definition["usage"] != "duration" else 0),
                                      show_price=str(show), sell_price=str(sell)))
                except ValueError:
                    result.report["无效或超限报价"] += 1
            if not valid:
                result.report["没有有效报价的物品"] += 1
                continue
            result.tables["items"].append(dict(
                item_id=ident, name=" ".join(source["itemName"].splitlines()).strip(), main_category=main, sub_category=sub,
                display_order=str(number(source.get("displayOrder", "0") or "0", "displayOrder")),
                duration_kind="day" if definition["usage"] == "duration" else definition["usage"],
                money_type=money, offers=str(len(valid)), on_sale="1" if listed else "0"))
            result.tables["offers"].extend(valid)
        for ident, source in raw_sets.items():
            listed = flag(source.get("isOnSale", "0"))
            if not listed and not include_off_sale:
                result.report["跳过下架礼包"] += 1
                continue
            money = source.get("moneyType", "").lower()
            if money not in ("lucci", "cash"):
                result.report["不支持礼包货币"] += 1
                continue
            currency = 1 if money == "lucci" else 9
            problem = result.bundle_problem(ident, currency)
            if problem:
                result.report[problem] += 1
                continue
            definition = reference["sets"][ident]
            xml_members = [int(source.get(f"stockId{i:02}", "0") or "0") for i in range(1, 11)]
            xml_members = [n for n in xml_members if n]
            if xml_members != definition["members"] or int(source.get("lucci", "0") or "0") != definition["bonus"]:
                result.report["礼包内容与服务端内置定义不同"] += 1
                continue
            try:
                show = sell = lucci if currency == 1 else cash
                if not fixed:
                    show = number(source["showPrice"], "showPrice", 1, MAX_VALUE)
                    sell = number(source["sellPrice"], "sellPrice", 1, MAX_VALUE)
            except ValueError:
                result.report["无效礼包价格"] += 1
                continue
            main = main_ids.get(source.get("mainCategory", ""), "800")
            sub = sub_ids.get(source.get("subCategory", ""), "800")
            if f"{main}/{sub}" not in reference["subs"]:
                main = sub = "800"
            result.tables["sets"].append(dict(
                stock_set_id=ident, name=" ".join(source["stockSetName"].splitlines()).strip(), currency=str(currency),
                show_price=str(show), sell_price=str(sell), bonus_lucci=str(definition["bonus"]),
                main_category=main, sub_category=sub,
                members=" ".join(map(str, definition["members"])), on_sale="1" if listed else "0"))
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        result.metadata = {
            "source": {"path": str(path.resolve()), "sha256": digest},
            "rules": {"fixed_prices": fixed, "lucci_price": lucci, "cash_price": cash,
                      "permanent": permanent, "quantities": list(quantities),
                      "include_off_sale": include_off_sale},
            "import_report": dict(result.report),
        }
        result.dirty = True
        errors, _ = result.validate()
        if errors:
            raise ValueError("\n".join(errors[:20]))
        return result
