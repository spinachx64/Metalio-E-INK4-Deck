"""Metalio E-INK4 adapter: incremental plugin sync + official-app filter + CMake/home_screen injection."""

from __future__ import annotations

import re
import shutil
import subprocess
from datetime import datetime
from pathlib import Path

from ..config import ROOT, AppManifest, DeviceProfile, copy_if_changed, resolve_path, sync_tree_incremental, write_if_changed
from ..ota import detect_lan_ip

OFFICIAL_APPS: dict[str, tuple[str, str, str]] = {
    "task":      ('"A:ic_app_home_task.spng"',      "Home_AppNameTask",      "TaskScreen::Create"),
    "assistant": ('"A:ic_app_home_teacher.spng"',   "Home_AppNameAssistant", "AssistantScreen::Create"),
    "book":      ('"A:ic_app_home_book.spng"',      "Home_AppNameBook",      "BookScreen::Create"),
    "wallpaper": ('"A:ic_app_home_wallpaper.spng"', "Home_AppNameWallpaper", "WallpaperScreen::Create"),
    "cloud":     ('"A:ic_app_home_cloud.spng"',     "Home_AppNameCloud",     "CloudScreen::Create"),
    "setting":   ('"A:ic_app_home_setting.spng"',   "Home_AppNameSettings",  "SettingsScreen::Create"),
}

CMAKE_BEGIN = "# --- ESPDECK_APPS_BEGIN ---"
CMAKE_END = "# --- ESPDECK_APPS_END ---"
HDR_BEGIN = "// --- ESPDECK_HEADERS_BEGIN ---"
HDR_END = "// --- ESPDECK_HEADERS_END ---"
APPS_BEGIN = "// --- ESPDECK_KAPPS_BEGIN ---"
APPS_END = "// --- ESPDECK_KAPPS_END ---"


def _sync_app_sources(output: Path, app: AppManifest) -> int:
    dst = output / "main" / app.target_dir
    changed = sync_tree_incremental(app.source_root, dst)
    for extra in app.extra_sync:
        extra_src = resolve_path(app.manifest_dir, extra["from"])
        extra_dst = dst / extra["to"]
        files = extra.get("files")
        if files:
            for fname in files:
                if copy_if_changed(extra_src / fname, extra_dst / fname):
                    changed += 1
        else:
            changed += sync_tree_incremental(extra_src, extra_dst)
    return changed


def _update_cmake(output: Path, apps: list[AppManifest]) -> bool:
    cmake = output / "main" / "CMakeLists.txt"
    text = cmake.read_text(encoding="utf-8")

    # Strip previous espdeck managed block if present
    text = re.sub(
        rf"{re.escape(CMAKE_BEGIN)}[\s\S]*?{re.escape(CMAKE_END)}\n?",
        "",
        text,
    )

    lines = [CMAKE_BEGIN]
    for app in apps:
        if app.builtin_in_upstream:
            continue
        if app.sources:
            srcs = " ".join(f'"{app.target_dir}/{s}"' for s in app.sources)
            lines.append(f"list(APPEND SOURCES {srcs})")
        for idx, pat in enumerate(app.raw.get("source_globs", [])):
            var = f"ESPDECK_GLOB_{app.id.upper().replace('-', '_')}_{idx}"
            lines.append(f'file(GLOB {var} "{app.target_dir}/{pat}")')
            lines.append(f"list(APPEND SOURCES ${{{var}}})")
        incs = " ".join(
            f'"{app.target_dir}"' if d == "." else f'"{app.target_dir}/{d}"'
            for d in app.include_dirs
        )
        if incs:
            lines.append(f"list(APPEND INCLUDE_DIRS {incs})")
    lines.append(CMAKE_END)
    block = "\n".join(lines) + "\n"

    marker = "idf_component_register(SRCS ${SOURCES}"
    if marker not in text:
        raise ValueError(f"Unsupported CMakeLists.txt in {cmake}")
    new_text = text.replace(marker, block + marker, 1)
    return write_if_changed(cmake, new_text)


