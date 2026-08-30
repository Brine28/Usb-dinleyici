// usb_monitor.cpp
// Arka planda USB tak/cikar olaylarini dinler, VID/PID bilgisini
// device_info_*.txt dosyasina yazar ve usb_lookup.py betigini tetikler.
// Sistem tepsisinde ikon gosterir; sag tik menusunden "Cikis" secilebilir.

#include <windows.h>
#include <dbt.h>
#include <initguid.h>
#include <usbiodef.h>
#include <shellapi.h>
#include <strsafe.h>

#include <string>
#include <fstream>
#include <vector>
#include <atomic>

// ---- Ayarlanabilir sabitler ----
static const std::wstring PYTHON_EXE  = L"python.exe";
static const std::wstring SCRIPT_NAME = L"usb_lookup.py";

#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_EXIT 1001

static NOTIFYICONDATAW g_nid = {};
static std::atomic<int> g_counter{0};

static std::wstring CurrentTimestamp() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t buf[64] = {};
    if (FAILED(StringCchPrintfW(
            buf, ARRAYSIZE(buf),
            L"%04u-%02u-%02u %02u:%02u:%02u",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond)))) {
        return L"";
    }

    return buf;
}

// exe'nin bulundugu klasoru dondurur (sonunda ters slash olmadan).
static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));

    if (len == 0 || len >= ARRAYSIZE(path)) {
        return L".";
    }

    const std::wstring full(path, len);
    const size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : full.substr(0, pos);
}

// Ayni anda birden fazla cihaz takilirsa dosyalarin birbirine karismamasi
// icin her olay icin benzersiz bir device_info dosya adi uretir.
static std::wstring MakeUniqueInfoPath(const std::wstring& dir) {
    wchar_t buf[96] = {};
    const int c = ++g_counter;

    if (FAILED(StringCchPrintfW(
            buf, ARRAYSIZE(buf),
            L"device_info_%llu_%d.txt",
            static_cast<unsigned long long>(GetTickCount64()), c))) {
        return dir + L"\\device_info_fallback.txt";
    }

    return dir + L"\\" + buf;
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

    if (vidPos + 8 > name.size() || pidPos + 8 > name.size()) {
        return false;
    }

    vid = name.substr(vidPos + 4, 4);
    pid = name.substr(pidPos + 4, 4);
    return true;
}

static void LaunchPython(
    const std::wstring& scriptPath,
    const std::wstring& infoFile,
    const std::wstring& workDir) {

    std::wstring cmdLine =
        L"\"" + PYTHON_EXE + L"\" \"" + scriptPath + L"\" \"" + infoFile + L"\"";

    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    // python betiginin calisma dizini her zaman exe klasoru olsun.
    if (CreateProcessW(
            NULL,
            buf.data(),
            NULL,
            NULL,
            FALSE,
            CREATE_NO_WINDOW,
            NULL,
            workDir.c_str(),
            &si,
            &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static void AddTrayIcon(HWND hWnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);

    // Cppcheck'in lstrcpynWCalled uyarisini gidermek icin StringCchCopyW.
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

static void ShowTrayMenu(HWND hWnd) {
    POINT pt = {};
    if (!GetCursorPos(&pt)) {
        return;
    }

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        return;
    }

    if (!AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"\u00c7\u0131k\u0131\u015f")) {
        DestroyMenu(hMenu);
        return;
    }

    SetForegroundWindow(hWnd);
    TrackPopupMenu(
        hMenu,
        TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x,
        pt.y,
        0,
        hWnd,
        NULL);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam) {

    static std::wstring exeDir;
    static std::wstring scriptPath;

    switch (msg) {
    case WM_CREATE:
        exeDir = GetExeDir();
        scriptPath = exeDir + L"\\" + SCRIPT_NAME;
        AddTrayIcon(hWnd);
        return 0;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL) {
            const auto* hdr =
                reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);

            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                const auto* dev =
                    reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(hdr);

                const std::wstring name = dev->dbcc_name;
                std::wstring vid;
                std::wstring pid;

                if (ParseVidPid(name, vid, pid)) {
                    const std::wstring infoFile = MakeUniqueInfoPath(exeDir);
                    std::wofstream ofs(infoFile.c_str(), std::ios::trunc);

                    if (ofs) {
                        ofs << L"VID=" << vid << L"\n";
                        ofs << L"PID=" << pid << L"\n";
                        ofs << L"RAW=" << name << L"\n";
                        ofs << L"TIME=" << CurrentTimestamp() << L"\n";
                        ofs.close();

                        LaunchPython(scriptPath, infoFile, exeDir);
                    }
                }
            }
            return TRUE;
        }
        break;

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
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Tek instance. hMutex her exit yolunda mutlaka kapatilacak.
    HANDLE hMutex = CreateMutexW(
        NULL,
        TRUE,
        L"Local\\USBMonitorSingleInstanceMutex");

    if (hMutex == NULL) {
        MessageBoxW(
            NULL,
            L"USB Monitor mutex olusturulamadi.",
            L"USB Monitor",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    const DWORD mutexError = GetLastError();

    if (mutexError == ERROR_ALREADY_EXISTS) {
        // Bu durumda mutex'in sahibi biz degiliz; sadece handle'i kapat.
        CloseHandle(hMutex);
        MessageBoxW(
            NULL,
            L"USB Monitor zaten \u00e7al\u0131\u015f\u0131yor.",
            L"USB Monitor",
            MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (mutexError != ERROR_SUCCESS) {
        CloseHandle(hMutex);
        MessageBoxW(
            NULL,
            L"USB Monitor mutex kontrolu basarisiz oldu.",
            L"USB Monitor",
            MB_OK | MB_ICONERROR);
        return 1;
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
                NULL,
                L"Window class kaydedilemedi.",
                L"USB Monitor",
                MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    // Tepsi ikonu ve device notification icin gercek gizli HWND.
    HWND hWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"USB Monitor",
        0,
        0,
        0,
        0,
        0,
        NULL,
        NULL,
        hInstance,
        NULL);

    if (!hWnd) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        MessageBoxW(
            NULL,
            L"Gizli pencere olusturulamadi.",
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
            NULL,
            L"USB device notification kaydedilemedi.",
            L"USB Monitor",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterDeviceNotification(hNotify);
    DestroyWindow(hWnd);

    // Normal shutdown: mutex sahipligi bizde, once release sonra close.
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return 0;
}
