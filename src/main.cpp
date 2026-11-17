// Wireless+ Battery - tray app showing the battery level of wireless mice, headsets and Bluetooth devices.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <tlhelp32.h>
#include <initguid.h>
#include <hidclass.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "devices.h"
#include "report.h"
#include "widget.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr UINT_PTR TIMER_LOCKS = 3;
constexpr UINT_PTR TIMER_SSH = 4;
constexpr DWORD POLL_INTERVAL_MS = 60 * 1000;
constexpr DWORD LOCK_POLL_MS = 150;  // Caps/Num/Scroll state: fast enough to feel instant
constexpr DWORD SSH_POLL_MS = 2000;  // same cadence as the standalone ssh-indicator.ps1
constexpr int LOW_BATTERY = 15;
constexpr UINT PLACEHOLDER_ID = 1;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_HIDE_LAST_KNOWN, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY, ID_SUPPORT, ID_REPORT,
    ID_LOCK_CAPS, ID_LOCK_NUM, ID_LOCK_SCROLL, ID_SSH_INDICATOR,
    ID_PWR_S15, ID_PWR_S30, ID_PWR_S60, ID_PWR_S120, ID_PWR_S240,
    ID_PWR_R15, ID_PWR_R30, ID_PWR_R60, ID_PWR_R120, ID_PWR_R240, ID_PWR_CANCEL,
    ID_DEVICE_FIRST = 1000
};
enum DeviceCmd : UINT { DEV_TOGGLE, DEV_LEFT, DEV_RIGHT, DEV_STRIDE = 4 };

const wchar_t* kAppName = L"Wireless+ Battery";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WirelessBattery";

const wchar_t* kSettingsKey = L"Software\\WirelessBattery";

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_refreshEvt, g_stopEvt, g_worker;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideLastKnown = false;  // hide (last known) / asleep devices from taskbar and tray
bool g_widgetBackground = false;  // draw a plate behind the taskbar text (transparent taskbar themes)

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    int lastPct = -1;        // last known percentage, kept while the device sleeps
    ULONGLONG lastSeen = 0;
    bool added = false;
    bool lowWarned = false;
    std::wstring iconSig;    // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
};
std::map<std::wstring, TrayEntry> g_entries;
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;

// ---- helpers ----------------------------------------------------------------------------------

UINT StableId(const std::wstring& key) {
    UINT h = 2166136261u;
    for (wchar_t c : key) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x100;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Devices the user chose to hide (by key), stored as a REG_MULTI_SZ.
std::set<std::wstring> g_hidden;

void LoadHidden() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_hidden.insert(p);
}

void SaveHidden() {
    std::wstring multi;
    for (const auto& k : g_hidden) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

// The user's left-to-right order (device keys), stored as a REG_MULTI_SZ. Devices not in it follow
// in the default order (by type, then name).
std::vector<std::wstring> g_order;

void LoadOrder() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_order.push_back(p);
}

void SaveOrder() {
    if (g_order.size() > 64) g_order.resize(64);  // forget long-gone devices
    std::wstring multi;
    for (const auto& k : g_order) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsEntryHidden(const TrayEntry& e) {
    return IsHidden(e.st.key) || (g_hideLastKnown && (e.st.asleep || (e.st.percent < 0 && e.lastPct >= 0)));
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadHideLastKnown() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideLastKnown(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- icon rendering ---------------------------------------------------------------------------
// Draws the percentage as large digits with a small level bar underneath. GDI text is rendered
// white-on-black into a scratch bitmap and its brightness is used as the alpha channel, which
// gives smooth edges on the transparent icon.

struct Rgb { BYTE r, g, b; };

HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
    const int barH = std::max(2, S / 8);
    const int textH = S - barH - 1;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // scratch bitmap for the glyphs
    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int fh = textH * 2; fh >= 6; fh--) {
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetTextCharacterExtra(dc, text.size() >= 3 ? -1 : 0);
        memset(sbits, 0, (size_t)W * H * 4);
        TextOutW(dc, 2, 0, text.c_str(), (int)text.size());
        GdiFlush();
        SelectObject(dc, oldF);
        DeleteObject(f);
        bx0 = W; by0 = H; bx1 = -1; by1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < 0) break;
        if (bx1 - bx0 + 1 <= S && by1 - by0 + 1 <= textH) break;
    }

    // final icon bitmap
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;  // straight alpha
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (textH - gh) / 2;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                put(ox + x, oy + y, textColor, sp[(by0 + y) * W + (bx0 + x)] & 0xFF);
    }
    // level bar: faint track + filled part
    int fill = barPct < 0 ? 0 : std::max(barPct > 0 ? 1 : 0, (barPct * S + 50) / 100);
    for (int y = S - barH; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (x < fill) put(x, y, barColor, 255);
            else put(x, y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, 70);
        }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