def _update_home_screen(output: Path, profile: DeviceProfile, apps: list[AppManifest]) -> bool:
    home_cc = output / "main" / "display" / "screen" / "home_screen" / "home_screen.cc"
    text = home_cc.read_text(encoding="utf-8")

    # 1. Strip any previous ESPDECK header block and duplicate plugin includes already baked into upstream
    text = re.sub(
        rf"{re.escape(HDR_BEGIN)}[\s\S]*?{re.escape(HDR_END)}\n?",
        "",
        text,
    )
    for app in apps:
        if app.header:
            text = re.sub(rf'^#include "{re.escape(app.header)}"\n', "", text, flags=re.MULTILINE)
        if app.icon_symbol:
            text = re.sub(rf'^LV_IMAGE_DECLARE\({re.escape(app.icon_symbol)}\);\n', "", text, flags=re.MULTILINE)

    hdr_lines = [HDR_BEGIN]
    for app in apps:
        if app.header:
            hdr_lines.append(f'#include "{app.header}"')
        if app.icon_symbol:
            hdr_lines.append(f"LV_IMAGE_DECLARE({app.icon_symbol});")
    hdr_lines.append(HDR_END)
    text = "\n".join(hdr_lines) + "\n" + text

    # 2. Build the exact filtered kApps[] and kSlashApps[] arrays
    name_fns: list[str] = []
    entries: list[str] = []
    for app in apps:
        fn_name = f"Home_AppName_{app.id.replace('-', '_')}"
        name_fns.append(f'    const char* {fn_name}() {{ return "{app.name}"; }}')
        entries.append(f"        {{&{app.icon_symbol}, {fn_name}, {app.entry}}},")

    for off_key in profile.keep_official_apps:
        if off_key not in OFFICIAL_APPS:
            raise ValueError(f"Unknown E-INK4 official app '{off_key}'. Valid: {list(OFFICIAL_APPS)}")
        icon, fn, entry = OFFICIAL_APPS[off_key]
        entries.append(f"        {{{icon}, {fn}, {entry}}},")

    entries_str = "\n".join(entries)
    fn_block = "\n".join(name_fns)
    apps_block = (
        f"{APPS_BEGIN}\n"
        f"{fn_block}\n"
        f"    extern const AppEntry kApps[] = {{\n"
        f"{entries_str}\n"
        f"    }};\n\n"
        f"    extern const AppEntry kSlashApps[] = {{\n"
        f"{entries_str}\n"
        f"    }};\n"
        f"{APPS_END}"
    )

    if APPS_BEGIN in text and APPS_END in text:
        new_text = re.sub(
            rf"{re.escape(APPS_BEGIN)}[\s\S]*?{re.escape(APPS_END)}",
            apps_block,
            text,
            count=1,
        )
    else:
        # First-time replace from first custom Home_AppName / extern const AppEntry kApps[] through end of kSlashApps[]
        pat = re.compile(
            r"(?:^\s*const char\*\s+Home_AppName\w+\(\)\s*\{[^}]*\}\s*\n)*"
            r"\s*extern const AppEntry kApps\[\]\s*=\s*\{[\s\S]*?\};\s*"
            r"(?://[^\n]*\n\s*)?"
            r"extern const AppEntry kSlashApps\[\]\s*=\s*\{[\s\S]*?\};",
            re.MULTILINE,
        )
        if not pat.search(text):
            raise ValueError(f"Could not locate kApps/kSlashApps block in {home_cc}")
        new_text = pat.sub(apps_block, text, count=1)

    changed = write_if_changed(home_cc, new_text)

    # Ensure home_screen_priv.h accepts const void* icon_path
    priv_h = output / "main" / "display" / "screen" / "home_screen" / "home_screen_priv.h"
    if priv_h.is_file():
        priv_text = priv_h.read_text(encoding="utf-8")
        if "const char* icon_path;" in priv_text:
            write_if_changed(priv_h, priv_text.replace("const char* icon_path;", "const void* icon_path;"))

    return changed


