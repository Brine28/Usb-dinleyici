# USB Monitor

Windows'ta arka planda çalışan, her USB cihaz takıldığında cihazın markasını/ürününü tanıyıp bildirim gösteren ve bir log dosyasına kaydeden hafif bir araç.

## Nasıl çalışır?

```
usb_monitor.exe  (arka planda, tepsi ikonlu)
      │  USB takma olayını yakalar (WM_DEVICECHANGE)
      │  VID/PID'i device_info_*.txt dosyasına yazar
      ▼
usb_lookup.py
      │  VID/PID'i usb.ids veritabanında (Gentoo hwids) arar
      │  Windows toast bildirimi gösterir
      ▼
usb_devices_log.txt  (tüm geçmiş burada birikir)
```

- **usb_monitor.cpp** — Win32 API ile yazılmış, arka planda çalışan, sistem tepsisinde ikonu olan izleyici. USB tak olaylarını dinler, VID/PID'i ayrıştırır ve `usb_lookup.py`'yi tetikler.
- **usb_lookup.py** — VID/PID'den marka/ürün adını bulur, Windows bildirimi gösterir, sonucu loglar.

## Özellikler

- Sistem tepsisinde ikon, sağ tık menüsünde **Çıkış**
- Tek instance koruması (aynı anda iki kez başlatılamaz)
- `usb.ids` veritabanı 7 gün önbelleklenir, gereksiz indirme yapılmaz
- Aynı anda birden fazla cihaz takılırsa dosya çakışması olmaz (her olay için benzersiz geçici dosya)
- Tamamen bağımsız `.exe` — çalıştırmak için MinGW runtime DLL'leri gerekmez (statik link)

## Gereksinimler

- Windows 10/11
- Python 3.x (PATH'te olmalı) + aşağıdaki paketler:
  ```
  pip install requests winotify
  ```
- Derlemek için: MinGW-w64 (Linux'ta cross-compile veya Windows'ta MSYS2)

## Derleme

Linux üzerinden Windows için cross-compile:

```bash
sudo apt install mingw-w64

x86_64-w64-mingw32-g++ -std=c++17 -O2 -mwindows -municode \
  usb_monitor.cpp -o usb_monitor.exe \
  -static -static-libgcc -static-libstdc++ \
  -luser32 -lshell32 -lole32 -lcomctl32 -lwinpthread
```

> `-static*` bayrakları sayesinde üretilen `usb_monitor.exe`, hedef makinede `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` gibi ek DLL'lere ihtiyaç duymaz; sadece Windows'un kendi sistem DLL'lerini (`kernel32`, `user32`, `shell32`, `msvcrt`) kullanır.

MSYS2 MinGW-w64 terminalinde de aynı komut (öntakı olmadan `g++` ile) çalışır.

## Kurulum / Kullanım

1. `usb_monitor.exe` ve `usb_lookup.py` dosyalarını **aynı klasöre** koyun.
2. `pip install requests winotify` ile bağımlılıkları kurun.
3. `usb_monitor.exe`'yi çalıştırın — tepsi ikonu belirir.
4. Bir USB cihaz taktığınızda bildirim ve log otomatik oluşur.
5. Bilgisayar açılışında otomatik başlaması için `usb_monitor.exe`'nin kısayolunu:
   ```
   shell:startup
   ```
   klasörüne koyabilirsiniz.

Log dosyası (`usb_devices_log.txt`) örneği:

```
[2026-07-17T14:32:05] VID=0951 PID=1666 Marka=Kingston Technology Urun=DataTraveler 100 G3
```

## Dosya yapısı

```
.
├── usb_monitor.cpp      # İzleyici (Win32, sistem tepsisi)
├── usb_lookup.py        # VID/PID -> marka/ürün çözümleme + bildirim
├── usb.ids.cache        # Otomatik oluşur (usb.ids önbelleği)
└── usb_devices_log.txt  # Otomatik oluşur (geçmiş log)
```