HICON IconFor(int pct, bool charging, bool dim) {
    Rgb fg = Foreground();
    Rgb bar = charging        ? Rgb{ 0, 170, 255 }
            : pct <= LOW_BATTERY ? Rgb{ 232, 32, 48 }
            : pct <= 30       ? Rgb{ 255, 150, 0 }
                              : Rgb{ 30, 190, 90 };
    Rgb text = (!charging && pct >= 0 && pct <= LOW_BATTERY) ? Rgb{ 240, 50, 60 } : fg;
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray management --------------------------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return L"off / not connected";
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = s.levelText;
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else if (e.lastPct >= 0) t = std::to_wstring(e.lastPct) + L"% (last known)";
    else t = L"unknown";
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.charging) t += L", charging";
    if (s.asleep) t += L", asleep";
    return t;
}

void TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_WARNING;
    }
    Shell_NotifyIconW(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdatePlaceholder() {
    if (g_useWidget) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (auto& [k, e] : g_entries) any |= e.added;
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        TrayCall(NIM_ADD, PLACEHOLDER_ID, g_placeholderIcon, L"Wireless+ Battery\nNo devices with battery info found");
        g_placeholder = true;
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

// ---- lock-key indicators (Caps / Num / Scroll) -----------------------------------------------
// Keyboards without LEDs give no clue that Caps Lock is on. Each enabled lock key gets its own
// tray icon whose brightness follows the toggle state, polled from GetKeyState. They are not
// devices: they stay in the tray even while the taskbar widget is active ("always on"), and
// each can be switched on/off in the menu. Print Screen and Pause/Break have no on/off state,
// so there is nothing to show for them.
struct LockEntry {
    int vk;
    const wchar_t* name;
    const wchar_t* label;    // single glyph drawn in the icon
    const wchar_t* regName;  // registry setting
    bool enabled;
    bool on;
    bool added;
    HICON icon;
    std::wstring sig;
    UINT uid;
};

LockEntry g_locks[] = {
    { VK_CAPITAL, L"Caps Lock",   L"A", L"ShowCapsLock",   true,  false, false, nullptr, L"", 0 },
    { VK_NUMLOCK, L"Num Lock",    L"1", L"ShowNumLock",    false, false, false, nullptr, L"", 0 },
    { VK_SCROLL,  L"Scroll Lock", L"S", L"ShowScrollLock", false, false, false, nullptr, L"", 0 },
};

bool LoadLockEnabled(LockEntry& lk) { return LoadBoolSetting(lk.regName, lk.enabled); }
void SaveLockEnabled(LockEntry& lk) { SaveBoolSetting(lk.regName, lk.enabled); }

void PollLocks() {
    for (auto& lk : g_locks) {
        if (!lk.uid) lk.uid = StableId(std::wstring(L"lock:") + lk.name);
        if (!lk.enabled) {
            if (lk.added) { TrayDelete(lk.uid); lk.added = false; }
            if (lk.icon) { DestroyIcon(lk.icon); lk.icon = nullptr; }
            lk.sig.clear();
            continue;
        }
        bool on = (GetKeyState(lk.vk) & 1) != 0;
        std::wstring sig = std::wstring(on ? L"1" : L"0") + (g_lightTaskbar ? L"l" : L"k");
        if (on != lk.on || sig != lk.sig || !lk.icon) {
            lk.on = on;
            if (lk.icon) DestroyIcon(lk.icon);
            // ON: bright letter, full green bar. OFF: dim letter, faint track only.
            lk.icon = on ? RenderIcon(lk.label, 100, Foreground(), Rgb{ 30, 190, 90 }, false)
                         : RenderIcon(lk.label, -1, Foreground(), Foreground(), true);
            lk.sig = sig;
        }
        std::wstring tip = std::wstring(lk.name) + L": " + (lk.on ? L"ON" : L"OFF");
        TrayCall(lk.added ? NIM_MODIFY : NIM_ADD, lk.uid, lk.icon, tip);
        lk.added = true;
    }
}

// ---- SSH connection indicator ---------------------------------------------------------------
// Same signal as the standalone ssh-indicator.ps1: an SSH session is active when an sshd.exe
// process has a child process (the session shell). Counted, so the tooltip can say how many
// sessions are connected.
bool g_showSsh = true;
bool g_sshOn = false;
bool g_sshAdded = false;
HICON g_sshIcon = nullptr;
std::wstring g_sshSig;
UINT g_sshUid = 0;
int g_sshSessions = 0;

int CountSshSessions() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::set<DWORD> sshd;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"sshd.exe") == 0) sshd.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    int sessions = 0;
    if (!sshd.empty() && Process32FirstW(snap, &pe)) {
        do {
            // Count only real session shells: the per-connection sshd.exe processes are
            // themselves children of sshd.exe, so counting them would inflate the number
            // (1 real session = 2 sshd.exe children + 1 shell).
            if (sshd.count(pe.th32ParentProcessID) && _wcsicmp(pe.szExeFile, L"sshd.exe") != 0) sessions++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return sessions;
}

void PollSsh() {
    if (!g_sshUid) g_sshUid = StableId(L"ssh:indicator");
    if (!g_showSsh) {
        if (g_sshAdded) { TrayDelete(g_sshUid); g_sshAdded = false; }
        if (g_sshIcon) { DestroyIcon(g_sshIcon); g_sshIcon = nullptr; }
        g_sshSig.clear();
        return;
    }
    int n = CountSshSessions();
    bool on = n > 0;
    std::wstring sig = std::to_wstring(n) + (g_lightTaskbar ? L"l" : L"k");
    if (on != g_sshOn || sig != g_sshSig || !g_sshIcon) {
        g_sshOn = on;
        g_sshSessions = n;
        if (g_sshIcon) DestroyIcon(g_sshIcon);
        // Active session: bright "SSH", full green bar. Idle: dim "SSH", faint track only.
        g_sshIcon = on ? RenderIcon(L"SSH", 100, Foreground(), Rgb{ 30, 190, 90 }, false)
                       : RenderIcon(L"SSH", -1, Foreground(), Foreground(), true);
        g_sshSig = sig;
    }
    std::wstring tip = on ? (L"SSH: " + std::to_wstring(n) + (n == 1 ? L" session connected" : L" sessions connected"))
                          : L"SSH: no sessions";
    TrayCall(g_sshAdded ? NIM_MODIFY : NIM_ADD, g_sshUid, g_sshIcon, tip);
    g_sshAdded = true;
}

void ApplyEntry(TrayEntry& e) {
    const auto& s = e.st;
    bool visible = s.connected && !g_useWidget && !IsEntryHidden(e);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    int shown = s.percent >= 0 ? s.percent : e.lastPct;
    bool dim = s.percent < 0;
    std::wstring sig = std::to_wstring(shown) + (s.charging ? L"c" : L"") + (dim ? L"d" : L"") + (g_lightTaskbar ? L"l" : L"k");
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = IconFor(shown, s.charging, dim);
        e.iconSig = sig;
    }
    std::wstring tip = s.name + L"\n" + StatusText(e);

    const wchar_t* infoTitle = nullptr;
    std::wstring info;
    if (s.charging || shown > LOW_BATTERY + 10) e.lowWarned = false;
    if (!s.charging && s.percent >= 0 && s.percent <= LOW_BATTERY && !e.lowWarned) {
        e.lowWarned = true;
        infoTitle = L"Low battery";
        info = s.name + L" is at " + std::to_wstring(s.percent) + L"%";
    }
    TrayCall(e.added ? NIM_MODIFY : NIM_ADD, e.uid, e.icon, tip, infoTitle, info);
    e.added = true;
}

