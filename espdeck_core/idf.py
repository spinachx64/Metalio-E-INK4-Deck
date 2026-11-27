"""ESP-IDF build & MAC-verified safe flashing helpers."""

from __future__ import annotations

import glob
import re
import subprocess
import sys
from pathlib import Path

from .config import DeviceProfile


def _idf_prefix(profile: DeviceProfile) -> str:
    if profile.idf_env.is_file():
        return f'source "{profile.idf_env}" && '
    return ""


def _run_shell(cmd: str) -> int:
    proc = subprocess.run(["/bin/zsh", "-lc", cmd])
    return proc.returncode


def build_device(profile: DeviceProfile, extra_args: list[str] | None = None) -> int:
    args = " ".join(extra_args or [])
    cmd = f'{_idf_prefix(profile)}idf.py -C "{profile.build_dir}" build {args}'.strip()
    print(f"[idf] Running: {cmd}")
    return _run_shell(cmd)


def _detect_ports() -> list[str]:
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))


def _read_mac(profile: DeviceProfile, port: str) -> str:
    cmd = f'{_idf_prefix(profile)}python -m esptool --chip esp32s3 -p "{port}" read_mac'
    for attempt in range(2):
        res = subprocess.run(["/bin/zsh", "-lc", cmd], capture_output=True, text=True)
        m = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", res.stdout + "\n" + res.stderr)
        if m:
            return m.group(1).lower()
        if attempt == 0:
            print(f"[idf] First MAC probe missed on {port} (device may be in Light Sleep), retrying...")
    return ""


def flash_device(profile: DeviceProfile, port: str | None = None, skip_mac_check: bool = False) -> int:
    if not port:
        ports = _detect_ports()
        if not ports:
            print("[idf] ERROR: No /dev/cu.usbmodem* or /dev/cu.usbserial* port found.", file=sys.stderr)
            return 1
        port = ports[0]

    print(f"[idf] Probing board on {port} ...")
    mac = _read_mac(profile, port)
    if not skip_mac_check and profile.board_mac:
        if not mac:
            print(f"[idf] ERROR: Could not read MAC on {port}; refusing to flash.", file=sys.stderr)
            return 2
        if mac != profile.board_mac:
            print(
                f"[idf] SAFETY ABORT: Connected board MAC is {mac}, "
                f"but profile '{profile.device}' expects {profile.board_mac}!",
                file=sys.stderr,
            )
            return 2
        print(f"[idf] Verified MAC {mac} matches '{profile.device}'.")

    cmd = (
        f'{_idf_prefix(profile)}idf.py -C "{profile.build_dir}" '
        f'-p "{port}" -b {profile.baud} {profile.flash_mode}'
    )
    print(f"[idf] Flashing: {cmd}")
    return _run_shell(cmd)
