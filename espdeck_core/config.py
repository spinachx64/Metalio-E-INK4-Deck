"""Profile and app manifest loader + incremental file sync helpers."""

from __future__ import annotations

import filecmp
import json
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
PROFILES_DIR = ROOT / "profiles"
APPS_DIR = ROOT / "apps"
BUILD_ROOT = ROOT / ".build"


@dataclass
class AppManifest:
    id: str
    platform: str
    name: str
    target_dir: str
    manifest_dir: Path
    source_root: Path
    raw: dict[str, Any] = field(default_factory=dict)

    @property
    def header(self) -> str:
        return self.raw.get("header", "")

    @property
    def icon_symbol(self) -> str:
        return self.raw.get("icon_symbol", "")

    @property
    def entry(self) -> str:
        return self.raw.get("entry", "")

    @property
    def class_name(self) -> str:
        return self.raw.get("class_name", "")

    @property
    def builtin_in_upstream(self) -> bool:
        return bool(self.raw.get("builtin_in_upstream", False))

    @property
    def sources(self) -> list[str]:
        return list(self.raw.get("sources", []))

    @property
    def include_dirs(self) -> list[str]:
        return list(self.raw.get("include_dirs", ["."]))

    @property
    def extra_sync(self) -> list[dict[str, Any]]:
        return list(self.raw.get("extra_sync", []))


@dataclass
class DeviceProfile:
    device: str
    adapter: str
    description: str
    board_mac: str
    baud: int
    flash_mode: str
    idf_env: Path
    upstream: Path
    keep_official_apps: list[str]
    apps: list[str]
    raw: dict[str, Any] = field(default_factory=dict)

    @property
    def build_dir(self) -> Path:
        return BUILD_ROOT / self.device


def resolve_path(base: Path, raw: str) -> Path:
    p = Path(raw).expanduser()
    return p.resolve() if p.is_absolute() else (base / p).resolve()


def _deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    out = dict(base)
    for k, v in override.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def list_profiles() -> list[DeviceProfile]:
    out: list[DeviceProfile] = []
    if not PROFILES_DIR.is_dir():
        return out
    for p in sorted(PROFILES_DIR.glob("*.json")):
        if p.name.endswith(".local.json"):
            continue
        out.append(load_profile(p.stem))
    return out


def load_profile(device: str) -> DeviceProfile:
    p = PROFILES_DIR / f"{device}.json"
    if not p.is_file():
        raise FileNotFoundError(f"Device profile not found: {p}")
    data = json.loads(p.read_text(encoding="utf-8"))
    local_p = PROFILES_DIR / f"{device}.local.json"
    if local_p.is_file():
        data = _deep_merge(data, json.loads(local_p.read_text(encoding="utf-8")))

    upstream = resolve_path(ROOT, data["upstream"])
    if not upstream.exists() and data.get("fallback_upstream"):
        upstream = resolve_path(ROOT, data["fallback_upstream"])

    return DeviceProfile(
        device=data["device"],
        adapter=data["adapter"],
        description=data.get("description", ""),
        board_mac=data.get("board_mac", "").lower(),
        baud=int(data.get("baud", 921600)),
        flash_mode=data.get("flash_mode", "app-flash"),
        idf_env=resolve_path(ROOT, data.get("idf_env", "../ink4-codex-monitor/.state/idf-env.sh")),
        upstream=upstream,
        keep_official_apps=list(data.get("keep_official_apps", [])),
        apps=list(data.get("apps", [])),
        raw=data,
    )


def load_app(platform: str, app_id: str) -> AppManifest:
    # Support both apps/<platform>/<app_id>/manifest.json and direct relative path
    cand = APPS_DIR / platform / app_id / "manifest.json"
    if not cand.is_file():
        alt = resolve_path(ROOT, app_id) / "manifest.json"
        if alt.is_file():
            cand = alt
        else:
            raise FileNotFoundError(f"App manifest not found for {platform}/{app_id} (checked {cand})")
    data = json.loads(cand.read_text(encoding="utf-8"))
    mdir = cand.parent
    src_raw = data.get("source_root", "src")
    src_root = resolve_path(mdir, src_raw)
    return AppManifest(
        id=data["id"],
        platform=data.get("platform", platform),
        name=data.get("name", app_id),
        target_dir=data.get("target_dir", app_id.replace("-", "_")),
        manifest_dir=mdir,
        source_root=src_root,
        raw=data,
    )


def copy_if_changed(src: Path, dst: Path) -> bool:
    """Copy src -> dst only if dst is missing or content differs (preserves Ninja cache!)."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_file() and filecmp.cmp(src, dst, shallow=False):
        return False
    shutil.copy2(src, dst)
    return True


def sync_tree_incremental(src_dir: Path, dst_dir: Path) -> int:
    """Recursively sync files from src_dir to dst_dir, skipping unchanged files."""
    changed = 0
    if not src_dir.is_dir():
        raise FileNotFoundError(f"Missing source directory: {src_dir}")
    for path in sorted(src_dir.rglob("*")):
        if path.is_dir():
            continue
        if any(part in {".git", "__pycache__", ".DS_Store", "build"} for part in path.parts):
            continue
        rel = path.relative_to(src_dir)
        if copy_if_changed(path, dst_dir / rel):
            changed += 1
    return changed


def write_if_changed(path: Path, content: str) -> bool:
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True
