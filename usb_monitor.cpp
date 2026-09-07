// usb_monitor.cpp
// Arka planda USB tak/cikar olaylarini dinler, VID/PID'i cozer, Gentoo'nun
// usb.ids veritabaninda marka/urun adini bulur, tepsi bildirimi gosterir ve
// bir UTF-8 log dosyasina yazar. Tamamen tek bir .exe -- Python veya baska hicbir
// disaridan yorumlayici/betik gerektirmez.

#include <windows.h>
#include <dbt.h>
#include <initguid.h>
#include <usbiodef.h>
#include <shellapi.h>
#include <strsafe.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// ---- Ayarlanabilir sabitler ----
static constexpr wchar_t USB_IDS_HOST[] = L"raw.githubusercontent.com";
static constexpr wchar_t USB_IDS_PATH[] = L"/gentoo/hwids/master/usb.ids";
static constexpr wchar_t CACHE_NAME[] = L"usb.ids.cache";
static constexpr wchar_t LOG_NAME[] = L"usb_devices_log.txt";
static constexpr auto CACHE_MAX_AGE = std::chrono::hours(7 * 24);

#define WM_TRAYICON      (WM_APP + 1)
#define WM_APP_NOTIFY    (WM_APP + 2)
#define ID_TRAY_EXIT     1001

static NOTIFYICONDATAW g_nid = {};
static std::mutex g_logMutex;
static std::mutex g_cacheMutex;
static std::atomic_bool g_shuttingDown = false;

struct NotifyPayload {
    std::wstring title;
    std::wstring message;
};

// UTF-8 <-> UTF-16 donusumleri Windows'un resmi donusum API'leriyle yapilir.
// std::wstring(bytes.begin(), bytes.end()) UTF-8'i bozabilecegi icin kullanilmaz.
static std::optional<std::wstring> Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring{};
    }

    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    const int sourceLength = static_cast<int>(utf8.size());
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        sourceLength,
        nullptr,
        0);

    if (required <= 0) {
        return std::nullopt;
    }

    std::wstring result(static_cast<size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        sourceLength,
        result.data(),
        required);

    if (written != required) {
        return std::nullopt;
    }

    return result;
}

static std::optional<std::string> WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string{};
    }

    if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    const int sourceLength = static_cast<int>(wide.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        sourceLength,
        nullptr,
        0,
        nullptr,
        nullptr);

    if (required <= 0) {
        return std::nullopt;
    }

    std::string result(static_cast<size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        sourceLength,
        result.data(),
        required,
        nullptr,
        nullptr);

    if (written != required) {
        return std::nullopt;
    }

    return result;
}

static std::wstring CurrentTimestamp() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    // "YYYY-MM-DD HH:MM:SS" = 19 wchar_t.
    wchar_t buf[20] = {};
    if (FAILED(StringCchPrintfW(
            buf,
            ARRAYSIZE(buf),
            L"%04u-%02u-%02u %02u:%02u:%02u",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond)))) {
        return {};
    }

    // Basari durumunda uzunluk sabittir; wcslen() ile compiler'in agresif
    // stringop-overread analizini tetiklememek icin uzunlugu acikca veriyoruz.
    return std::wstring(buf, 19);
}

// exe'nin bulundugu klasoru dondurur (sonunda ters slash olmadan).
static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));

    if (len == 0 || len >= ARRAYSIZE(path)) {
        return L".";
    }

    const std::wstring full(path, len);
    const size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : full.substr(0, pos);
}

static std::wstring ToLowerW(std::wstring s);

static bool IsHex4(const std::wstring& value) {
    return value.size() == 4
        && std::all_of(value.begin(), value.end(),
                       [](wchar_t c) { return iswxdigit(c) != 0; });
}

// dbcc_name ornegi:
// \\?\USB#VID_0951&PID_1666#0123456789AB#{GUID}
static bool ParseVidPid(
    const std::wstring& name,
    std::wstring& vid,
    std::wstring& pid) {

    const size_t vidPos = name.find(L"VID_");
    const size_t pidPos = name.find(L"PID_");

    if (vidPos == std::wstring::npos || pidPos == std::wstring::npos) {
        return false;
    }

    // The markers themselves guarantee that at least four characters follow,
    // but keep the bounds check explicit for malformed device-interface names.
    if (vidPos + 8 > name.size() || pidPos + 8 > name.size()) {
        return false;
    }

    const std::wstring parsedVid = ToLowerW(name.substr(vidPos + 4, 4));
    const std::wstring parsedPid = ToLowerW(name.substr(pidPos + 4, 4));

    if (!IsHex4(parsedVid) || !IsHex4(parsedPid)) {
        return false;
    }

    vid = parsedVid;
    pid = parsedPid;
    return true;
}

