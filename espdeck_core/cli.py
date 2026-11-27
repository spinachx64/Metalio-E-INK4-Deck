"""espdeck command-line interface."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .adapters import eink4, mooncake
from .config import APPS_DIR, DeviceProfile, list_profiles, load_app, load_profile
from .idf import build_device, flash_device
from .ota import publish_remote_ota, serve_lan_ota

ADAPTERS = {
    "eink4": eink4,
    "mooncake": mooncake,
}


def cmd_list(_args: argparse.Namespace) -> int:
    profiles = list_profiles()
    if not profiles:
        print("No device profiles found in profiles/.")
        return 0
    for prof in profiles:
        print(f"- {prof.device} ({prof.adapter})  MAC={prof.board_mac or 'unset'}")
        print(f"    desc:     {prof.description}")
        print(f"    upstream: {prof.upstream}")
        print(f"    apps:     {', '.join(prof.apps) or '(none)'}")
        print(f"    official: keep [{', '.join(prof.keep_official_apps)}]")
    return 0


def _prepare_profile(device: str, clean: bool = False) -> DeviceProfile:
    prof = load_profile(device)
    adapter = ADAPTERS.get(prof.adapter)
    if not adapter:
        raise ValueError(f"Unsupported adapter '{prof.adapter}' for device '{device}'")
    apps = [load_app(prof.device if prof.adapter == "eink4" else "stopwatch", app_id) for app_id in prof.apps]
    adapter.prepare(prof, apps, clean=clean)
    return prof


def cmd_prepare(args: argparse.Namespace) -> int:
    _prepare_profile(args.device, clean=args.clean)
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    prof = _prepare_profile(args.device, clean=args.clean)
    return build_device(prof)


def cmd_flash(args: argparse.Namespace) -> int:
    prof = load_profile(args.device)
    if not prof.build_dir.exists():
        print(f"[espdeck] {prof.build_dir} does not exist; running build first...")
        rc = cmd_build(args)
        if rc != 0:
            return rc
    return flash_device(prof, port=args.port, skip_mac_check=args.force_mac)


def cmd_new_app(args: argparse.Namespace) -> int:
    platform = args.platform
    app_id = args.app_id
    slug = app_id.replace("-", "_")
    camel = "".join(part.capitalize() for part in slug.split("_"))

    app_dir = APPS_DIR / platform / app_id
    src_dir = app_dir / "src"
    if app_dir.exists():
        print(f"App already exists: {app_dir}", file=sys.stderr)
        return 1
    src_dir.mkdir(parents=True, exist_ok=True)

    if platform == "eink4":
        manifest = {
            "id": app_id,
            "platform": "eink4",
            "name": camel,
            "description": f"Custom E-INK4 app {camel}",
            "target_dir": slug,
            "header": f"{slug}_screen.h",
            "icon_symbol": f"{slug}_app_icon",
            "entry": f"{camel}Screen::Create",
            "source_root": "src",
            "sources": [f"{slug}_screen.cc"],
            "include_dirs": ["."],
        }
        (src_dir / f"{slug}_screen.h").write_text(
            f"#pragma once\n#include \"lvgl.h\"\n\nclass {camel}Screen {{\npublic:\n    static lv_obj_t* Create();\n}};\n",
            encoding="utf-8",
        )
        (src_dir / f"{slug}_screen.cc").write_text(
            f'#include "{slug}_screen.h"\n#include "screen_common.h"\n\n'
            f"lv_obj_t* {camel}Screen::Create() {{\n"
            f"    lv_obj_t* scr = lv_obj_create(nullptr);\n"
            f"    lv_obj_t* lbl = lv_label_create(scr);\n"
            f'    lv_label_set_text(lbl, "{camel}");\n'
            f"    lv_obj_center(lbl);\n"
            f"    return scr;\n"
            f"}}\n",
            encoding="utf-8",
        )
    else:
        manifest = {
            "id": app_id,
            "platform": platform,
            "name": camel,
            "description": f"Custom Mooncake app {camel}",
            "target_dir": f"app_{slug}",
            "header": f"app_{slug}/app_{slug}.h",
            "class_name": f"App{camel}",
            "source_root": "src",
        }
        (src_dir / f"app_{slug}.h").write_text(
            f"#pragma once\n#include <mooncake.h>\n\nclass App{camel} : public mooncake::AppAbility {{\n"
            f"public:\n    App{camel}() {{ setAppInfo().name = \"{camel}\"; }}\n"
            f"    void onOpen() override {{}}\n    void onRunning() override {{}}\n    void onClose() override {{}}\n}};\n",
            encoding="utf-8",
        )

    (app_dir / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Scaffolded new {platform} app at {app_dir}")
    print(f'Add "{app_id}" to profiles/{platform}.json -> "apps" to enable it.')
    return 0


def cmd_ota_serve(args: argparse.Namespace) -> int:
    prof = load_profile(args.device)
    if args.build:
        _prepare_profile(args.device)
        rc = build_device(prof)
        if rc != 0:
            return rc
    ota_cfg = prof.raw.get("ota", {})
    port = args.port or int(ota_cfg.get("lan_port", 8787))
    udp_port = int(ota_cfg.get("udp_port", 8788))
    return serve_lan_ota(prof, port=port, udp_port=udp_port, version=args.version, notes=args.notes)


def cmd_ota_publish(args: argparse.Namespace) -> int:
    prof = load_profile(args.device)
    if args.build:
        _prepare_profile(args.device)
        rc = build_device(prof)
        if rc != 0:
            return rc
    return publish_remote_ota(
        prof,
        remote_ssh=args.remote,
        base_url=args.base_url,
        version=args.version,
        notes=args.notes,
        dry_run=args.dry_run,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="espdeck", description="Multi-device ESP32/M5Stack/E-INK4 app composer")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="List configured devices and apps")
    p_list.set_defaults(func=cmd_list)

    p_prep = sub.add_parser("prepare", help="Incrementally compose .build/<device>/")
    p_prep.add_argument("device")
    p_prep.add_argument("--clean", action="store_true", help="Recreate .build/<device>/ from upstream")
    p_prep.set_defaults(func=cmd_prepare)

    p_build = sub.add_parser("build", help="Prepare and build firmware via ESP-IDF")
    p_build.add_argument("device")
    p_build.add_argument("--clean", action="store_true")
    p_build.set_defaults(func=cmd_build)

    p_flash = sub.add_parser("flash", help="MAC-verified safe app-flash to connected device")
    p_flash.add_argument("device")
    p_flash.add_argument("-p", "--port", default=None)
    p_flash.add_argument("--force-mac", action="store_true", help="Skip MAC safety check")
    p_flash.add_argument("--clean", action="store_true")
    p_flash.set_defaults(func=cmd_flash)

    p_new = sub.add_parser("new-app", help="Scaffold a new plugin app")
    p_new.add_argument("platform", choices=["eink4", "stopwatch"])
    p_new.add_argument("app_id")
    p_new.set_defaults(func=cmd_new_app)

    p_ota = sub.add_parser("ota", help="Dual-mode Wi-Fi OTA (LAN serve / remote HTTPS publish)")
    ota_sub = p_ota.add_subparsers(dest="ota_cmd", required=True)

    p_serve = ota_sub.add_parser("serve", help="Start LAN HTTP OTA server + UDP auto-discovery beacon")
    p_serve.add_argument("device")
    p_serve.add_argument("--port", type=int, default=0)
    p_serve.add_argument("--build", action="store_true", help="Build firmware before serving")
    p_serve.add_argument("--version", default=None)
    p_serve.add_argument("--notes", default="LAN dev OTA")
    p_serve.set_defaults(func=cmd_ota_serve)

    p_pub = ota_sub.add_parser("publish", help="Stage release and rsync to remote HTTPS server")
    p_pub.add_argument("device")
    p_pub.add_argument("--remote", default=None, help="Override ssh destination (e.g. user@host:/path/to/ota)")
    p_pub.add_argument("--base-url", default=None, help="Public HTTPS base URL (e.g. https://ota.example.com)")
    p_pub.add_argument("--build", action="store_true", help="Build firmware before publishing")
    p_pub.add_argument("--version", default=None)
    p_pub.add_argument("--notes", default="espdeck release")
    p_pub.add_argument("--dry-run", action="store_true")
    p_pub.set_defaults(func=cmd_ota_publish)

    args = parser.parse_args(argv)
    return args.func(args)