int KindOrder(DeviceKind k) {  // default order: headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        case DeviceKind::Phone:    return 4;
        default:                   return 5;
    }
}

bool IsShown(const TrayEntry& e) { return e.st.connected && !IsEntryHidden(e); }

// Entries in left-to-right order: the user's order first, then the rest by type and name. Works the
// same for every kind of device (receivers, Bluetooth, controllers, HeadsetControl) as it only uses
// the device key.
std::vector<TrayEntry*> OrderedEntries(bool shownOnly) {
    std::vector<TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (!shownOnly || IsShown(e)) list.push_back(&e);
    auto rank = [](const TrayEntry* e) {
        auto it = std::find(g_order.begin(), g_order.end(), e->st.key);
        return it == g_order.end() ? INT_MAX : (int)(it - g_order.begin());
    };
    std::stable_sort(list.begin(), list.end(), [&](const TrayEntry* a, const TrayEntry* b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        return ka != kb ? ka < kb : a->st.name < b->st.name;
    });
    return list;
}

std::wstring PowerStatusText();  // defined with the shutdown timer, below

void UpdateWidget() {
    if (!g_useWidget) return;
    std::vector<TrayEntry*> list = OrderedEntries(true);
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (auto* e : list) {
        WidgetItem it;
        it.kind = e->st.kind;
        it.percent = e->st.percent >= 0 ? e->st.percent : e->lastPct;
        if (e->st.percent >= 0) it.text = e->st.levelText;
        it.charging = e->st.charging;
        it.dim = e->st.percent < 0;
        it.low = !it.charging && it.percent >= 0 && it.percent <= LOW_BATTERY;
        it.bluetooth = e->st.via == L"Bluetooth";
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += e->st.name + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = L"Wireless+ Battery: no devices with battery info found";
    std::wstring power = PowerStatusText();
    if (!power.empty()) tip += L"\n" + power;
    widget::SetItems(items, tip);
}

// ---- shutdown / restart timer -----------------------------------------------------------------
// Schedules a real Windows shutdown/restart through shutdown.exe, so the countdown survives
// the app closing and Windows shows its own sign-out warning. The app only remembers what it
// asked for (persisted in the registry, so a restart of the app restores the countdown text);
// cancelling from anywhere else leaves a stale label, and cancelling that just runs
// `shutdown /a`, which is harmless when nothing is scheduled.
enum class PowerAction : int { None = 0, Shutdown, Restart };
PowerAction g_powerAction = PowerAction::None;
ULONGLONG g_powerAt = 0;  // GetTickCount64() deadline

bool RunShutdownExe(const std::wstring& args) {
    SHELLEXECUTEINFOW ei{ sizeof(ei) };
    ei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    ei.lpVerb = L"open";
    ei.lpFile = L"shutdown.exe";
    ei.lpParameters = args.c_str();
    ei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&ei)) return false;
    if (ei.hProcess) { WaitForSingleObject(ei.hProcess, 10000); CloseHandle(ei.hProcess); }
    return true;
}

