"""M5Stack Mooncake adapter (StopWatch, CoreS3, Dial, etc.): incremental plugin sync + official-app filter."""

from __future__ import annotations

import re
import shutil
from pathlib import Path

from ..config import AppManifest, DeviceProfile, copy_if_changed, resolve_path, sync_tree_incremental, write_if_changed

OFFICIAL_MOONCAKE_APPS = [
    "AppFluidBox",
    "AppAlarmClock",
    "AppWatchFace",
    "AppStopWatch",
    "AppBadge",
    "AppImu",
    "AppFft",
    "AppLuckyWheel",
    "AppSetup",
]

APPS_H_BEGIN = "// --- ESPDECK_APPS_H_BEGIN ---"
APPS_H_END = "// --- ESPDECK_APPS_H_END ---"
MAIN_BEGIN = "// --- ESPDECK_INSTALL_BEGIN ---"
MAIN_END = "// --- ESPDECK_INSTALL_END ---"


def _sync_app_sources(output: Path, app: AppManifest) -> int:
    dst = output / "main" / "apps" / app.target_dir
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


def _update_apps_h(output: Path, apps: list[AppManifest]) -> bool:
    apps_h = output / "main" / "apps" / "apps.h"
    text = apps_h.read_text(encoding="utf-8")
    text = re.sub(
        rf"\n?{re.escape(APPS_H_BEGIN)}[\s\S]*?{re.escape(APPS_H_END)}\n?",
        "\n",
        text,
    )
    lines = [APPS_H_BEGIN]
    for app in apps:
        if app.header:
            lines.append(f'#include "{app.header}"')
    lines.append(APPS_H_END)
    new_text = text.rstrip() + "\n" + "\n".join(lines) + "\n"
    return write_if_changed(apps_h, new_text)


def _update_main_cpp(output: Path, profile: DeviceProfile, apps: list[AppManifest]) -> bool:
    main_cpp = output / "main" / "main.cpp"
    text = main_cpp.read_text(encoding="utf-8")

    install_lines = [
        f"    {MAIN_BEGIN}",
        "    GetMooncake().installApp(std::make_unique<AppLauncher>());",
    ]
    for app in apps:
        install_lines.append(f"    GetMooncake().installApp(std::make_unique<{app.class_name}>());")
    for off in profile.keep_official_apps:
        install_lines.append(f"    GetMooncake().installApp(std::make_unique<{off}>());")
    install_lines.append(f"    {MAIN_END}")
    block = "\n".join(install_lines)

    if MAIN_BEGIN in text and MAIN_END in text:
        new_text = re.sub(
            rf"[ \t]*{re.escape(MAIN_BEGIN)}[\s\S]*?{re.escape(MAIN_END)}",
            block,
            text,
            count=1,
        )
    else:
        pat = re.compile(
            r"[ \t]*GetMooncake\(\)\.installApp\(std::make_unique<AppLauncher>\(\)\);[\s\S]*?"
            r"(?=\n[ \t]*//\s*Main loop|\n[ \t]*while\s*\()",
            re.MULTILINE,
        )
        if not pat.search(text):
            raise ValueError(f"Could not locate Mooncake installApp block in {main_cpp}")
        new_text = pat.sub(block + "\n", text, count=1)

    return write_if_changed(main_cpp, new_text)


def prepare(profile: DeviceProfile, apps: list[AppManifest], clean: bool = False) -> Path:
    out = profile.build_dir
    if clean and out.exists():
        shutil.rmtree(out)

    if not out.exists():
        print(f"[mooncake] Cloning upstream {profile.upstream} -> {out} ...")
        shutil.copytree(
            profile.upstream,
            out,
            ignore=shutil.ignore_patterns(".git", ".pio", "build", "__pycache__", ".DS_Store"),
        )

    for app in apps:
        c = _sync_app_sources(out, app)
        print(f"[mooncake] Synced app '{app.id}' ({c} file(s) updated)")

    h_upd = _update_apps_h(out, apps)
    m_upd = _update_main_cpp(out, profile, apps)
    hidden = [k for k in OFFICIAL_MOONCAKE_APPS if k not in profile.keep_official_apps]
    print(
        f"[mooncake] Ready: {out} | apps={[a.id for a in apps]} | "
        f"official_kept={profile.keep_official_apps} | official_hidden={hidden} | "
        f"apps_h_upd={h_upd} main_upd={m_upd}"
    )
    return out
