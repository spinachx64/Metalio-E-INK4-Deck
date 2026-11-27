"""Dual-mode Wi-Fi OTA server (LAN HTTP + UDP discovery beacon) and remote HTTPS publisher."""

from __future__ import annotations

import hashlib
import http.server
import json
import shutil
import socket
import socketserver
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

from .config import ROOT, DeviceProfile

OTA_STATE_DIR = ROOT / ".state" / "ota"


def detect_lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def find_firmware_bin(profile: DeviceProfile) -> Path:
    build_out = profile.build_dir / "build"
    if not build_out.is_dir():
        raise FileNotFoundError(f"Build output directory missing: {build_out}. Run `./espdeck build {profile.device}` first.")
    # Ignore bootloader / partition / ota_data / srmodels / resources
    skip = {"bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "srmodels.bin", "resources.bin"}
    cands = [
        p for p in build_out.glob("*.bin")
        if p.name not in skip and p.stat().st_size > 200_000
    ]
    if not cands:
        raise FileNotFoundError(f"No app firmware .bin found in {build_out}")
    cands.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return cands[0]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def stage_release(
    profile: DeviceProfile,
    base_url: str,
    version: str | None = None,
    notes: str = "espdeck OTA release",
) -> tuple[Path, dict]:
    bin_src = find_firmware_bin(profile)
    stage_dir = OTA_STATE_DIR / profile.device
    stage_dir.mkdir(parents=True, exist_ok=True)

    fw_dst = stage_dir / "firmware.bin"
    shutil.copy2(bin_src, fw_dst)

    digest = sha256_file(fw_dst)
    size = fw_dst.stat().st_size
    ver = version or datetime.now().strftime("%Y.%m.%d-%H%M%S")
    base = base_url.rstrip("/")

    manifest = {
        "device": profile.device,
        "version": ver,
        "sha256": digest,
        "size": size,
        "url": f"{base}/{profile.device}/firmware.bin" if base else "",
        "apps": profile.apps,
        "notes": notes,
        "built_from": bin_src.name,
    }
    (stage_dir / "latest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return stage_dir, manifest


def serve_lan_ota(
    profile: DeviceProfile,
    port: int = 8787,
    udp_port: int = 8788,
    version: str | None = None,
    notes: str = "LAN dev OTA",
) -> int:
    lan_ip = detect_lan_ip()
    base_url = f"http://{lan_ip}:{port}"
    stage_dir, manifest = stage_release(profile, base_url=base_url, version=version, notes=notes)

    stop_evt = threading.Event()

    def beacon_loop() -> None:
        payload = f"ESPDECK_OTA|{profile.device}|{base_url}".encode("utf-8")
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            while not stop_evt.is_set():
                try:
                    sock.sendto(payload, ("255.255.255.255", udp_port))
                except OSError:
                    pass
                stop_evt.wait(1.0)
        finally:
            sock.close()

    t = threading.Thread(target=beacon_loop, daemon=True)
    t.start()

    class QuietHandler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(OTA_STATE_DIR), **kwargs)

    class ReusableTCPServer(socketserver.TCPServer):
        allow_reuse_address = True

    print(f"[ota] Serving '{profile.device}' version={manifest['version']} ({manifest['size'] / 1048576:.2f} MB)")
    print(f"[ota] Manifest URL: {base_url}/{profile.device}/latest.json")
    print(f"[ota] Firmware URL: {manifest['url']}")
    print(f"[ota] SHA-256:      {manifest['sha256']}")
    print(f"[ota] UDP Beacon:   broadcasting on port {udp_port} -> open OTA app on {profile.device}!")
    print("[ota] Press Ctrl+C to stop.")

    try:
        with ReusableTCPServer(("0.0.0.0", port), QuietHandler) as httpd:
            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[ota] Stopped.")
    finally:
        stop_evt.set()
    return 0


def publish_remote_ota(
    profile: DeviceProfile,
    remote_ssh: str | None = None,
    base_url: str | None = None,
    version: str | None = None,
    notes: str = "espdeck release",
    dry_run: bool = False,
) -> int:
    ota_cfg = profile.raw.get("ota", {})
    target_ssh = remote_ssh or ota_cfg.get("remote_ssh", "")
    pub_url = base_url or ota_cfg.get("remote_url", "")

    if not pub_url:
        print(
            "[ota] ERROR: No public base URL configured. Pass --base-url https://ota.<your-domain> "
            f"or set ota.remote_url in profiles/{profile.device}.json.",
            file=sys.stderr,
        )
        return 1

    stage_dir, manifest = stage_release(profile, base_url=pub_url, version=version, notes=notes)
    print(f"[ota] Staged release in {stage_dir}:")
    print(f"      version={manifest['version']} size={manifest['size'] / 1048576:.2f} MB sha256={manifest['sha256'][:16]}...")
    print(f"      manifest={pub_url.rstrip('/')}/{profile.device}/latest.json")

    if not target_ssh:
        print("[ota] No remote_ssh specified; files are staged locally only.")
        return 0

    # Ensure remote device directory exists & rsync latest.json + firmware.bin
    host, remote_root = target_ssh.split(":", 1)
    remote_dev_dir = f"{remote_root.rstrip('/')}/{profile.device}"
    mkdir_cmd = ["ssh", host, f"mkdir -p '{remote_dev_dir}'"]
    rsync_cmd = [
        "rsync", "-avz", "--progress",
        str(stage_dir / "latest.json"),
        str(stage_dir / "firmware.bin"),
        f"{host}:{remote_dev_dir}/",
    ]

    if dry_run:
        print("[ota] DRY-RUN:", " ".join(mkdir_cmd))
        print("[ota] DRY-RUN:", " ".join(rsync_cmd))
        return 0

    r1 = subprocess.run(mkdir_cmd)
    if r1.returncode != 0:
        return r1.returncode
    r2 = subprocess.run(rsync_cmd)
    if r2.returncode == 0:
        print(f"[ota] Published to {host}:{remote_dev_dir}/ successfully!")
    return r2.returncode
