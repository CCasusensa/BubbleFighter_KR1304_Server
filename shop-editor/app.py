"""KR1304 native Windows shop maintenance tool."""

import argparse
import ctypes
import json
import math
import queue
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk

from model import MAX_VALUE, PERMANENT, ShopDocument, flag, number

TITLE = "KR1304 商城表维护"
DEFAULT_XML = Path(r"D:\BF\KR1304\pkn\ConfigSh_00000\Files\Config\ShopItem2.xml")
PAGE_SIZE = 200
LABELS = {
    "item_id": "物品 ID", "name": "名称", "main_category": "主分类",
    "sub_category": "子分类", "display_order": "显示顺序", "on_sale": "上架",
    "stock_id": "报价 ID", "currency": "货币", "duration": "期限 / 数量",
    "show_price": "展示价格", "sell_price": "实际价格", "stock_set_id": "礼包 ID",
    "bonus_lucci": "附赠 Lucci", "members": "内置成员报价 ID",
}
COLUMNS = {
    "items": ("item_id", "name", "main_category", "sub_category", "display_order", "offers", "on_sale"),
    "offers": ("stock_id", "item_id", "name", "currency", "duration", "show_price", "sell_price"),
    "sets": ("stock_set_id", "name", "main_category", "sub_category", "currency", "sell_price", "on_sale"),
}


def server_root():
    current = Path(sys.executable if getattr(sys, "frozen", False) else __file__).resolve().parent
    for root in (current, *current.parents):
        if (root / "data/shop/shop_items.csv").exists():
            return root
    return current


def load_reference():
    if getattr(sys, "frozen", False):
        return json.loads((Path(sys._MEIPASS) / "catalog.json").read_text(encoding="utf-8"))
    from reference import build_reference
    return build_reference(Path(__file__).resolve().parent.parent)


class TextWindow(tk.Toplevel):
    def __init__(self, parent, title, text):
        super().__init__(parent)
        self.title(title)
        self.geometry("880x580")
        self.minsize(560, 340)
        self.transient(parent)
        box = ttk.Frame(self, padding=12)
        box.pack(fill="both", expand=True)
        content = tk.Text(box, wrap="word", font=("Microsoft YaHei UI", 10), padx=12, pady=10)
        scroll = ttk.Scrollbar(box, command=content.yview)
        content.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        content.pack(fill="both", expand=True)
        content.insert("1.0", text)
        content.configure(state="disabled")
        ttk.Button(self, text="关闭", command=self.destroy).pack(pady=(0, 12))


class FieldsDialog(tk.Toplevel):
    def __init__(self, app, title, fields, values, readonly=(), choices=None, on_change=None):
        super().__init__(app)
        self.title(title)
        self.resizable(False, False)
        self.transient(app)
        self.result = None
        self.variables = {}
        self.widgets = {}
        self.on_change = on_change
        form = ttk.Frame(self, padding=18)
        form.pack(fill="both", expand=True)
        choices = choices or {}
        for index, key in enumerate(fields):
            ttk.Label(form, text=LABELS.get(key, key)).grid(row=index, column=0, sticky="w", padx=(0, 18), pady=6)
            if key == "on_sale":
                variable = tk.BooleanVar(value=flag(values.get(key, "1")))
                widget = ttk.Checkbutton(form, variable=variable)
            else:
                variable = tk.StringVar(value=values.get(key, ""))
                if key in choices:
                    widget = ttk.Combobox(form, textvariable=variable, values=choices[key],
                                          state="readonly", width=44)
                else:
                    widget = ttk.Entry(form, textvariable=variable, width=47,
                                       state="readonly" if key in readonly else "normal")
            self.variables[key] = variable
            self.widgets[key] = widget
            widget.grid(row=index, column=1, sticky="ew", pady=6)
        self.error = tk.StringVar()
        ttk.Label(form, textvariable=self.error, foreground="#b42318", wraplength=470).grid(
            row=len(fields), column=0, columnspan=2, sticky="w", pady=8)
        commands = ttk.Frame(form)
        commands.grid(row=len(fields) + 1, column=0, columnspan=2, sticky="e")
        ttk.Button(commands, text="取消", command=self.destroy).pack(side="left", padx=5)
        ttk.Button(commands, text="确定", command=self.accept).pack(side="left")
        self.bind("<Escape>", lambda _: self.destroy())
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.grab_set()

    def accept(self):
        values = {key: ("1" if variable.get() else "0") if isinstance(variable, tk.BooleanVar)
                  else variable.get().strip() for key, variable in self.variables.items()}
        try:
            if self.on_change:
                self.on_change(values)
        except (ValueError, KeyError) as error:
            self.error.set(str(error))
            return
        self.result = values
        self.destroy()