static std::wstring ToLowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

// ---------------------------------------------------------------------
// WinHTTP uzerinden usb.ids indirme.
// ---------------------------------------------------------------------
static std::optional<std::wstring> DownloadUsbIds(const std::wstring& exeDir) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);

    const fs::path cachePath = fs::path(exeDir) / CACHE_NAME;

    auto readCache = [&]() -> std::optional<std::wstring> {
        std::ifstream in(cachePath, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }

        std::ostringstream ss;
        ss << in.rdbuf();
        if (!in.good() && !in.eof()) {
            return std::nullopt;
        }

        const std::string bytes = ss.str();
        return Utf8ToWide(bytes);
    };

    std::error_code ec;
    if (fs::exists(cachePath, ec) && !ec) {
        const auto mtime = fs::last_write_time(cachePath, ec);
        if (!ec) {
            const auto age = fs::file_time_type::clock::now() - mtime;
            if (age >= fs::file_time_type::duration::zero()
                && age < CACHE_MAX_AGE) {
                if (auto cached = readCache()) {
                    return cached;
                }
            }
        }
    }

    if (g_shuttingDown.load(std::memory_order_acquire)) {
        return readCache();
    }

    bool downloadOk = false;
    std::string body;

    HINTERNET hSession = WinHttpOpen(
        L"USBMonitor/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (hSession) {
        // Kapanista gereksiz yere uzun sure beklememek icin WinHTTP timeoutlari.
        (void)WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 10000);

        HINTERNET hConnect = WinHttpConnect(
            hSession, USB_IDS_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);

        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(
                hConnect, L"GET", USB_IDS_PATH,
                nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

            if (hRequest) {
                if (WinHttpSendRequest(
                        hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                    && WinHttpReceiveResponse(hRequest, nullptr)) {

                    DWORD statusCode = 0;
                    DWORD statusCodeSize = sizeof(statusCode);
                    const bool statusOk = WinHttpQueryHeaders(
                        hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusCodeSize,
                        WINHTTP_NO_HEADER_INDEX)
                        && statusCode == 200;

                    if (statusOk) {
                        char chunk[4096];
                        DWORD bytesRead = 0;
                        bool readOk = true;

                        do {
                            bytesRead = 0;
                            if (!WinHttpReadData(
                                    hRequest,
                                    chunk,
                                    sizeof(chunk),
                                    &bytesRead)) {
                                readOk = false;
                                break;
                            }
                            if (bytesRead > 0) {
                                body.append(chunk, bytesRead);
                            }
                        } while (bytesRead > 0);

                        downloadOk = readOk && !body.empty();
                    }
                }

                WinHttpCloseHandle(hRequest);
            }

            WinHttpCloseHandle(hConnect);
        }

        WinHttpCloseHandle(hSession);
    }

    if (downloadOk) {
        const fs::path tmpPath = cachePath.wstring() + L".tmp";

        bool tempWriteOk = false;
        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                tempWriteOk = static_cast<bool>(out);
            }
        }

        if (tempWriteOk) {
            bool installOk = ReplaceFileW(
                cachePath.c_str(),
                tmpPath.c_str(),
                nullptr,
                0,
                nullptr,
                nullptr) != FALSE;

            if (!installOk) {
                installOk = MoveFileExW(
                    tmpPath.c_str(),
                    cachePath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            }

            if (!installOk) {
                (void)DeleteFileW(tmpPath.c_str());
                downloadOk = false;
            }
        } else {
            (void)DeleteFileW(tmpPath.c_str());
            downloadOk = false;
        }
    }

    // Yeni indirme basarisizsa eski cache'i kullan.
    return readCache();
}