void SavePowerState() {
    DWORD action = (DWORD)g_powerAction;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"PowerAction", REG_DWORD, &action, sizeof(action));
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    ULONGLONG deadline = g_powerAction == PowerAction::None ? 0
        : now.QuadPart + (g_powerAt > GetTickCount64() ? (g_powerAt - GetTickCount64()) * 10000ULL : 0);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"PowerDeadline", REG_QWORD, &deadline, sizeof(deadline));
}

void LoadPowerState() {
    DWORD action = 0, sz = sizeof(action);
    ULONGLONG deadline = 0;
    DWORD sz2 = sizeof(deadline);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"PowerAction", RRF_RT_REG_DWORD, nullptr, &action, &sz) != ERROR_SUCCESS ||
        RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"PowerDeadline", RRF_RT_REG_QWORD, nullptr, &deadline, &sz2) != ERROR_SUCCESS)
        return;
    if (action < 1 || action > 2 || !deadline) return;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    if (deadline <= now.QuadPart) return;  // already fired while the app was closed
    g_powerAction = (PowerAction)action;
    g_powerAt = GetTickCount64() + (deadline - now.QuadPart) / 10000ULL;
}

std::wstring PowerStatusText() {
    if (g_powerAction == PowerAction::None) return L"";
    ULONGLONG now = GetTickCount64();
    long remainSec = g_powerAt > now ? (long)((g_powerAt - now) / 1000) : 0;
    std::wstring kind = g_powerAction == PowerAction::Shutdown ? L"Shutdown" : L"Restart";
    return kind + L" in " + std::to_wstring(remainSec / 60) + L"m " + std::to_wstring(remainSec % 60) + L"s";
}

