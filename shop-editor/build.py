"""Build the standalone executable with the current compiled server catalog."""

import json
import shutil
import subprocess
import sys
from importlib.metadata import version
from pathlib import Path

from reference import build_reference


def main():
    root = Path(__file__).resolve().parent.parent
    output = root / "Artifacts/ShopEditor"
    working = root / "Artifacts/ShopEditorBuild"
    if version("pyinstaller") != "6.22.2":
        raise SystemExit("Install requirements-build.txt before packaging.")
    output.mkdir(parents=True, exist_ok=True)
    working.mkdir(parents=True, exist_ok=True)
    reference = build_reference(root)
    catalog = working / "catalog.json"
    catalog.write_text(json.dumps(reference, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    subprocess.run([
        sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed",
        "--name", "KR1304-ShopEditor", "--distpath", str(output), "--workpath", str(working / "work"),
        "--specpath", str(working), "--add-data", f"{catalog};.",
        str(Path(__file__).with_name("app.py")),
    ], check=True)
    licenses = output / "licenses"
    licenses.mkdir(exist_ok=True)
    for source, name in ((root / "LICENSE", "Project-LICENSE.txt"),
                         (Path(sys.base_prefix) / "LICENSE.txt", "Python-LICENSE.txt")):
        if source.is_file():
            shutil.copy2(source, licenses / name)
    import PyInstaller
    for source in Path(PyInstaller.__file__).parent.parent.glob("pyinstaller-*.dist-info/licenses/*"):
        if source.is_file():
            shutil.copy2(source, licenses / ("PyInstaller-" + source.name))
    for toolkit in ("tcl8.6", "tk8.6"):
        source = Path(sys.base_prefix) / "tcl" / toolkit / "license.terms"
        if source.is_file():
            shutil.copy2(source, licenses / (toolkit + "-license.terms"))
    (output / "build-info.json").write_text(json.dumps({
        "python": sys.version.split()[0], "pyinstaller": version("pyinstaller"),
        "server_catalog_sha256": reference["fingerprint"],
        "items": len(reference["items"]), "bundles": len(reference["sets"]),
    }, indent=2) + "\n", encoding="utf-8")
    print(output / "KR1304-ShopEditor.exe")


if __name__ == "__main__":
    main()