def _write_ota_config_if_needed(output: Path, profile: DeviceProfile, apps: list[AppManifest]) -> bool:
    if not any(a.id == "espdeck-ota" for a in apps):
        return False
    ota_cfg = profile.raw.get("ota", {})
    lan_port = int(ota_cfg.get("lan_port", 8787))
    udp_port = int(ota_cfg.get("udp_port", 8788))
    remote_url = str(ota_cfg.get("remote_url", "")).rstrip("/")
    lan_url = f"http://{detect_lan_ip()}:{lan_port}"
    ver = datetime.now().strftime("%Y.%m.%d")

    cfg_h = output / "main" / "espdeck_ota" / "espdeck_ota_config.h"
    content = (
        "#pragma once\n"
        f'#define ESPDECK_BUILD_VERSION "{ver}"\n'
        f'#define ESPDECK_OTA_LAN_URL "{lan_url}"\n'
        f'#define ESPDECK_OTA_REMOTE_URL "{remote_url}"\n'
        f"#define ESPDECK_OTA_UDP_PORT {udp_port}\n"
    )
    return write_if_changed(cfg_h, content)


def _apply_base_patches(output: Path, profile: DeviceProfile) -> None:
    stamp = output / ".espdeck_base_applied"
    if stamp.is_file():
        return

    for raw_patch in profile.raw.get("base_patches", []):
        patch_path = resolve_path(ROOT, raw_patch)
        if not patch_path.is_file():
            raise FileNotFoundError(f"Base patch not found: {patch_path}")
        res = subprocess.run(
            ["patch", "-p1", "--batch", "--forward", "-i", str(patch_path)],
            cwd=str(output),
            capture_output=True,
            text=True,
        )
        if res.returncode != 0:
            raise RuntimeError(f"Failed applying base patch {patch_path}:\n{res.stdout}\n{res.stderr}")
        print(f"[eink4] Applied base patch: {raw_patch}")

    frag_raw = profile.raw.get("sdkconfig_fragment")
    if frag_raw:
        frag_path = resolve_path(ROOT, frag_raw)
        sdk_path = output / "sdkconfig"
        if frag_path.is_file() and sdk_path.is_file():
            frag_lines = [
                ln.strip() for ln in frag_path.read_text(encoding="utf-8").splitlines()
                if ln.strip() and not ln.strip().startswith("#")
            ]
            override_keys = {ln.split("=", 1)[0] for ln in frag_lines if "=" in ln}
            kept = []
            for ln in sdk_path.read_text(encoding="utf-8").splitlines():
                key = ln.split("=", 1)[0] if ln.startswith("CONFIG_") else (
                    ln[2:].split(" ", 1)[0] if ln.startswith("# CONFIG_") else ""
                )
                if key in override_keys:
                    continue
                kept.append(ln)
            kept.append("")
            kept.append("# --- ESPDECK SDKCONFIG OVERRIDES ---")
            kept.extend(frag_lines)
            sdk_path.write_text("\n".join(kept) + "\n", encoding="utf-8")
            print(f"[eink4] Merged sdkconfig fragment: {frag_raw} ({len(frag_lines)} keys)")

    stamp.write_text("ok\n", encoding="utf-8")


def prepare(profile: DeviceProfile, apps: list[AppManifest], clean: bool = False) -> Path:
    out = profile.build_dir
    if clean and out.exists():
        shutil.rmtree(out)

    if not out.exists():
        print(f"[eink4] Cloning upstream {profile.upstream} -> {out} ...")
        shutil.copytree(
            profile.upstream,
            out,
            ignore=shutil.ignore_patterns(".git", ".pio", "build", "__pycache__", ".DS_Store"),
        )

    _apply_base_patches(out, profile)

    total_changed = 0
    for app in apps:
        c = _sync_app_sources(out, app)
        total_changed += c
        print(f"[eink4] Synced app '{app.id}' ({c} file(s) updated)")

    _write_ota_config_if_needed(out, profile, apps)
    cmake_changed = _update_cmake(out, apps)
    home_changed = _update_home_screen(out, profile, apps)
    hidden = [k for k in OFFICIAL_APPS if k not in profile.keep_official_apps]
    print(
        f"[eink4] Ready: {out} | apps={[a.id for a in apps]} | "
        f"official_kept={profile.keep_official_apps} | official_hidden={hidden} | "
        f"cmake_upd={cmake_changed} home_upd={home_changed}"
    )
    return out