void ShowPowerBalloon(const std::wstring& info) {
    // A balloon needs an existing tray icon; use the SSH indicator, else the Caps Lock one.
    UINT uid = 0;
    HICON icon = nullptr;
    std::wstring tip;
    if (g_sshAdded) {
        uid = g_sshUid;
        icon = g_sshIcon;
        tip = g_sshOn ? (L"SSH: " + std::to_wstring(g_sshSessions) + L" session(s) connected") : L"SSH: no sessions";
    } else if (g_locks[0].added) {
        uid = g_locks[0].uid;
        icon = g_locks[0].icon;
        tip = std::wstring(g_locks[0].name) + L": " + (g_locks[0].on ? L"ON" : L"OFF");
    } else {
        return;
    }
    TrayCall(NIM_MODIFY, uid, icon, tip, L"Wireless+ Battery", info);
}

void SchedulePower(PowerAction action, int minutes) {
    if (g_powerAction != PowerAction::None) RunShutdownExe(L"/a");  // cancel the previous one first
    std::wstring args = (action == PowerAction::Shutdown ? L"/s /t " : L"/r /t ") + std::to_wstring(minutes * 60);
    if (!RunShutdownExe(args)) return;
    g_powerAction = action;
    g_powerAt = GetTickCount64() + (ULONGLONG)minutes * 60 * 1000;
    SavePowerState();
    UpdateWidget();
    ShowPowerBalloon((action == PowerAction::Shutdown ? L"Shutdown scheduled in " : L"Restart scheduled in ") +
                     std::to_wstring(minutes) + L" minutes. Cancel it from the menu.");
}

void CancelPower() {
    if (g_powerAction == PowerAction::None) return;
    RunShutdownExe(L"/a");
    g_powerAction = PowerAction::None;
    g_powerAt = 0;
    SavePowerState();
    UpdateWidget();
}