class ImportDialog(FieldsDialog):
    def __init__(self, app):
        self.app = app
        super().__init__(
            app, "从 ShopItem2.xml 生成", (),
            {}, on_change=None)
        for child in self.winfo_children():
            child.destroy()
        self.variables = {
            "path": tk.StringVar(value=str(DEFAULT_XML)),
            "mode": tk.StringVar(value="XML 原价"),
            "lucci": tk.StringVar(value="1000"),
            "cash": tk.StringVar(value="100"),
            "quantities": tk.StringVar(value="200,500,1000"),
            "permanent": tk.BooleanVar(value=False),
            "include_off_sale": tk.BooleanVar(value=False),
        }
        form = ttk.Frame(self, padding=18)
        form.pack(fill="both", expand=True)
        ttk.Label(form, text="XML 文件").grid(row=0, column=0, sticky="w", pady=8)
        ttk.Entry(form, textvariable=self.variables["path"], width=60).grid(row=0, column=1, sticky="ew")
        ttk.Button(form, text="选择文件", command=self.choose).grid(row=0, column=2, padx=(8, 0))
        ttk.Label(form, text="报价方式").grid(row=1, column=0, sticky="w", pady=8)
        mode = ttk.Combobox(form, textvariable=self.variables["mode"],
                           values=("XML 原价", "统一定价"), state="readonly")
        mode.grid(row=1, column=1, sticky="w")
        mode.bind("<<ComboboxSelected>>", lambda _: self.update_mode())
        self.fixed_widgets = []
        for index, (key, label) in enumerate((("lucci", "Lucci 价格"), ("cash", "Cash 价格"),
                                              ("quantities", "计数报价数量")), 2):
            ttk.Label(form, text=label).grid(row=index, column=0, sticky="w", pady=8, padx=(0, 14))
            entry = ttk.Entry(form, textvariable=self.variables[key], width=30)
            entry.grid(row=index, column=1, sticky="w")
            self.fixed_widgets.append(entry)
        permanent = ttk.Checkbutton(form, text="期限物品改为永久", variable=self.variables["permanent"])
        permanent.grid(row=5, column=1, sticky="w", pady=8)
        self.fixed_widgets.append(permanent)
        ttk.Checkbutton(form, text="包含下架商品和礼包", variable=self.variables["include_off_sale"]).grid(
            row=6, column=1, sticky="w", pady=8)
        self.error = tk.StringVar()
        ttk.Label(form, textvariable=self.error, foreground="#b42318", wraplength=500).grid(
            row=7, column=0, columnspan=3, sticky="w", pady=8)
        commands = ttk.Frame(form)
        commands.grid(row=8, column=0, columnspan=3, sticky="e")
        ttk.Button(commands, text="取消", command=self.destroy).pack(side="left", padx=8)
        ttk.Button(commands, text="生成预览", command=self.accept).pack(side="left")
        self.update_mode()

    def update_mode(self):
        for widget in self.fixed_widgets:
            widget.configure(state="normal" if self.variables["mode"].get() == "统一定价" else "disabled")

    def choose(self):
        path = filedialog.askopenfilename(parent=self, filetypes=[("ShopItem XML", "*.xml")])
        if path:
            self.variables["path"].set(path)

    def accept(self):
        try:
            values = {key: value.get() for key, value in self.variables.items()}
            path = Path(values.pop("path"))
            if not path.is_file():
                raise ValueError("请选择有效的 XML 文件。")
            fixed = values.pop("mode") == "统一定价"
            quantities = tuple(number(value.strip(), "计数报价数量", 1, MAX_VALUE)
                               for value in values["quantities"].replace("，", ",").split(","))
            options = dict(fixed=fixed, lucci=number(values["lucci"], "Lucci", 1, MAX_VALUE),
                           cash=number(values["cash"], "Cash", 1, MAX_VALUE), quantities=quantities,
                           permanent=values["permanent"] if fixed else False,
                           include_off_sale=values["include_off_sale"])
        except ValueError as error:
            self.error.set(str(error))
            return
        self.result = path, options
        self.destroy()