// ---------------------------------------------------------------------
// usb.ids metnini ayristirip verilen VID/PID icin marka/urun adini bulur.
// ---------------------------------------------------------------------
static void FindVendorProduct(
    const std::wstring& usbIdsText,
    const std::wstring& vid,
    const std::wstring& pid,
    std::optional<std::wstring>& vendorOut,
    std::optional<std::wstring>& productOut) {

    const std::wstring normVid = ToLowerW(vid);
    const std::wstring normPid = ToLowerW(pid);
    std::wstring currentVendorId;
    bool haveCurrentVendor = false;

    std::wistringstream stream(usbIdsText);
    std::wstring line;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }

        if (line.empty() || line[0] == L'#') {
            continue;
        }
        if (line.size() >= 2 && line[0] == L'\t' && line[1] == L'\t') {
            continue;
        }

        if (line[0] == L'\t') {
            if (haveCurrentVendor && currentVendorId == normVid && line.size() >= 5) {
                const std::wstring hex = ToLowerW(line.substr(1, 4));
                if (IsHex4(hex) && hex == normPid && line.size() > 5) {
                    size_t namePos = 5;
                    while (namePos < line.size() && iswspace(line[namePos])) {
                        ++namePos;
                    }
                    if (namePos < line.size()) {
                        productOut = line.substr(namePos);
                    }
                }
            }
            continue;
        }

        if (line.size() >= 4) {
            const std::wstring hex = ToLowerW(line.substr(0, 4));
            if (IsHex4(hex)) {
                currentVendorId = hex;
                haveCurrentVendor = true;

                if (currentVendorId == normVid && line.size() > 4) {
                    size_t namePos = 4;
                    while (namePos < line.size() && iswspace(line[namePos])) {
                        ++namePos;
                    }
                    if (namePos < line.size()) {
                        vendorOut = line.substr(namePos);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------
// Sonucu UTF-8 olarak log dosyasina ekler.
// ---------------------------------------------------------------------
static void LogResult(
    const std::wstring& exeDir,
    const std::wstring& vid,
    const std::wstring& pid,
    const std::optional<std::wstring>& vendor,
    const std::optional<std::wstring>& product) {

    std::lock_guard<std::mutex> lock(g_logMutex);

    const fs::path logPath = fs::path(exeDir) / LOG_NAME;
    std::ofstream ofs(logPath, std::ios::app | std::ios::binary);
    if (!ofs) {
        return;
    }

    std::wstring line = L"[";
    line += CurrentTimestamp();
    line += L"] VID=";
    line += vid;
    line += L" PID=";
    line += pid;
    line += L" Marka=";
    line += (vendor ? *vendor : L"Bilinmiyor");
    line += L" Urun=";
    line += (product ? *product : L"Bilinmiyor");
    line += L"\r\n";

    if (auto utf8 = WideToUtf8(line)) {
        ofs.write(utf8->data(), static_cast<std::streamsize>(utf8->size()));
    }
}

// ---------------------------------------------------------------------
// Bir USB olayini tamamen isleyen arka plan is parcasi.
// ---------------------------------------------------------------------
static void ProcessDeviceWorker(
    HWND hWnd,
    std::wstring exeDir,
    std::wstring vid,
    std::wstring pid) {

    std::optional<std::wstring> vendor;
    std::optional<std::wstring> product;

    if (auto usbIdsText = DownloadUsbIds(exeDir)) {
        FindVendorProduct(*usbIdsText, vid, pid, vendor, product);
    }

    LogResult(exeDir, vid, pid, vendor, product);

    if (g_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    auto payload = std::make_unique<NotifyPayload>();
    payload->title = L"USB Cihaz Bağlandı";

    if (vendor) {
        payload->message = *vendor;
        payload->message += L" markalı cihaz bağlandı";
        if (product) {
            payload->message += L" (";
            payload->message += *product;
            payload->message += L")";
        }
    } else {
        payload->message = L"Bilinmeyen cihaz bağlandı (VID:";
        payload->message += vid;
        payload->message += L" PID:";
        payload->message += pid;
        payload->message += L")";
    }

    NotifyPayload* rawPayload = payload.release();
    if (!PostMessageW(hWnd, WM_APP_NOTIFY, 0, reinterpret_cast<LPARAM>(rawPayload))) {
        delete rawPayload;
    }
}

static void AddTrayIcon(HWND hWnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
   g_nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));

    if (FAILED(StringCchCopyW(
            g_nid.szTip,
            ARRAYSIZE(g_nid.szTip),
            L"USB Monitor"))) {
        g_nid.szTip[0] = L'\0';
    }

    (void)Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon() {
    (void)Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowBalloon(const std::wstring& title, const std::wstring& message) {
    g_nid.uFlags |= NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;

    if (FAILED(StringCchCopyW(
            g_nid.szInfoTitle,
            ARRAYSIZE(g_nid.szInfoTitle),
            title.c_str()))) {
        g_nid.szInfoTitle[0] = L'\0';
    }
    if (FAILED(StringCchCopyW(
            g_nid.szInfo,
            ARRAYSIZE(g_nid.szInfo),
            message.c_str()))) {
        g_nid.szInfo[0] = L'\0';
    }

    (void)Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowTrayMenu(HWND hWnd) {
    POINT pt = {};
    if (!GetCursorPos(&pt)) {
        return;
    }

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        return;
    }

    if (!AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Çıkış")) {
        DestroyMenu(hMenu);
        return;
    }

    SetForegroundWindow(hWnd);
    (void)TrackPopupMenu(
        hMenu,
        TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x,
        pt.y,
        0,
        hWnd,
        nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam) {

    static std::wstring exeDir;

    switch (msg) {
    case WM_CREATE:
        exeDir = GetExeDir();
        AddTrayIcon(hWnd);
        return 0;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL) {
            const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);

            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                const auto* dev =
                    reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(hdr);

                const std::wstring name(dev->dbcc_name);
                std::wstring vid;
                std::wstring pid;

                if (ParseVidPid(name, vid, pid)
                    && !g_shuttingDown.load(std::memory_order_acquire)) {
                    try {
                        std::thread(
                            ProcessDeviceWorker,
                            hWnd,
                            exeDir,
                            vid,
                            pid).detach();
                    } catch (const std::system_error&) {
                        // Thread olusturulamamasi USB bildirimini engellemesin.
                    }
                }
            }
            return TRUE;
        }
        break;

    case WM_APP_NOTIFY: {
        std::unique_ptr<NotifyPayload> payload(
            reinterpret_cast<NotifyPayload*>(lParam));
        if (payload) {
            ShowBalloon(payload->title, payload->message);
        }
        return 0;
    }

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            ShowTrayMenu(hWnd);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            DestroyWindow(hWnd);
        }
        return 0;

    case WM_DESTROY:
        g_shuttingDown.store(true, std::memory_order_release);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    HANDLE hMutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Local\\USBMonitorSingleInstanceMutex");

    if (hMutex == nullptr) {
        MessageBoxW(
            nullptr,
            L"USB Monitor mutex oluşturulamadı.",
            L"USB Monitor",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    // CreateMutexW'de ERROR_ALREADY_EXISTS, adlandırılmış mutex'in zaten
    // var oldugunu kesin olarak gösteren durumdur. Yeni olusturma basarisinde
    // GetLastError()'ı ERROR_SUCCESS varsaymak doğru degildir; bu yüzden
    // yalnizca bu hatayi özel olarak kontrol ediyoruz.
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        MessageBoxW(
            nullptr,
            L"USB Monitor zaten çalışıyor.",
            L"USB Monitor",
            MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    const wchar_t CLASS_NAME[] = L"USBMonitorHiddenWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
            MessageBoxW(
                nullptr,
                L"Window class kaydedilemedi.",
                L"USB Monitor",
                MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    HWND hWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"USB Monitor",
        0,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hWnd) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        MessageBoxW(
            nullptr,
            L"Gizli pencere oluşturulamadı.",
            L"USB Monitor",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

    HDEVNOTIFY hNotify = RegisterDeviceNotificationW(
        hWnd,
        &filter,
        DEVICE_NOTIFY_WINDOW_HANDLE);

    if (!hNotify) {
        DestroyWindow(hWnd);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        MessageBoxW(
            nullptr,
            L"USB device notification kaydedilemedi.",
            L"USB Monitor",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // WM_COMMAND -> WM_DESTROY zaten pencereyi yok etti. Burada tekrar
    // DestroyWindow cagirmanin bir anlami yok; notify handle'i once kaldirilir.
    UnregisterDeviceNotification(hNotify);

    // The mutex was created with initial ownership by this thread.
    // Release it explicitly before closing the handle.
    (void)ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}