void OnResults(std::vector<DeviceStatus>& results) {
    std::map<std::wstring, bool> seen;
    for (auto& s : results) {
        auto& e = g_entries[s.key];
        if (!e.uid) e.uid = StableId(s.key);
        e.st = s;
        if (s.percent >= 0) e.lastPct = s.percent;
        e.lastSeen = GetTickCount64();
        seen[s.key] = true;
        ApplyEntry(e);
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        // Some receivers drop sleeping devices from the link; keep showing the last value for a while.
        bool inGrace = !seen.count(it->first) && e.st.graceSec > 0 && e.st.connected &&
                       GetTickCount64() - e.lastSeen < (ULONGLONG)e.st.graceSec * 1000;
        if (inGrace) {
            e.st.percent = -1;
            e.st.asleep = true;
            ApplyEntry(e);
        }
        if (!seen.count(it->first) && !inGrace) {
            if (it->second.added) TrayDelete(it->second.uid);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
    UpdatePlaceholder();
    UpdateWidget();
}

// Makes the actual display match the chosen mode; falls back to tray icons if the taskbar
// widget cannot be created (e.g. Explorer not ready yet). Called once a second.
void Reconcile() {
    bool alive = g_wantWidget && widget::Alive();
    if (g_wantWidget && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget) widget::Destroy();
    bool changed = alive != g_useWidget;
    g_useWidget = alive;
    if (changed) {
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
        UpdateWidget();
    } else if (g_useWidget) {
        widget::Reposition();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void SetHideLastKnown(bool on) {
    g_hideLastKnown = on;
    SaveHideLastKnown(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    for (auto& lk : g_locks) lk.added = false;  // re-added by the next PollLocks
    g_sshAdded = false;  // re-added by the next PollSsh
    UpdatePlaceholder();
}

void RedrawAll() {
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    for (auto& lk : g_locks) lk.sig.clear();  // re-rendered by the next PollLocks
    g_sshSig.clear();  // re-rendered by the next PollSsh
    UpdatePlaceholder();
}

void ToggleHidden(const std::wstring& key) {
    if (!g_hidden.erase(key)) g_hidden.insert(key);
    SaveHidden();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdatePlaceholder();
    UpdateWidget();
}

// Swaps a shown device with its neighbour (dir -1 = left, +1 = right) and remembers the new order.
void MoveEntry(const std::wstring& key, int dir) {
    std::vector<TrayEntry*> list = OrderedEntries(true);
    auto pos = std::find_if(list.begin(), list.end(), [&](const TrayEntry* e) { return e->st.key == key; });
    if (pos == list.end()) return;
    ptrdiff_t i = pos - list.begin(), j = i + dir;
    if (j < 0 || j >= (ptrdiff_t)list.size()) return;
    std::swap(list[i], list[j]);
    std::vector<std::wstring> order;
    for (auto* e : list) order.push_back(e->st.key);
    for (auto& k : g_order)  // keep the places of devices that are not shown right now
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    g_order = order;
    SaveOrder();
    UpdateWidget();
}

void ShowMenu() {
    ApplyMenuTheme(g_lightTaskbar);
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> keys;  // device index -> device key; menu id ID_DEVICE_FIRST + index * DEV_STRIDE + command
    std::vector<TrayEntry*> all = OrderedEntries(false), shown = OrderedEntries(true);
    if (all.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No devices with battery info found");
    else AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Point at a device to show, hide or move it");
    for (TrayEntry* e : all) {
        const std::wstring& k = e->st.key;
        UINT base = ID_DEVICE_FIRST + (UINT)keys.size() * DEV_STRIDE;
        auto pos = std::find(shown.begin(), shown.end(), e);
        bool canLeft = pos != shown.end() && pos != shown.begin();
        bool canRight = pos != shown.end() && pos + 1 != shown.end();
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING | (IsHidden(k) ? 0 : MF_CHECKED), base + DEV_TOGGLE,
                    g_wantWidget ? L"Show on the taskbar" : L"Show in the tray");
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING | (canLeft ? 0 : MF_GRAYED), base + DEV_LEFT, g_wantWidget ? L"Move left" : L"Move up");
        AppendMenuW(sub, MF_STRING | (canRight ? 0 : MF_GRAYED), base + DEV_RIGHT, g_wantWidget ? L"Move right" : L"Move down");
        std::wstring line = e->st.name + L":  " + StatusText(*e) + L"   [" + e->st.via + L"]";
        AppendMenuW(m, MF_POPUP | (IsHidden(k) ? 0 : MF_CHECKED), (UINT_PTR)sub, line.c_str());
        keys.push_back(k);
    }
    AppendMenuW(m, MF_STRING | (g_hideLastKnown ? MF_CHECKED : 0), ID_HIDE_LAST_KNOWN, L"Hide last known / asleep devices");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_locks[0].enabled ? MF_CHECKED : 0), ID_LOCK_CAPS, L"Caps Lock indicator");
    AppendMenuW(m, MF_STRING | (g_locks[1].enabled ? MF_CHECKED : 0), ID_LOCK_NUM, L"Num Lock indicator");
    AppendMenuW(m, MF_STRING | (g_locks[2].enabled ? MF_CHECKED : 0), ID_LOCK_SCROLL, L"Scroll Lock indicator");
    AppendMenuW(m, MF_STRING | (g_showSsh ? MF_CHECKED : 0), ID_SSH_INDICATOR, L"SSH connection indicator");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU shut = CreatePopupMenu();
    AppendMenuW(shut, MF_STRING, ID_PWR_S15, L"in 15 minutes");
    AppendMenuW(shut, MF_STRING, ID_PWR_S30, L"in 30 minutes");
    AppendMenuW(shut, MF_STRING, ID_PWR_S60, L"in 1 hour");
    AppendMenuW(shut, MF_STRING, ID_PWR_S120, L"in 2 hours");
    AppendMenuW(shut, MF_STRING, ID_PWR_S240, L"in 4 hours");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)shut, L"Shutdown timer");
    HMENU rest = CreatePopupMenu();
    AppendMenuW(rest, MF_STRING, ID_PWR_R15, L"in 15 minutes");
    AppendMenuW(rest, MF_STRING, ID_PWR_R30, L"in 30 minutes");
    AppendMenuW(rest, MF_STRING, ID_PWR_R60, L"in 1 hour");
    AppendMenuW(rest, MF_STRING, ID_PWR_R120, L"in 2 hours");
    AppendMenuW(rest, MF_STRING, ID_PWR_R240, L"in 4 hours");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rest, L"Restart timer");
    std::wstring cancelLabel = g_powerAction == PowerAction::None
        ? L"Cancel scheduled shutdown / restart"
        : L"Cancel " + PowerStatusText();
    AppendMenuW(m, MF_STRING | (g_powerAction == PowerAction::None ? MF_GRAYED : 0), ID_PWR_CANCEL, cancelLabel.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_wantWidget ? MF_CHECKED : 0), ID_MODE_WIDGET, L"Show on the taskbar (icon + percentage)");
    AppendMenuW(m, MF_STRING | (!g_wantWidget ? MF_CHECKED : 0), ID_MODE_TRAY, L"Show as tray icons");
    CheckMenuRadioItem(m, ID_MODE_WIDGET, ID_MODE_TRAY, g_wantWidget ? ID_MODE_WIDGET : ID_MODE_TRAY, MF_BYCOMMAND);
    UINT bgFlags = MF_STRING | (g_widgetBackground ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED);
    AppendMenuW(m, bgFlags, ID_WIDGET_BACKGROUND, L"Background behind taskbar text (for transparent taskbars)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"Refresh now");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_REPORT, L"Request support for a device...");
    AppendMenuW(m, MF_STRING, ID_SUPPORT, L"Support the project...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_REFRESH: SetEvent(g_refreshEvt); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_HIDE_LAST_KNOWN: SetHideLastKnown(!g_hideLastKnown); break;
        case ID_LOCK_CAPS: case ID_LOCK_NUM: case ID_LOCK_SCROLL: {
            LockEntry& lk = g_locks[cmd - ID_LOCK_CAPS];
            lk.enabled = !lk.enabled;
            SaveLockEnabled(lk);
            PollLocks();
            break;
        }
        case ID_SSH_INDICATOR:
            g_showSsh = !g_showSsh;
            SaveBoolSetting(L"ShowSshIndicator", g_showSsh);
            PollSsh();
            break;
        case ID_PWR_S15: SchedulePower(PowerAction::Shutdown, 15); break;
        case ID_PWR_S30: SchedulePower(PowerAction::Shutdown, 30); break;
        case ID_PWR_S60: SchedulePower(PowerAction::Shutdown, 60); break;
        case ID_PWR_S120: SchedulePower(PowerAction::Shutdown, 120); break;
        case ID_PWR_S240: SchedulePower(PowerAction::Shutdown, 240); break;
        case ID_PWR_R15: SchedulePower(PowerAction::Restart, 15); break;
        case ID_PWR_R30: SchedulePower(PowerAction::Restart, 30); break;
        case ID_PWR_R60: SchedulePower(PowerAction::Restart, 60); break;
        case ID_PWR_R120: SchedulePower(PowerAction::Restart, 120); break;
        case ID_PWR_R240: SchedulePower(PowerAction::Restart, 240); break;
        case ID_PWR_CANCEL: CancelPower(); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_SUPPORT: ShellExecuteW(nullptr, L"open", L"https://github.com/sponsors/krisgfx9", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_REPORT: report::Show(g_inst, g_hwnd); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size() * DEV_STRIDE) {
                std::wstring key = keys[(cmd - ID_DEVICE_FIRST) / DEV_STRIDE];
                switch ((cmd - ID_DEVICE_FIRST) % DEV_STRIDE) {
                    case DEV_TOGGLE: ToggleHidden(key); break;
                    case DEV_LEFT:   MoveEntry(key, -1); break;
                    case DEV_RIGHT:  MoveEntry(key, +1); break;
                }
            }
    }
}

