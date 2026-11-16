"""Finds this setup's boards by USB ID, narrowed by the serials pinned in mise.local.toml, so a second board
with the same ID (another P4, another Nano) is never opened by mistake."""
import os
import pathlib
import sys

from serial.tools.list_ports import comports


def _local_env():
    """[env] from the repo's mise.local.toml, for scripts run outside mise."""
    path = pathlib.Path(__file__).resolve().parent.parent / "mise.local.toml"
    if not path.exists():
        return {}
    section, env = None, {}
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line.startswith("["):
            section = line
        elif section == "[env]" and "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"')
    return env


ENV = {**_local_env(), **os.environ}

PINS = {
    (0x303A, 0x8000): "P4VRX_P4_USB_SERIAL",
    (0x303A, 0x0012): "P4VRX_P4_USB_SERIAL",
    (0x1A86, 0x55D3): "P4VRX_P4_UART_SERIAL",
    (0x0403, 0x6010): "P4VRX_NANO_SERIAL",
}


def same_serial(a, b):
    return (a or "").replace(":", "").lower() == (b or "").replace(":", "").lower()


def port(vid, pid, suffix=""):
    """The one port with this USB ID (and device-name suffix, for the Nano's second interface)."""
    var = PINS.get((vid, pid))
    pin = ENV.get(var, "") if var else ""
    found = [p for p in comports() if (p.vid, p.pid) == (vid, pid) and p.device.endswith(suffix)]
    if pin:
        found = [p for p in found if same_serial(p.serial_number, pin)]
    if len(found) != 1:
        hint = f"; set {var} in mise.local.toml" if var and len(found) > 1 else ""
        sys.exit(f"expected one {vid:04x}:{pid:04x} port, found {[p.device for p in found]}{hint}")
    return found[0].device
