import copy
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from model import FILES, PERMANENT, ShopDocument, worksheet_rows
from reference import build_reference

SERVER = Path(__file__).resolve().parent.parent
XML = SERVER.parent / "pkn/ConfigSh_00000/Files/Config/ShopItem2.xml"


class ShopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reference = build_reference(SERVER)
        cls.original = ShopDocument.load(SERVER / "data/shop", cls.reference)

    def setUp(self):
        self.document = self.original.clone()
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_current_table_and_reference(self):
        self.assertEqual(len(self.reference["items"]), 7069)
        self.assertEqual(len(self.reference["sets"]), 4110)
        errors, warnings = self.document.validate()
        self.assertEqual(errors, [])
        self.assertTrue(any("不能使用永久期限" in message for message in warnings))
        self.assertEqual(self.reference["subs"]["100/107"], "套装")

    def test_roundtrip_unknown_fields_quotes_and_unicode(self):
        row = self.document.tables["items"][0]
        row["name"] = '中文, "商城" 테스트'
        row["operator_note"] = "keep this"
        self.document.headers["items"].append("operator_note")
        self.document.metadata["custom"] = {"keep": True}
        self.document.save(self.directory)
        loaded = ShopDocument.load(self.directory, self.reference)
        self.assertEqual(loaded.tables["items"][0]["name"], row["name"])
        self.assertEqual(loaded.tables["items"][0]["operator_note"], "keep this")
        self.assertEqual(loaded.metadata["custom"], {"keep": True})
        self.assertEqual(loaded.validate()[0], [])
        self.assertNotIn("price", loaded.tables["offers"][0])

    def test_save_backups_and_external_conflict(self):
        self.document.save(self.directory)
        target = self.directory / FILES["items"]
        original = target.read_bytes()
        self.document.tables["items"][0]["on_sale"] = "0"
        backup, _ = self.document.save(self.directory)
        self.assertEqual((backup / target.name).read_bytes(), original)
        target.write_bytes(target.read_bytes() + b"\n")
        changed = target.read_bytes()
        with self.assertRaisesRegex(ValueError, "外部修改"):
            self.document.save(self.directory)
        self.assertEqual(target.read_bytes(), changed)
        self.assertFalse((self.directory / ".shop-editor.lock").exists())

    def test_partial_save_failure_rolls_back_all_files(self):
        self.document.save(self.directory)
        before = {name: (self.directory / name).read_bytes() for name in (*FILES.values(), "shop_table.json")}
        self.document.tables["items"][0]["name"] = "changed"
        replace = os.replace
        calls = 0

        def fail_once(source, destination):
            nonlocal calls
            calls += 1
            if calls == 3:
                raise OSError("simulated locked file")
            return replace(source, destination)
        with patch("model.os.replace", side_effect=fail_once):
            with self.assertRaises(OSError):
                self.document.save(self.directory)
        self.assertEqual({name: (self.directory / name).read_bytes() for name in before}, before)

    def test_concurrent_editor_lock(self):
        (self.directory / ".shop-editor.lock").touch()
        with self.assertRaisesRegex(ValueError, "另一个商城工具"):
            self.document.save(self.directory)

    def test_invalid_ids_currency_names_and_relationships(self):
        for kind, key, value in (
            ("items", "name", "a\nb"), ("items", "item_id", "99999999"),
            ("offers", "currency", "3"), ("offers", "duration", "0"),
            ("offers", "sell_price", str(0x80000000)), ("offers", "item_id", "99999999"),
        ):
            with self.subTest(kind=kind, key=key):
                document = self.original.clone()
                document.tables[kind][0][key] = value
                self.assertTrue(document.validate()[0])
        self.document.tables["offers"].append(copy.deepcopy(self.document.tables["offers"][0]))
        self.assertTrue(any("重复 ID" in error for error in self.document.validate()[0]))

    def test_bundle_contents_are_immutable(self):
        self.document.tables["sets"][0]["members"] = "1000001"
        self.assertTrue(any("内置定义不符" in error for error in self.document.validate()[0]))

    def test_offerless_item_and_too_many_offers(self):
        item = self.document.tables["items"][0]["item_id"]
        self.document.tables["offers"] = [row for row in self.document.tables["offers"] if row["item_id"] != item]
        self.assertTrue(any("没有报价" in error for error in self.document.validate()[0]))
        template = self.original.tables["offers"][0]
        self.document = self.original.clone()
        for index in range(40):
            self.document.tables["offers"].append(dict(template, stock_id=str(1800000 + index)))
        self.assertTrue(any("超过 32" in error for error in self.document.validate()[0]))

    def test_sparse_spreadsheet_and_nested_text(self):
        path = self.directory / "sparse.xml"
        path.write_text('''<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet"
          xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet">
          <Worksheet ss:Name="ShopItem"><Table><Row>
          <Cell><Data>itemId</Data></Cell><Cell><Data>empty</Data></Cell>
          <Cell><Data>name</Data></Cell></Row>
          <Row ss:Index="10"><Cell><Data>100001</Data></Cell>
          <Cell ss:Index="3"><Data>hello <B>world</B></Data></Cell></Row>
          </Table></Worksheet></Workbook>''', encoding="utf-8")
        self.assertEqual(list(worksheet_rows(path, {"ShopItem"})), [
            ("ShopItem", {"itemId": "100001", "empty": "", "name": "hello world"})])

    def test_real_xml_import_original_and_fixed(self):
        if not XML.is_file():
            self.skipTest("Local client XML is unavailable")
        for fixed in (False, True):
            with self.subTest(fixed=fixed):
                document = ShopDocument.from_xml(
                    XML, self.reference, fixed=fixed, permanent=fixed, include_off_sale=fixed)
                self.assertGreater(len(document.tables["items"]), 100)
                self.assertGreater(len(document.tables["sets"]), 100)
                self.assertEqual(document.validate()[0], [])
                if fixed:
                    for offer in document.tables["offers"]:
                        self.assertEqual(offer["sell_price"], "1000" if offer["currency"] == "1" else "100")
                        item = document.reference_item(offer["item_id"])
                        if item["usage"] == "duration":
                            self.assertEqual(offer["duration"], str(PERMANENT))
                        else:
                            self.assertIn(int(offer["duration"]), (200, 500, 1000))
                directory = self.directory / str(fixed)
                document.save(directory)
                self.assertEqual(ShopDocument.load(directory, self.reference).validate()[0], [])

    def test_incomplete_xml_fails(self):
        path = self.directory / "empty.xml"
        path.write_text('<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet"/>', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "缺少"):
            ShopDocument.from_xml(path, self.reference)


if __name__ == "__main__":
    unittest.main()