// ---- worker thread ----------------------------------------------------------------------------

DWORD WINAPI Worker(LPVOID) {
    for (;;) {
        auto* v = new std::vector<DeviceStatus>(PollAllDevices());
        if (!PostMessageW(g_hwnd, WM_RESULTS, 0, (LPARAM)v)) delete v;
        HANDLE hs[2] = { g_stopEvt, g_refreshEvt };
        if (WaitForMultipleObjects(2, hs, FALSE, POLL_INTERVAL_MS) == WAIT_OBJECT_0) return 0;
    }
}

void RefreshSoon(UINT ms) { SetTimer(g_hwnd, TIMER_DEBOUNCE, ms, nullptr); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_RESULTS: {
            auto* v = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            OnResults(*v);
            delete v;
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: ShowMenu(); break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            ShowMenu();
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) RefreshSoon(2500);
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) RefreshSoon(5000);
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { KillTimer(h, TIMER_DEBOUNCE); SetEvent(g_refreshEvt); }
            if (wp == TIMER_WIDGET) { Reconcile(); if (g_powerAction != PowerAction::None) UpdateWidget(); }
            if (wp == TIMER_LOCKS) PollLocks();
            if (wp == TIMER_SSH) PollSsh();
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) { g_lightTaskbar = light; ApplyMenuTheme(light); RedrawAll(); widget::SetLightTheme(light); }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_WIDGET);
            KillTimer(h, TIMER_LOCKS);
            KillTimer(h, TIMER_SSH);
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_worker) WaitForSingleObject(g_worker, 5000);
            for (auto& [k, e] : g_entries) {
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            for (auto& lk : g_locks) {
                if (lk.added) TrayDelete(lk.uid);
                if (lk.icon) DestroyIcon(lk.icon);
            }
            if (g_sshAdded) TrayDelete(g_sshUid);
            if (g_sshIcon) DestroyIcon(g_sshIcon);
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `WirelessBattery.exe --locks` prints the Caps / Num / Scroll toggle states, for
// debugging the lock-key indicators (and so the state can be checked from a console).
int LocksToConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        if (!out || out == INVALID_HANDLE_VALUE) return;
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    const struct { int vk; const wchar_t* name; } keys[] = {
        { VK_CAPITAL, L"Caps Lock" }, { VK_NUMLOCK, L"Num Lock" }, { VK_SCROLL, L"Scroll Lock" },
    };
    for (auto& k : keys)
        write(std::wstring(k.name) + L": " + (((GetKeyState(k.vk) & 1) != 0) ? L"ON\n" : L"OFF\n"));
    write(L"SSH sessions: " + std::to_wstring(CountSshSessions()) + L"\n");
    return 0;
}

