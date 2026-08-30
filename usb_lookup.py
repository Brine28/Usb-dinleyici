# Copyright (c) 2026 USB Monitor contributors.
"""Look up USB device information and record the detected device.

usb_monitor.exe calls this script for each USB connection:
    python.exe usb_lookup.py device_info_XXXX.txt

The script reads VID/PID information, looks up the vendor and product in the
current Gentoo hwids usb.ids database, displays a Windows notification,
appends the result to a log file, and removes the processed information file.

Dependencies:
    pip install requests winotify

usb_monitor.exe starts this script with its working directory set to the
executable directory, so cache and log files are stored next to the executable.
"""

from contextlib import suppress
from datetime import datetime
from pathlib import Path
import re
import sys
import time

try:
    import requests
except ImportError:
    sys.stderr.write(
        "HATA: 'requests' paketi kurulu degil. "
        "Kurmak icin: pip install requests\n"
    )
    sys.exit(1)

try:
    from winotify import Notification
except ImportError:
    Notification = None


USB_IDS_URL = "https://raw.githubusercontent.com/gentoo/hwids/master/usb.ids"
CACHE_FILE = Path("usb.ids.cache")
CACHE_MAX_AGE = 7 * 24 * 3600
LOG_FILE = Path("usb_devices_log.txt")
EXPECTED_ARGUMENT_COUNT = 2
FILE_ENCODING = "utf-8"


def download_usb_ids() -> str:
    """Download usb.ids or return a fresh cached copy."""
    if CACHE_FILE.exists():
        age = time.time() - CACHE_FILE.stat().st_mtime
        if age < CACHE_MAX_AGE:
            return CACHE_FILE.read_text(encoding=FILE_ENCODING, errors="ignore")

    try:
        response = requests.get(USB_IDS_URL, timeout=15)
        response.raise_for_status()
        temporary_path = CACHE_FILE.with_name(f"{CACHE_FILE.name}.tmp")
        with temporary_path.open("wb") as file_handle:
            file_handle.write(response.content)
        temporary_path.replace(CACHE_FILE)
    except (requests.RequestException, OSError) as error:
        sys.stderr.write(
            f"usb.ids indirilemedi, mevcut onbellek kullanilacak: {error}\n"
        )

    if not CACHE_FILE.exists():
        error_message = (
            "usb.ids veritabani bulunamadi ve internetten indirilemedi."
        )
        raise RuntimeError(error_message)

    return CACHE_FILE.read_text(encoding=FILE_ENCODING, errors="ignore")


def find_vendor_product(
    usb_ids_text: str,
    vid: str,
    pid: str,
) -> tuple[str | None, str | None]:
    """Find vendor and product names matching the supplied VID and PID."""
    normalized_vid = vid.lower()
    normalized_pid = pid.lower()
    vendor_name = None
    product_name = None
    current_vendor_id = None

    for line in usb_ids_text.splitlines():
        stripped_line = line.strip()
        if not stripped_line or stripped_line.startswith("#"):
            continue
        if line.startswith("\t\t"):
            continue
        if line.startswith("\t"):
            if current_vendor_id == normalized_vid:
                match = re.match(r"\t([0-9a-fA-F]{4})\s+(.+)", line)
                if match and match.group(1).lower() == normalized_pid:
                    product_name = match.group(2).strip()
            continue

        match = re.match(r"([0-9a-fA-F]{4})\s+(.+)", line)
        if match:
            current_vendor_id = match.group(1).lower()
            if current_vendor_id == normalized_vid:
                vendor_name = match.group(2).strip()

    return vendor_name, product_name


def parse_info_file(path: str) -> dict[str, str]:
    """Parse KEY=VALUE entries from a device information file."""
    data: dict[str, str] = {}
    info_path = Path(path)

    with info_path.open(encoding=FILE_ENCODING, errors="ignore") as file_handle:
        for raw_line in file_handle:
            line = raw_line.strip()
            if "=" in line:
                key, value = line.split("=", 1)
                data[key.strip()] = value.strip()

    return data


def notify(
    vendor: str | None,
    product: str | None,
    vid: str,
    pid: str,
) -> None:
    """Display a Windows toast notification for the detected USB device."""
    title = "USB Cihaz Baglandi"
    if vendor:
        message = f"{vendor} markali cihaz baglandi"
        if product:
            message += f" ({product})"
    else:
        message = f"Bilinmeyen cihaz baglandi (VID:{vid} PID:{pid})"

    if Notification is not None:
        try:
            toast = Notification(
                app_id="USB Monitor",
                title=title,
                msg=message,
            )
            toast.show()
        except (OSError, RuntimeError, ValueError) as error:
            sys.stderr.write(f"Bildirim gosterilemedi: {error}\n")

    sys.stderr.write(f"{title}: {message}\n")


def save_result(
    vid: str,
    pid: str,
    vendor: str | None,
    product: str | None,
) -> None:
    """Append detected USB device information to the log file."""
    timestamp = datetime.now().astimezone().isoformat(timespec="seconds")
    with LOG_FILE.open("a", encoding=FILE_ENCODING) as file_handle:
        file_handle.write(
            f"[{timestamp}] "
            f"VID={vid} PID={pid} Marka={vendor or 'Bilinmiyor'} "
            f"Urun={product or 'Bilinmiyor'}\n",
        )


def main() -> None:
    """Process the device information file supplied on the command line."""
    if len(sys.argv) < EXPECTED_ARGUMENT_COUNT:
        sys.stderr.write("Kullanim: usb_lookup.py <device_info.txt>\n")
        sys.exit(1)

    info_path = Path(sys.argv[1])

    try:
        data = parse_info_file(str(info_path))
        vid = data.get("VID", "")
        pid = data.get("PID", "")

        if not vid or not pid:
            sys.stderr.write("VID/PID bilgisi bulunamadi.\n")
            sys.exit(1)

        usb_ids_text = download_usb_ids()
        vendor, product = find_vendor_product(usb_ids_text, vid, pid)

        notify(vendor, product, vid, pid)
        save_result(vid, pid, vendor, product)
    finally:
        with suppress(OSError):
            info_path.unlink()


if __name__ == "__main__":
    main()
