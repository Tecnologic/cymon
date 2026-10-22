# SPDX-License-Identifier: AGPL-3.0-or-later
# PyInstaller spec of the one-file executable: pyinstaller packaging/cymon.spec
#
# The executable carries the standard DSDL, cymon and the web UI. It compiles
# the DSDL on its first start (into ~/.cache/cymon), like the Python package,
# so nunavut, pydsdl and pycyphal go in whole, with their templates.
from pathlib import Path

from PyInstaller.utils.hooks import collect_all, collect_submodules, copy_metadata

repo = Path(SPECPATH).parent

datas = [
    (str(repo / "src/cymon_app/web/static"), "cymon_app/web/static"),
    (str(repo / "lib/cymon-lib/dsdl/cymon"), "cymon_app/_dsdl/cymon"),
    (str(repo / "dsdl/public_regulated_data_types/uavcan"), "cymon_app/_dsdl/uavcan"),
    (str(repo / "dsdl/public_regulated_data_types/reg"), "cymon_app/_dsdl/reg"),
]
binaries = []
# The compiled DSDL is imported at run time and needs what its generated code imports.
hiddenimports = collect_submodules("cymon_app") + collect_submodules("uvicorn") + collect_submodules("can")
for package in ("nunavut", "pydsdl", "pycyphal", "numpy"):
    d, b, h = collect_all(package)
    datas += d
    binaries += b
    hiddenimports += h
# python-can finds the "canable" interface through the entry points of the cymon distribution.
datas += copy_metadata("cymon") + copy_metadata("python-can") + copy_metadata("pycyphal")

a = Analysis(
    [str(repo / "src/cymon_app/__main__.py")],
    pathex=[str(repo / "src")],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    excludes=["tkinter", "pytest"],
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    name="cymon",
    console=True,  # the log and Ctrl+C to quit
    upx=False,
)