// `WirelessBattery.exe --list` prints what it finds, `--dump` every HID collection.
int ListToConsole(bool dump) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (dump) {
        // Also saved to a file: PowerShell does not wait for this windowed exe, so console output can end up
        // after the prompt or get lost, and a double-click has no console at all.
        std::wstring text = DumpHidCollections();
        wchar_t path[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"WirelessBattery-dump.txt");
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::wstring crlf;
            for (wchar_t ch : text) crlf += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
            int n = WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), nullptr, 0, nullptr, nullptr);
            std::string u = "\xEF\xBB\xBF" + std::string(n, 0);  // UTF-8 byte order mark, so Notepad shows it right
            WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), u.data() + 3, n, nullptr, nullptr);
            DWORD w;
            WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
            CloseHandle(f);
        }
        if (out && out != INVALID_HANDLE_VALUE) {
            write(text);
            write(std::wstring(L"\nAlso saved to ") + path + L"\n");
        } else {
            ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);  // no console: show the file
        }
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) write(L"No devices with battery info found.\n");
    for (auto& s : list) {
        std::wstring line = s.name + L"  [" + s.via + L"]  ";
        if (!s.connected) line += L"off / not connected";
        else if (s.asleep) line += L"asleep";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += std::to_wstring(s.percent) + L"%";
        else line += L"unknown";
        if (!s.detail.empty()) line += L" (" + s.detail + L")";
        if (s.charging) line += L", charging";
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        write(line + L"   (" + s.key + L")\n");
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--locks")) return LocksToConsole();
    if (cmdLine && wcsstr(cmdLine, L"--list")) return ListToConsole(false);
    if (cmdLine && wcsstr(cmdLine, L"--dump")) return ListToConsole(true);
    if (cmdLine && wcsstr(cmdLine, L"--report")) {  // the "Request support for a device" window on its own
        report::Show(inst, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (report::PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        if (HWND w = FindWindowW(L"WirelessBatteryWnd", nullptr)) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 50 && FindWindowW(L"WirelessBatteryWnd", nullptr); i++) Sleep(100);
        }
        return 0;
    }
    g_inst = inst;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WirelessBatteryTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_lightTaskbar = ReadLightTaskbar();
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WirelessBatteryWnd";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W filter{ sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wantWidget = LoadWidgetMode();
    g_hideLastKnown = LoadHideLastKnown();
    g_widgetBackground = LoadWidgetBackground();
    for (auto& lk : g_locks) lk.enabled = LoadLockEnabled(lk);
    g_showSsh = LoadBoolSetting(L"ShowSshIndicator", true);
    LoadPowerState();  // restores a shutdown/restart countdown scheduled earlier
    widget::SetBackground(g_widgetBackground);  // remembered by the widget across re-creation
    LoadHidden();
    LoadOrder();
    Reconcile();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    SetTimer(g_hwnd, TIMER_LOCKS, LOCK_POLL_MS, nullptr);
    SetTimer(g_hwnd, TIMER_SSH, SSH_POLL_MS, nullptr);
    PollLocks();  // draw the lock-key icons right away, before the first timer tick
    PollSsh();    // same for the SSH indicator
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (report::PreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<std::vector<DeviceStatus>*>(m.lParam);
    if (notify) UnregisterDeviceNotification(notify);
    if (g_worker) CloseHandle(g_worker);
    ReleaseMutex(mutex);
    return 0;
}