class ShopEditor(tk.Tk):
    def __init__(self, reference=None, directory=None):
        super().__init__()
        self.title(TITLE)
        self.geometry("1260x800")
        self.minsize(940, 620)
        self.option_add("*Font", ("Microsoft YaHei UI", 10))
        self.reference = reference or load_reference()
        self.document = ShopDocument(self.reference)
        self.history, self.future = [], []
        self.current_kind = "items"
        self.page = 0
        self.visible = []
        self.busy = False
        self.jobs = queue.Queue()
        self.pending_search = None
        self.item_filter = None
        self.folder = tk.StringVar()
        self.query = tk.StringVar()
        self.sale = tk.StringVar(value="全部状态")
        self.main_filter = tk.StringVar(value="全部主分类")
        self.sub_filter = tk.StringVar(value="全部子分类")
        self.status = tk.StringVar(value="就绪")
        self.page_label = tk.StringVar()
        self.detail = tk.StringVar()
        self.build_ui()
        self.bind("<Configure>", self.resize_labels)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.bind("<Control-s>", lambda _: self.save())
        self.bind("<Control-o>", lambda _: self.open_directory())
        self.bind("<Control-z>", lambda _: self.undo())
        self.bind("<Control-y>", lambda _: self.redo())
        self.poll_id = self.after(100, self.poll_jobs)
        directory = directory or server_root() / "data/shop"
        if Path(directory).is_dir():
            self.load_directory(directory)

    def build_ui(self):
        self.columnconfigure(0, weight=1)
        self.rowconfigure(5, weight=1)
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure(".", background="#f4f5f7", foreground="#20242b")
        style.configure("TButton", padding=(9, 7))
        style.configure("Treeview", rowheight=29, background="#ffffff", fieldbackground="#ffffff",
                        font=("Microsoft YaHei UI", 10))
        style.configure("Treeview.Heading", font=("Microsoft YaHei UI", 10, "bold"), padding=7)
        style.map("Treeview", background=[("selected", "#d6e8fa")], foreground=[("selected", "#172b4d")])
        header = ttk.Frame(self, padding=(18, 14, 18, 8))
        header.grid(row=0, column=0, sticky="ew")
        ttk.Label(header, text="KR1304  商城表维护", font=("Microsoft YaHei UI", 18, "bold")).pack(side="left")
        ttk.Button(header, text="校验", command=self.validate_document).pack(side="right")
        toolbar = ttk.Frame(self, padding=(18, 2, 18, 10))
        toolbar.grid(row=1, column=0, sticky="ew")
        for text, command in (("打开商城目录", self.open_directory), ("XML 导入", self.import_xml),
                              ("保存", self.save), ("另存为", self.save_as)):
            ttk.Button(toolbar, text=text, command=command).pack(side="left", padx=(0, 7))
        ttk.Separator(toolbar, orient="vertical").pack(side="left", fill="y", padx=5)
        ttk.Button(toolbar, text="撤销", command=self.undo).pack(side="left", padx=5)
        ttk.Button(toolbar, text="重做", command=self.redo).pack(side="left")
        ttk.Button(toolbar, text="导入报告", command=self.import_report).pack(side="right")
        self.folder_label = ttk.Label(self, textvariable=self.folder, padding=(18, 0, 18, 10),
                                      anchor="w", foreground="#536174")
        self.folder_label.grid(row=2, column=0, sticky="ew")
        self.notebook = ttk.Notebook(self)
        self.notebook.grid(row=3, column=0, sticky="ew", padx=18)
        self.tabs = {}
        for kind, label in (("items", "商品"), ("offers", "报价"), ("sets", "礼包")):
            tab = ttk.Frame(self.notebook, height=1)
            self.notebook.add(tab, text=label)
            self.tabs[str(tab)] = kind
        self.notebook.bind("<<NotebookTabChanged>>", self.change_tab)
        filters = ttk.Frame(self, padding=(18, 12))
        filters.grid(row=4, column=0, sticky="ew")
        ttk.Label(filters, text="搜索").pack(side="left", padx=(0, 8))
        search = ttk.Entry(filters, textvariable=self.query, width=28)
        search.pack(side="left", fill="x", expand=True, padx=(0, 8))
        search.bind("<KeyRelease>", self.search_changed)
        for variable, values, width, callback in (
            (self.sale, ("全部状态", "已上架", "已下架"), 11, self.refresh),
            (self.main_filter, ["全部主分类"] + [f"{k} · {v}" for k, v in self.reference["mains"].items()], 18, self.main_changed),
            (self.sub_filter, ["全部子分类"], 18, self.refresh),
        ):
            combo = ttk.Combobox(filters, textvariable=variable, values=values, width=width, state="readonly")
            combo.pack(side="left", padx=(0, 8))
            combo.bind("<<ComboboxSelected>>", lambda _, callback=callback: callback())
            if variable is self.sub_filter:
                self.sub_combo = combo
        ttk.Button(filters, text="清除筛选", command=self.clear_filters).pack(side="left")
        grid = ttk.Frame(self)
        grid.grid(row=5, column=0, sticky="nsew", padx=18)
        grid.rowconfigure(0, weight=1)
        grid.columnconfigure(0, weight=1)
        self.tree = ttk.Treeview(grid, show="headings", selectmode="extended")
        self.tree.grid(row=0, column=0, sticky="nsew")
        vertical = ttk.Scrollbar(grid, orient="vertical", command=self.tree.yview)
        vertical.grid(row=0, column=1, sticky="ns")
        horizontal = ttk.Scrollbar(grid, orient="horizontal", command=self.tree.xview)
        horizontal.grid(row=1, column=0, sticky="ew")
        self.tree.configure(yscrollcommand=vertical.set, xscrollcommand=horizontal.set)
        self.tree.tag_configure("off", foreground="#7b8492")
        self.tree.bind("<Double-1>", lambda _: self.edit())
        self.tree.bind("<<TreeviewSelect>>", self.select_row)
        self.tree.bind("<Delete>", lambda _: self.delete())
        actions = ttk.Frame(self, padding=(18, 10))
        actions.grid(row=6, column=0, sticky="ew")
        for text, command in (("新增", self.add), ("编辑", self.edit), ("删除", self.delete),
                              ("上架", lambda: self.set_sale(True)), ("下架", lambda: self.set_sale(False)),
                              ("批量改价", self.batch_price), ("查看报价", self.show_offers)):
            ttk.Button(actions, text=text, command=command).pack(side="left", padx=(0, 6))
        self.detail_label = ttk.Label(self, textvariable=self.detail, anchor="w", padding=(18, 0, 18, 8),
                                      foreground="#536174", wraplength=1100)
        self.detail_label.grid(row=7, column=0, sticky="ew")
        footer = ttk.Frame(self, padding=(18, 7))
        footer.grid(row=8, column=0, sticky="ew")
        self.progress = ttk.Progressbar(footer, mode="indeterminate", length=130)
        self.progress.pack(side="right")
        ttk.Button(footer, text="下一页", command=lambda: self.turn_page(1)).pack(side="right", padx=(0, 10))
        ttk.Label(footer, textvariable=self.page_label, width=17, anchor="center").pack(side="right")
        ttk.Button(footer, text="上一页", command=lambda: self.turn_page(-1)).pack(side="right")
        ttk.Label(footer, textvariable=self.status, anchor="w", width=1).pack(side="left", fill="x", expand=True)

    def available(self):
        return not self.busy

    def resize_labels(self, event):
        if event.widget is self:
            for widget in (self.folder_label, self.detail_label):
                widget.configure(wraplength=max(400, event.width - 40))

    def discard(self):
        return self.available() and (not self.document.dirty or messagebox.askyesno(
            TITLE, "当前修改尚未保存，是否放弃？", parent=self))

    def load_directory(self, directory):
        try:
            document = ShopDocument.load(directory, self.reference)
        except (ValueError, OSError, KeyError) as error:
            messagebox.showerror("打开失败", str(error), parent=self)
            return
        self.document = document
        self.history.clear()
        self.future.clear()
        self.clear_filters()
        self.status.set("已打开商城表")

    def open_directory(self):
        if not self.discard():
            return
        directory = filedialog.askdirectory(parent=self, title="选择包含三张商城 CSV 的目录",
                                            initialdir=self.document.directory or server_root())
        if directory:
            self.load_directory(directory)

    def checkpoint(self):
        self.history.append(self.document.clone())
        del self.history[:-12]
        self.future.clear()
        self.document.dirty = True

    def undo(self):
        if self.available() and self.history:
            self.future.append(self.document)
            self.document = self.history.pop()
            self.document.dirty = True
            self.refresh()

    def redo(self):
        if self.available() and self.future:
            self.history.append(self.document)
            self.document = self.future.pop()
            self.document.dirty = True
            self.refresh()

    def change_tab(self, _=None):
        self.current_kind = self.tabs.get(self.notebook.select(), "items")
        self.page = 0
        self.refresh()

    def search_changed(self, _=None):
        if self.pending_search:
            self.after_cancel(self.pending_search)
        self.pending_search = self.after(200, self.refresh)

    def main_changed(self):
        self.sub_filter.set("全部子分类")
        main = self.main_filter.get().split(" · ")[0]
        self.sub_combo["values"] = ["全部子分类"] + [
            f"{key.split('/')[1]} · {value}" for key, value in self.reference["subs"].items()
            if key.split("/")[0] == main]
        self.refresh()

    def clear_filters(self):
        self.query.set("")
        self.sale.set("全部状态")
        self.main_filter.set("全部主分类")
        self.sub_filter.set("全部子分类")
        self.item_filter = None
        self.refresh()

    def refresh(self, reset_page=True):
        self.pending_search = None
        if reset_page:
            self.page = 0
        kind = self.current_kind
        columns = COLUMNS[kind]
        self.tree["columns"] = columns
        for key in columns:
            self.tree.heading(key, text=LABELS.get(key, "报价数" if key == "offers" else key))
            width = 250 if key == "name" else 145 if key in ("main_category", "sub_category") else 110
            self.tree.column(key, width=width, minwidth=70, stretch=key == "name", anchor="w")
        self.tree.delete(*self.tree.get_children())
        items = {row["item_id"]: row for row in self.document.tables["items"]}
        counts = {}
        for row in self.document.tables["offers"]:
            counts[row["item_id"]] = counts.get(row["item_id"], 0) + 1
        query = self.query.get().casefold().strip()
        main = self.main_filter.get().split(" · ")[0]
        sub = self.sub_filter.get().split(" · ")[0]
        self.visible = []
        for index, row in enumerate(self.document.tables[kind]):
            card = items.get(row.get("item_id"), row) if kind == "offers" else row
            if kind == "offers" and self.item_filter and row["item_id"] != self.item_filter:
                continue
            if query and query not in " ".join(str(v) for v in (*row.values(), card.get("name", ""))).casefold():
                continue
            if main != "全部主分类" and card.get("main_category") != main:
                continue
            if sub != "全部子分类" and card.get("sub_category") != sub:
                continue
            on_sale = flag(card.get("on_sale", "1"))
            if self.sale.get() != "全部状态" and on_sale != (self.sale.get() == "已上架"):
                continue
            self.visible.append((index, row, card, on_sale))
        pages = max(1, math.ceil(len(self.visible) / PAGE_SIZE))
        self.page = min(self.page, pages - 1)
        for index, row, card, on_sale in self.visible[self.page * PAGE_SIZE:(self.page + 1) * PAGE_SIZE]:
            values = []
            for key in columns:
                value = row.get(key, "")
                if key == "name":
                    value = card.get("name", "")
                elif key == "main_category":
                    value = f"{value} · {self.reference['mains'].get(value, '未映射')}"
                elif key == "sub_category":
                    value = f"{value} · {self.reference['subs'].get(row.get('main_category', '') + '/' + value, '未映射')}"
                elif key == "currency":
                    value = "Lucci" if value == "1" else "Cash" if value == "9" else value
                elif key == "duration" and value == str(PERMANENT):
                    value = "永久"
                elif key == "on_sale":
                    value = "已上架" if on_sale else "已下架"
                elif key == "offers":
                    value = counts.get(row["item_id"], 0)
                values.append(value)
            self.tree.insert("", "end", iid=str(index), values=values, tags=() if on_sale else ("off",))
        self.page_label.set(f"{self.page + 1} / {pages} · {len(self.visible)} 条")
        directory = str(self.document.directory) if self.document.directory else "未保存的生成结果"
        self.folder.set(directory + ("  ·  尚未保存" if self.document.dirty else ""))
        self.title(TITLE + (" *" if self.document.dirty else ""))
        counts_text = "  ·  ".join(f"{label} {len(self.document.tables[key])}" for key, label in
                                   (("items", "商品"), ("offers", "报价"), ("sets", "礼包")))
        self.detail.set(counts_text)

    def turn_page(self, delta):
        self.page = max(0, min(self.page + delta, max(0, math.ceil(len(self.visible) / PAGE_SIZE) - 1)))
        self.refresh(False)

    def selection(self):
        return [int(value) for value in self.tree.selection()]

    def select_row(self, _=None):
        selected = self.selection()
        if len(selected) != 1:
            return
        row = self.document.tables[self.current_kind][selected[0]]
        if self.current_kind == "sets":
            definition = self.reference["sets"].get(row["stock_set_id"], {})
            self.detail.set(f"内置礼包成员：{' '.join(map(str, definition.get('members', [])))}"
                            f"    附赠 Lucci：{definition.get('bonus', 0)}")
        else:
            definition = self.document.reference_item(row.get("item_id"))
            if definition:
                unit = "天" if definition["usage"] == "duration" else "个 / 次"
                self.detail.set(f"{definition['name']}    用途：{definition['usage']}"
                                f"    内置上限：{definition['maximum']} {unit}")

    def show_offers(self):
        if self.current_kind != "items" or len(self.selection()) != 1:
            return
        ident = self.document.tables["items"][self.selection()[0]]["item_id"]
        self.clear_filters()
        self.item_filter = ident
        self.notebook.select(1)
        self.change_tab()
        self.status.set(f"商品 {ident} 的报价")

    def mutate_rows(self, callback):
        if not self.available():
            return
        candidate = self.document.clone()
        callback(candidate)
        errors, _ = candidate.validate()
        if errors:
            raise ValueError("\n".join(errors[:5]))
        self.checkpoint()
        self.document.tables = candidate.tables
        self.refresh(False)

    def form_values(self, row):
        values = dict(row)
        if "currency" in values:
            values["currency"] = "1 · Lucci" if values["currency"] == "1" else "9 · Cash"
        return values

    def edit(self):
        if not self.available() or len(self.selection()) != 1:
            return
        index = self.selection()[0]
        kind = self.current_kind
        row = self.document.tables[kind][index]
        fields = {"items": ("item_id", "name", "main_category", "sub_category", "display_order", "on_sale"),
                  "offers": ("stock_id", "item_id", "currency", "duration", "show_price", "sell_price"),
                  "sets": ("stock_set_id", "name", "main_category", "sub_category", "currency",
                           "show_price", "sell_price", "on_sale", "bonus_lucci", "members")}[kind]
        values = self.form_values(row)
        if kind == "sets":
            definition = self.reference["sets"].get(row["stock_set_id"], {})
            values["bonus_lucci"] = str(definition.get("bonus", 0))
            values["members"] = " ".join(map(str, definition.get("members", [])))
        readonly = {"items": ("item_id",), "offers": ("stock_id", "item_id"),
                    "sets": ("stock_set_id", "bonus_lucci", "members")}[kind]

        def apply(values):
            if "currency" in values:
                values["currency"] = values["currency"].split(" · ")[0]
            self.mutate_rows(lambda document: document.tables[kind][index].update(values))

        dialog = FieldsDialog(self, "编辑" + {"items": "商品", "offers": "报价", "sets": "礼包"}[kind],
                              fields, values, readonly, {"currency": ("1 · Lucci", "9 · Cash")}, apply)
        self.wait_window(dialog)

    def add(self):
        if not self.available():
            return
        kind = self.current_kind
        if kind == "items":
            ident = simpledialog.askstring("新增商品", "服务端已支持的物品 ID", parent=self)
            if ident is None:
                return
            definition = self.document.reference_item(ident.strip())
            if not definition or not definition["maximum"] or definition["usage"] not in ("count", "duration", "life"):
                messagebox.showerror(TITLE, "此物品未被服务端支持或不能售卖。", parent=self)
                return
            ident = str(int(ident))
            if any(row["item_id"] == ident for row in self.document.tables["items"]):
                messagebox.showerror(TITLE, "商品已存在。", parent=self)
                return
            stock = self.next_stock()
            item = dict(item_id=ident, name=definition["name"], main_category=str(definition["main"]),
                        sub_category=str(definition["sub"]), display_order=str(definition["order"]),
                        duration_kind="day" if definition["usage"] == "duration" else definition["usage"],
                        money_type="lucci", offers="1", on_sale="0")
            offer = dict(stock_id=str(stock), item_id=ident, currency="1",
                         duration=str(PERMANENT if definition["usage"] == "duration" else 1),
                         quantity="0" if definition["usage"] == "duration" else "1",
                         show_price="1000", sell_price="1000")
            try:
                def apply(document):
                    document.tables["items"].append(item)
                    document.tables["offers"].append(offer)
                self.mutate_rows(apply)
                self.status.set(f"已新增下架商品 {ident}，初始 Lucci 报价 1000")
            except ValueError as error:
                messagebox.showerror(TITLE, str(error), parent=self)
            return
        if kind == "sets":
            ident = simpledialog.askstring("新增礼包", "服务端内置的礼包 ID", parent=self)
            if ident is None:
                return
            definition = self.reference["sets"].get(ident.strip())
            if not definition:
                messagebox.showerror(TITLE, "服务端没有此礼包。", parent=self)
                return
            currency = definition["currency"]
            problem = self.document.bundle_problem(ident.strip(), currency)
            if problem:
                messagebox.showerror(TITLE, problem, parent=self)
                return
            row = dict(stock_set_id=str(int(ident)), name=definition["name"],
                       main_category="800", sub_category="800", currency=str(currency),
                       show_price=str(min(MAX_VALUE, max(1, definition["show"]))),
                       sell_price=str(min(MAX_VALUE, max(1, definition["sell"]))),
                       bonus_lucci=str(definition["bonus"]),
                       members=" ".join(map(str, definition["members"])), on_sale="0")
            try:
                self.mutate_rows(lambda document: document.tables["sets"].append(row))
            except ValueError as error:
                messagebox.showerror(TITLE, str(error), parent=self)
            return
        ident = simpledialog.askstring("新增报价", "商城内的商品 ID", initialvalue=self.item_filter or "", parent=self)
        if ident is None:
            return
        if not any(row["item_id"] == ident.strip() for row in self.document.tables["items"]):
            messagebox.showerror(TITLE, "请先新增此商品。", parent=self)
            return
        row = dict(stock_id=str(self.next_stock()), item_id=ident.strip(), currency="1",
                   duration="1", show_price="1000", sell_price="1000", quantity="1")

        def apply(values):
            values["currency"] = values["currency"].split(" · ")[0]
            new = dict(row, **values)
            self.mutate_rows(lambda document: document.tables["offers"].append(new))

        dialog = FieldsDialog(self, "新增报价", ("stock_id", "item_id", "currency", "duration", "show_price", "sell_price"),
                              self.form_values(row), choices={"currency": ("1 · Lucci", "9 · Cash")}, on_change=apply)
        self.wait_window(dialog)

    def next_stock(self):
        used = {int(row["stock_id"]) for row in self.document.tables["offers"]}
        used.update(int(key) for key in self.reference["stocks"])
        for stock_id in range(1800000, 2000000):
            if stock_id not in used:
                return stock_id
        raise ValueError("可用报价 ID 已耗尽")

    def delete(self):
        indices = self.selection()
        if not self.available() or not indices:
            return
        kind = self.current_kind
        suffix = "及其全部报价" if kind == "items" else ""
        if not messagebox.askyesno(TITLE, f"删除选中的 {len(indices)} 条记录{suffix}？", parent=self):
            return
        try:
            def apply(document):
                rows = document.tables[kind]
                removed = {rows[index].get("item_id") for index in indices}
                document.tables[kind] = [row for index, row in enumerate(rows) if index not in indices]
                if kind == "items":
                    document.tables["offers"] = [row for row in document.tables["offers"] if row["item_id"] not in removed]
            self.mutate_rows(apply)
        except ValueError as error:
            messagebox.showerror(TITLE, str(error), parent=self)

    def set_sale(self, enabled):
        indices = self.selection()
        kind = self.current_kind
        if not self.available() or not indices or kind == "offers":
            return
        try:
            self.mutate_rows(lambda document: [document.tables[kind][index].update(on_sale="1" if enabled else "0")
                                               for index in indices])
        except ValueError as error:
            messagebox.showerror(TITLE, str(error), parent=self)

    def batch_price(self):
        indices = self.selection()
        if not self.available() or not indices:
            return
        value = simpledialog.askinteger("批量改价", "统一展示价格和实际价格", parent=self,
                                        minvalue=0, maxvalue=MAX_VALUE)
        if value is None:
            return
        kind = self.current_kind
        try:
            def apply(document):
                rows = [document.tables[kind][index] for index in indices]
                if kind == "items":
                    ids = {row["item_id"] for row in rows}
                    rows = [row for row in document.tables["offers"] if row["item_id"] in ids]
                for row in rows:
                    row.update(show_price=str(value), sell_price=str(value))
            self.mutate_rows(apply)
        except ValueError as error:
            messagebox.showerror(TITLE, str(error), parent=self)

    def validate_document(self):
        if not self.available():
            return
        errors, warnings = self.document.validate()
        TextWindow(self, "校验结果",
                   f"错误 {len(errors)} 项，提示 {len(warnings)} 项\n\n"
                   + ("\n".join(errors) or "CSV 结构及关联校验通过。")
                   + ("\n\n购买限制提示：\n" + "\n".join(warnings) if warnings else ""))

    def import_report(self):
        TextWindow(self, "最近导入报告",
                   json.dumps(self.document.metadata.get("import_report", {"状态": "尚未从 XML 导入"}),
                              ensure_ascii=False, indent=2))

    def import_xml(self):
        if not self.discard():
            return
        dialog = ImportDialog(self)
        self.wait_window(dialog)
        if not dialog.result:
            return
        path, options = dialog.result
        self.busy = True
        self.progress.start(12)
        self.status.set("正在解析 XML…")
        self.configure(cursor="watch")
        reference = self.reference

        def worker():
            try:
                self.jobs.put(("ok", ShopDocument.from_xml(path, reference, **options)))
            except Exception as error:
                self.jobs.put(("error", str(error)))
        threading.Thread(target=worker, daemon=True).start()

    def poll_jobs(self):
        try:
            state, result = self.jobs.get_nowait()
        except queue.Empty:
            pass
        else:
            self.busy = False
            self.progress.stop()
            self.configure(cursor="")
            if state == "ok":
                self.checkpoint()
                self.document = result
                self.clear_filters()
                self.status.set("XML 生成完成；尚未写入商城目录")
                self.import_report()
            else:
                self.status.set("XML 导入失败")
                messagebox.showerror("导入失败", result, parent=self)
        self.poll_id = self.after(100, self.poll_jobs)

    def save(self):
        if not self.available():
            return
        if not self.document.directory:
            return self.save_as()
        self.write_directory(self.document.directory)

    def save_as(self):
        if not self.available():
            return
        directory = filedialog.askdirectory(parent=self, title="选择输出商城表目录",
                                            initialdir=self.document.directory or server_root())
        if directory:
            self.write_directory(directory)

    def write_directory(self, directory):
        errors, warnings = self.document.validate()
        if errors:
            TextWindow(self, "无法保存", "\n".join(errors))
            return
        if not messagebox.askyesno(
                "保存商城表", "将写入三张商城 CSV 和元数据，原文件会备份。\n"
                "请在停服状态下替换正式商城表，保存后重启服务端生效。"
                + (f"\n另有 {len(warnings)} 项购买限制提示，可在“校验”中查看。" if warnings else ""),
                parent=self):
            return
        try:
            backup, _ = self.document.save(directory)
            self.history.clear()
            self.future.clear()
            self.refresh()
            self.status.set("已保存；重启服务端生效" + (f"  ·  备份：{backup.name}" if backup else ""))
        except (OSError, ValueError) as error:
            messagebox.showerror("保存失败", str(error), parent=self)

    def close(self):
        if self.busy:
            if not messagebox.askyesno(TITLE, "XML 导入尚未完成，是否退出？", parent=self):
                return
        elif not self.discard():
            return
        self.destroy()

    def destroy(self):
        for identifier in (getattr(self, "poll_id", None), self.pending_search):
            if identifier:
                self.after_cancel(identifier)
        super().destroy()


def main():
    parser = argparse.ArgumentParser(description=TITLE)
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--validate", type=Path)
    parser.add_argument("--import-xml", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.validate or args.import_xml:
        report = {}
        try:
            reference = load_reference()
            document = (ShopDocument.load(args.validate, reference) if args.validate
                        else ShopDocument.from_xml(args.import_xml, reference))
            errors, warnings = document.validate()
            report = dict(ok=not errors, counts={k: len(v) for k, v in document.tables.items()},
                          errors=errors, warnings=warnings, import_report=dict(document.report))
            if args.import_xml and args.output and not errors:
                document.save(args.output)
        except Exception as error:
            report = dict(ok=False, errors=[str(error)])
        if args.report:
            args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        elif sys.stdout:
            print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0 if report.get("ok") else 1
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    app = ShopEditor(directory=args.directory)
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
