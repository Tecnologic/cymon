# SPDX-License-Identifier: AGPL-3.0-or-later
"""DSDL compilation and type lookup.

pycyphal needs Python packages generated from the DSDL root namespaces before
``pycyphal.application`` can be imported. :func:`setup` compiles the standard
namespaces (uavcan, reg), cymon and any extra roots (for example
``unimoc/dsdl/unimoc``) into a cache directory keyed by their content, and puts
it on ``sys.path``. Call it before importing any other cymon module that uses
pycyphal.application.
"""

from __future__ import annotations

import hashlib
import importlib
import logging
import os
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import Any

_logger = logging.getLogger(__name__)

_PACKAGE_DIR = Path(__file__).resolve().parent
_REPO_DIR = _PACKAGE_DIR.parent.parent

_compiled_roots: list[Path] = []
_path_entry: str | None = None
_message_types: list[str] = []


def default_roots() -> list[Path]:
    """The namespaces cymon always needs: uavcan, reg and cymon."""
    bundled = _PACKAGE_DIR / "_dsdl"
    if bundled.is_dir():
        return [bundled / "uavcan", bundled / "reg", bundled / "cymon"]
    public = _REPO_DIR / "dsdl" / "public_regulated_data_types"
    roots = [public / "uavcan", public / "reg", _REPO_DIR / "lib" / "cymon-lib" / "dsdl" / "cymon"]
    missing = [r for r in roots if not r.is_dir()]
    if missing:
        raise FileNotFoundError(
            "DSDL namespaces not found: " + ", ".join(map(str, missing)) + ". Run: git submodule update --init"
        )
    return roots


def cache_dir() -> Path:
    base = os.environ.get("CYMON_CACHE_DIR") or os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache"
    return Path(base) / "cymon" / "dsdl"


def _fingerprint(roots: Iterable[Path]) -> str:
    import pycyphal

    digest = hashlib.sha256(pycyphal.__version__.encode())
    for root in roots:
        digest.update(root.name.encode())
        for path in sorted(root.rglob("*.dsdl")):
            digest.update(str(path.relative_to(root)).encode())
            digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def _resolve(extra_roots: Iterable[str | Path]) -> list[Path]:
    roots = default_roots()
    for extra in extra_roots:
        root = Path(extra).expanduser().resolve()
        if not root.is_dir():
            raise ValueError(f"DSDL root namespace {extra} is not a directory")
        if not any(root.rglob("*.dsdl")):
            raise ValueError(f"DSDL root namespace {extra} contains no .dsdl file")
        roots.append(root)
    names = [r.name for r in roots]
    duplicates = {n for n in names if names.count(n) > 1}
    if duplicates:
        raise ValueError(f"DSDL root namespace given twice: {', '.join(sorted(duplicates))}")
    return roots


def _compile(roots: list[Path], output_dir: Path | None) -> Path:
    import pycyphal.dsdl

    out = (output_dir or cache_dir()) / _fingerprint(roots)
    if not (out / ".complete").exists():
        _logger.info("Compiling DSDL %s into %s (first time only)", [r.name for r in roots], out)
        pycyphal.dsdl.compile_all(roots, out)
        (out / ".complete").touch()
    return out


def compile_roots(extra_roots: Iterable[str | Path] = (), output_dir: Path | None = None) -> Path:
    """Compiles the DSDL (once per content) without making it importable. Returns the package directory."""
    return _compile(_resolve(extra_roots), output_dir)


def setup(extra_roots: Iterable[str | Path] = (), output_dir: Path | None = None) -> Path:
    """Compiles the DSDL (once per content) and makes it importable. Returns the package directory."""
    global _path_entry
    roots = _resolve(extra_roots)
    out = _compile(roots, output_dir)
    if _path_entry in sys.path:
        sys.path.remove(_path_entry)
    sys.path.insert(0, str(out))
    _path_entry = str(out)
    # Extra namespaces may have changed on disk: import them again from the new directory.
    defaults = {r.name for r in default_roots()}
    for name in {r.name for r in _compiled_roots + roots} - defaults:
        for module in [m for m in sys.modules if m == name or m.startswith(name + ".")]:
            del sys.modules[module]
    importlib.invalidate_caches()
    _compiled_roots[:] = roots
    _message_types.clear()
    return out


def namespaces() -> list[str]:
    """Root namespaces compiled by :func:`setup`."""
    return [r.name for r in _compiled_roots]


def message_types() -> list[str]:
    """Full names of all message types (no services) in the namespaces of :func:`setup`."""
    import pydsdl

    if not _message_types:
        for root in _compiled_roots:
            for t in pydsdl.read_namespace(root, [r for r in _compiled_roots if r != root]):
                if not isinstance(t, pydsdl.ServiceType):
                    _message_types.append(f"{t.full_name}.{t.version.major}.{t.version.minor}")
        _message_types.sort()
    return list(_message_types)


def find_type(full_name: str) -> type[Any]:
    """Returns the generated class for a name like ``unimoc.drive.Telemetry.0.1``.

    Without a version, the newest version is used (``uavcan.node.Heartbeat``).
    """
    parts = full_name.strip().split(".")
    if len(parts) >= 3 and parts[-1].isdigit() and parts[-2].isdigit():
        module_name, short, major, minor = ".".join(parts[:-3]), parts[-3], parts[-2], parts[-1]
        try:
            module = importlib.import_module(module_name)
        except ImportError:
            raise LookupError(f"Unknown DSDL namespace: {module_name}") from None
        try:
            return getattr(module, f"{short}_{major}_{minor}")
        except AttributeError:
            raise LookupError(f"Unknown DSDL type: {full_name}") from None
    module_name, short = ".".join(parts[:-1]), parts[-1]
    try:
        module = importlib.import_module(module_name)
    except ImportError:
        raise LookupError(f"Unknown DSDL namespace: {module_name}") from None
    versions = [
        name
        for name in dir(module)
        if name.startswith(short + "_") and name[len(short) + 1 :].replace("_", "").isdigit()
    ]
    if not versions:
        raise LookupError(f"Unknown DSDL type: {full_name}")

    def version_key(name: str) -> tuple[int, ...]:
        return tuple(int(v) for v in name[len(short) + 1 :].split("_"))

    return getattr(module, max(versions, key=version_key))


def type_name(dtype: type[Any]) -> str:
    """``unimoc.drive.Telemetry.0.1`` for a generated class."""
    import pycyphal.dsdl

    model = pycyphal.dsdl.get_model(dtype)
    return f"{model.full_name}.{model.version.major}.{model.version.minor}"
