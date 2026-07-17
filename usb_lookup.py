"""
usb_lookup.py
usb_monitor.exe tarafindan her USB takilisinda su sekilde cagrilir:
    python.exe usb_lookup.py device_info_XXXX.txt

Yaptiklari:
  1) device_info dosyasindan VID/PID'i okur
  2) Internetten (Gentoo hwids reposundaki guncel usb.ids veritabani) markayi bulur
  3) Windows bildirimi gosterir
  4) Sonucu usb_devices_log.txt dosyasina ekler
  5) Islenen device_info dosyasini siler (usb_monitor.exe her olay icin
     benzersiz bir dosya olusturuyor, birikmesinler diye temizliyoruz)

Gerekli paketler (requirements.txt uzerinden):
    pip install requests winotify

Not: Bu betik, usb_monitor.exe tarafindan calisma dizini (cwd) kendi
bulundugu klasore ayarlanarak baslatilir; bu yuzden USB_IDS cache ve log
dosyalari her zaman exe ile ayni klasorde olusur, betik nereden
tetiklenirse tetiklensin.
"""

import sys
import os
import re
import time
from datetime import datetime

try:
    import requests
except ImportError:
    print("HATA: 'requests' paketi kurulu degil. Kurmak icin: pip install requests")
    sys.exit(1)

USB_IDS_URL = "https://raw.githubusercontent.com/gentoo/hwids/master/usb.ids"
CACHE_FILE = "usb.ids.cache"
CACHE_MAX_AGE = 7 * 24 * 3600  # 7 gun, gereksiz indirmeyi onlemek icin
LOG_FILE = "usb_devices_log.txt"


def download_usb_ids() -> str:
    """usb.ids veritabanini indirir (veya taze bir onbellek varsa onu kullanir)."""
    need_download = True
    if os.path.exists(CACHE_FILE):
        age = time.time() - os.path.getmtime(CACHE_FILE)
        if age < CACHE_MAX_AGE:
            need_download = False

    if need_download:
        try:
            resp = requests.get(USB_IDS_URL, timeout=15)
            resp.raise_for_status()
            # Once gecici dosyaya yaz, sonra atomik olarak yerine koy;
            # indirme yarida kesilirse onbellek bozulmamis olur.
            tmp_path = CACHE_FILE + ".tmp"
            with open(tmp_path, "wb") as f:
                f.write(resp.content)
            os.replace(tmp_path, CACHE_FILE)
        except Exception as e:
            print(f"usb.ids indirilemedi, mevcut onbellek kullanilacak: {e}")

    if not os.path.exists(CACHE_FILE):
        raise RuntimeError("usb.ids veritabani bulunamadi ve internetten indirilemedi.")

    with open(CACHE_FILE, "r", encoding="utf-8", errors="ignore") as f:
        return f.read()


def find_vendor_product(usb_ids_text: str, vid: str, pid: str):
    """usb.ids metni icinde VID -> marka adi, PID -> urun adi eslemesini arar."""
    vid = vid.lower()
    pid = pid.lower()
    vendor_name = None
    product_name = None
    current_vendor_id = None

    for line in usb_ids_text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        if line.startswith("\t\t"):
            continue  # interface satirlari, bizi ilgilendirmiyor
        if line.startswith("\t"):
            if current_vendor_id == vid:
                m = re.match(r"\t([0-9a-fA-F]{4})\s+(.+)", line)
                if m and m.group(1).lower() == pid:
                    product_name = m.group(2).strip()
        else:
            m = re.match(r"([0-9a-fA-F]{4})\s+(.+)", line)
            if m:
                current_vendor_id = m.group(1).lower()
                if current_vendor_id == vid:
                    vendor_name = m.group(2).strip()
            else:
                # Vendor satiri degilse (ornegin "# comment" harici baska bir
                # blok basligiysa) mevcut vendor takibini bozmamak icin devam et.
                continue

    return vendor_name, product_name


def parse_info_file(path: str) -> dict:
    data = {}
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                data[k.strip()] = v.strip()
    return data


def notify(vendor, product, vid, pid):
    title = "USB Cihaz Baglandi"
    if vendor:
        msg = f"{vendor} markali cihaz baglandi"
        if product:
            msg += f" ({product})"
    else:
        msg = f"Bilinmeyen cihaz baglandi (VID:{vid} PID:{pid})"

    try:
        from winotify import Notification
        toast = Notification(app_id="USB Monitor", title=title, msg=msg)
        toast.show()
    except Exception as e:
        print(f"Bildirim gosterilemedi: {e}")

    print(f"{title}: {msg}")


def save_result(vid, pid, vendor, product):
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(
            f"[{datetime.now().isoformat(timespec='seconds')}] "
            f"VID={vid} PID={pid} Marka={vendor or 'Bilinmiyor'} "
            f"Urun={product or 'Bilinmiyor'}\n"
        )


def main():
    if len(sys.argv) < 2:
        print("Kullanim: usb_lookup.py <device_info.txt>")
        sys.exit(1)

    info_path = sys.argv[1]

    try:
        data = parse_info_file(info_path)
        vid = data.get("VID", "")
        pid = data.get("PID", "")

        if not vid or not pid:
            print("VID/PID bilgisi bulunamadi.")
            sys.exit(1)

        usb_ids_text = download_usb_ids()
        vendor, product = find_vendor_product(usb_ids_text, vid, pid)

        notify(vendor, product, vid, pid)
        save_result(vid, pid, vendor, product)
    finally:
        # usb_monitor.exe her olay icin ayri bir device_info dosyasi
        # olusturuyor; isimiz bitince onu temizleyelim ki klasor dolmasin.
        try:
            os.remove(info_path)
        except OSError:
            pass


if __name__ == "__main__":
    main()
