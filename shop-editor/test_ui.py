"""Exercise real Tk widgets; all writes go into a temporary directory."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from app import FieldsDialog, ImportDialog, ShopEditor
from model import ShopDocument


class UiTests(unittest.TestCase):
    def setUp(self):
        self.app = ShopEditor()
        self.app.update()
        self.temporary = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.app.destroy()
        self.temporary.cleanup()

    def test_search_pagination_and_filters(self):
        app = self.app
        self.assertEqual(len(app.tree.get_children()), 200)
        app.turn_page(1)
        self.assertEqual(app.page, 1)
        app.query.set("100001")
        app.refresh()
        self.assertTrue(app.visible)
        self.assertIn("100001", [row[1]["item_id"] for row in app.visible])
        app.clear_filters()
        app.main_filter.set("100 · 服装")
        app.main_changed()
        app.sub_filter.set("106 · 靴子")
        app.refresh()
        self.assertTrue(app.visible)
        self.assertTrue(all(row[1]["sub_category"] == "106" for row in app.visible))

    def test_sale_undo_redo_and_offer_view(self):
        app = self.app
        app.tree.selection_set("0")
        app.set_sale(False)
        self.assertEqual(app.document.tables["items"][0]["on_sale"], "0")
        app.undo()
        self.assertEqual(app.document.tables["items"][0]["on_sale"], "1")
        app.redo()
        self.assertEqual(app.document.tables["items"][0]["on_sale"], "0")
        app.tree.selection_set("0")
        app.show_offers()
        self.assertEqual(app.current_kind, "offers")
        self.assertTrue(app.visible)
        self.assertTrue(all(row[1]["item_id"] == "100001" for row in app.visible))

    def test_editor_dialog_updates_and_saves(self):
        app = self.app
        app.tree.selection_set("0")
        failures = []

        def edit_dialog():
            try:
                dialog = next(child for child in app.winfo_children() if isinstance(child, FieldsDialog))
                dialog.variables["name"].set("商城工具回归测试")
                dialog.accept()
            except Exception as error:
                failures.append(error)
                for child in app.winfo_children():
                    if isinstance(child, FieldsDialog):
                        child.destroy()
        app.after(100, edit_dialog)
        app.edit()
        self.assertEqual(failures, [])
        self.assertEqual(app.document.tables["items"][0]["name"], "商城工具回归测试")
        with patch("app.messagebox.askyesno", return_value=True):
            app.write_directory(self.temporary.name)
        self.assertFalse(app.document.dirty)
        self.assertEqual(ShopDocument.load(self.temporary.name, app.reference).tables["items"][0]["name"],
                         "商城工具回归测试")

    def test_import_dialog_options_and_window_sizes(self):
        app = self.app
        dialog = ImportDialog(app)
        app.update()
        self.assertTrue(all(str(widget["state"]) == "disabled" for widget in dialog.fixed_widgets))
        dialog.variables["mode"].set("统一定价")
        dialog.update_mode()
        self.assertTrue(all(str(widget["state"]) == "normal" for widget in dialog.fixed_widgets))
        dialog.accept()
        self.assertIsNotNone(dialog.result)
        self.assertTrue(dialog.result[1]["fixed"])
        for geometry in ("940x620", "1260x800"):
            app.geometry(geometry)
            app.update()
            self.assertGreater(app.tree.winfo_height(), 180)
            self.assertLess(app.progress.winfo_rootx() + app.progress.winfo_width(),
                            app.winfo_rootx() + app.winfo_width() + 1)


if __name__ == "__main__":
    unittest.main()
