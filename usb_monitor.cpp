// usb_monitor.cpp
// Arka planda USB tak/cikar olaylarini dinler, VID/PID bilgisini
// device_info_*.txt dosyasina yazar ve usb_lookup.py betigini tetikler.
// Sistem tepsisinde ikon gosterir; sag tik menusunden "Cikis" secilebilir.
//
// Derleme (MSYS2 MinGW-w64 terminalinde veya Linux'ta cross-compile):
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -mwindows -municode \
//     usb_monitor.cpp -o usb_monitor.exe \
//     -static -static-libgcc -static-libstdc++ \
//     -luser32 -lshell32 -lole32 -lcomctl32 -lwinpthread
//
// -static* bayraklari sayesinde hedef makinede libstdc++-6.dll,
// libgcc_s_seh-1.dll, libwinpthread-1.dll gibi ek DLL'lere ihtiyac kalmaz;
// tek basina calisan bir usb_monitor.exe uretilir.

#include <windows.h>
#include <dbt.h>
#include <initguid.h>   // GUID_DEVINTERFACE_USB_DEVICE'i bu TU icinde gercekten tanimlar
#include <usbiodef.h>
#include <shellapi.h>
#include <string>
#include <fstream>
#include <vector>
#include <atomic>

// ---- Ayarlanabilir sabitler ----
static const std::wstring PYTHON_EXE  = L"python.exe";      // PATH'te aranir
static const std::wstring SCRIPT_NAME = L"usb_lookup.py";   // exe ile ayni klasorde varsayilir

#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_EXIT 1001

static NOTIFYICONDATAW g_nid = {};
static std::atomic<int> g_counter{0};

static std::wstring CurrentTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// exe'nin bulundugu klasoru dondurur (sonunda ters slash olmadan).
static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (len == 0) return L".";
    std::wstring full(path, len);
    size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : full.substr(0, pos);
}

// Ayni anda birden fazla cihaz takilirsa dosyalarin birbirine karismamasi
// icin her olay icin benzersiz bir device_info dosya adi uretir.
static std::wstring MakeUniqueInfoPath(const std::wstring& dir) {
    wchar_t buf[96];
    int c = ++g_counter;
    swprintf(buf, 96, L"device_info_%llu_%d.txt",
             (unsigned long long)GetTickCount64(), c);
    return dir + L"\\" + buf;
}

// dbcc_name ornegi:
// \\?\USB#VID_0951&PID_1666#0123456789AB#{a5dcbf10-6530-11d2-901f-00c04fb951ed}
static bool ParseVidPid(const std::wstring& name, std::wstring& vid, std::wstring& pid) {
    size_t vidPos = name.find(L"VID_");
    size_t pidPos = name.find(L"PID_");
    if (vidPos == std::wstring::npos || pidPos == std::wstring::npos) return false;
    if (vidPos + 8 > name.size() || pidPos + 8 > name.size()) return false;
    vid = name.substr(vidPos + 4, 4);
    pid = name.substr(pidPos + 4, 4);
    return true;
}

static void LaunchPython(const std::wstring& scriptPath,
                          const std::wstring& infoFile,
                          const std::wstring& workDir) {
    std::wstring cmdLine = L"\"" + PYTHON_EXE + L"\" \"" + scriptPath + L"\" \"" + infoFile + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    // lpCurrentDirectory: usb_lookup.py'nin onbellek/log dosyalarini her zaman
    // exe'nin klasorunde olusturmasini saglar (nereden tetiklenirse tetiklensin).
    if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, workDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static void AddTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"USB Monitor", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"\u00c7\u0131k\u0131\u015f"); // "Çıkış"
    // Menunun disina tiklandiginda kapanmasi icin gerekli standart Win32 numarasi.
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
            PDEV_BROADCAST_HDR hdr = (PDEV_BROADCAST_HDR)lParam;
            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                PDEV_BROADCAST_DEVICEINTERFACE dev = (PDEV_BROADCAST_DEVICEINTERFACE)hdr;
                std::wstring name = dev->dbcc_name;
                std::wstring vid, pid;
                if (ParseVidPid(name, vid, pid)) {
                    std::wstring infoFile = MakeUniqueInfoPath(exeDir);
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
    // Tek instance calismasini garanti eder; ayni programi iki kez
    // baslatmak eski usb.ids indirmelerini/tepsi ikonlarini cakistirmaz.
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\USBMonitorSingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"USB Monitor zaten \u00e7al\u0131\u015f\u0131yor.",
                    L"USB Monitor", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    const wchar_t CLASS_NAME[] = L"USBMonitorHiddenWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    // Gorunmez bir "message-only" degil, normal gizli pencere: tepsi ikonu
    // icin Shell_NotifyIcon'un guvenilir calismasi adina HWND_MESSAGE yerine
    // gercek (gorunmeyen) bir pencere kullaniyoruz.
    HWND hWnd = CreateWindowExW(0, CLASS_NAME, L"USB Monitor",
                                 0, 0, 0, 0, 0,
                                 NULL, NULL, hInstance, NULL);
    if (!hWnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE filter = {};
    filter.dbcc_size       = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid  = GUID_DEVINTERFACE_USB_DEVICE;

    HDEVNOTIFY hNotify = RegisterDeviceNotificationW(hWnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!hNotify) {
        DestroyWindow(hWnd);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterDeviceNotification(hNotify);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}
