/* phibat — the battery's draw in watts, as a number in the tray.
 *
 * One file, no config. Every two seconds the battery is asked for its rate
 * (IOCTL_BATTERY_QUERY_STATUS, milliwatts), and the watts are drawn as a tray
 * icon: plain digits, black or white to match the taskbar, green while charging.
 * The tooltip has the exact figure, the charge and the time left. Right-click
 * for "Start with Windows" and "Quit"; a left click opens the battery settings.
 */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <setupapi.h>
#include <batclass.h>
#include <math.h>
#include <stdio.h>

#define WM_TRAY      (WM_APP + 1)
#define ID_AUTOSTART 1
#define ID_QUIT      2
#define TICK_MS      2000
#define IDLE_MW      500      /* below this the battery is neither charging nor draining */

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* Battery device interface class, {72631E54-78A4-11D0-BCF7-00AA00B7B32A}. */
static const GUID battery_guid = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

typedef struct {
    int present;   /* a system battery answered */
    int known;     /* its rate is in milliwatts, not relative units */
    int mw;        /* summed over batteries: > 0 charging, < 0 discharging */
    int pct;       /* 0..100, -1 when unknown */
    int ac;        /* on mains */
    int secs;      /* seconds left on battery, -1 when unknown */
} Bat;

static HWND wnd;
static NOTIFYICONDATAW nid;
static HICON icon;
static int added;
static wchar_t shown_txt[8];
static unsigned shown_rgb;
static UINT taskbar_created;
static int promote_tries = 15;   /* ticks left to find our icon in explorer's list */

/* ---------- reading the battery ---------- */

/* Adds one battery device to *b. */
static void read_one(const wchar_t *path, Bat *b)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n;
    ULONG wait = 0, tag = BATTERY_TAG_INVALID;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof wait, &tag, sizeof tag, &n, NULL)
        || tag == BATTERY_TAG_INVALID) { CloseHandle(h); return; }

    BATTERY_QUERY_INFORMATION q;
    ZeroMemory(&q, sizeof q);
    q.BatteryTag = tag;
    q.InformationLevel = BatteryInformation;
    BATTERY_INFORMATION bi;
    ZeroMemory(&bi, sizeof bi);
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, &bi, sizeof bi, &n, NULL)
        || !(bi.Capabilities & BATTERY_SYSTEM_BATTERY)) { CloseHandle(h); return; }   /* a UPS, say */

    BATTERY_WAIT_STATUS ws;
    ZeroMemory(&ws, sizeof ws);
    ws.BatteryTag = tag;
    BATTERY_STATUS st;
    ZeroMemory(&st, sizeof st);
    if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &ws, sizeof ws, &st, sizeof st, &n, NULL)) {
        b->present = 1;
        if (!(bi.Capabilities & BATTERY_CAPACITY_RELATIVE) && st.Rate != (LONG)BATTERY_UNKNOWN_RATE) {
            b->known = 1;
            b->mw += (int)st.Rate;
        }
    }
    CloseHandle(h);
}

static Bat read_battery(void)
{
    Bat b;
    ZeroMemory(&b, sizeof b);
    b.pct = -1;
    b.secs = -1;

    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        b.ac = sps.ACLineStatus == 1;
        if (sps.BatteryLifePercent <= 100) b.pct = sps.BatteryLifePercent;
        if (sps.BatteryLifeTime != (DWORD)-1) b.secs = (int)sps.BatteryLifeTime;
    }

    HDEVINFO set = SetupDiGetClassDevsW(&battery_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return b;
    SP_DEVICE_INTERFACE_DATA di;
    di.cbSize = sizeof di;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &battery_guid, i, &di); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &di, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *dd = HeapAlloc(GetProcessHeap(), 0, need);
        if (!dd) break;
        dd->cbSize = sizeof *dd;
        if (SetupDiGetDeviceInterfaceDetailW(set, &di, dd, need, NULL, NULL)) read_one(dd->DevicePath, &b);
        HeapFree(GetProcessHeap(), 0, dd);
    }
    SetupDiDestroyDeviceInfoList(set);
    return b;
}

/* ---------- what to show ---------- */

static int light_taskbar(void)
{
    DWORD v = 0, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS) return 0;
    return v == 1;
}

/* The icon's text and color, and the tooltip, for a reading. */
static void describe(const Bat *b, wchar_t *txt, size_t ntxt, unsigned *rgb, wchar_t *tip, size_t ntip)
{
    unsigned fg = light_taskbar() ? 0x000000 : 0xFFFFFF;
    double w = fabs((double)b->mw) / 1000.0;
    wchar_t rest[64] = L"";
    if (b->pct >= 0) _snwprintf(rest, 64, L", %d %%", b->pct);
    if (b->secs >= 0 && b->mw < -IDLE_MW)
        _snwprintf(rest + lstrlenW(rest), 64 - lstrlenW(rest), L", %d:%02d left", b->secs / 3600, b->secs % 3600 / 60);

    *rgb = fg;
    if (!b->present) {
        _snwprintf(txt, ntxt, L"--");
        _snwprintf(tip, ntip, L"phibat — no battery");
    } else if (!b->known) {
        _snwprintf(txt, ntxt, L"?");
        _snwprintf(tip, ntip, L"phibat — this battery does not report its rate%ls", rest);
    } else if (b->mw > IDLE_MW) {
        *rgb = 0x3DDC84;
        _snwprintf(txt, ntxt, w < 9.95 ? L"%.1f" : L"%.0f", w);   /* 5.4, then 12, 45, 140 */
        _snwprintf(tip, ntip, L"phibat — charging at %.1f W%ls", w, rest);
    } else if (b->mw < -IDLE_MW) {
        _snwprintf(txt, ntxt, w < 9.95 ? L"%.1f" : L"%.0f", w);
        _snwprintf(tip, ntip, L"phibat — %.1f W%ls", w, rest);
    } else {
        _snwprintf(txt, ntxt, b->ac ? L"AC" : L"0");
        _snwprintf(tip, ntip, L"phibat — %ls%ls", b->ac ? L"on mains" : L"idle", rest);
    }
    txt[ntxt - 1] = 0;
    tip[ntip - 1] = 0;
}

/* ---------- the icon ---------- */

/* Draws `txt` in `rgb` on a transparent square. */
static HICON make_icon(const wchar_t *txt, unsigned rgb)
{
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    if (size < 16) size = 16;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!dib || !bits) { DeleteDC(dc); return NULL; }
    HGDIOBJ oldbmp = SelectObject(dc, dib);
    ZeroMemory(bits, (size_t)size * size * 4);

    /* As tall as the square allows, shrunk until the text fits its width. */
    int fh = size * 90 / 100;
    HFONT font = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldfont = SelectObject(dc, font);
    SIZE ext;
    if (GetTextExtentPoint32W(dc, txt, lstrlenW(txt), &ext) && ext.cx > size) {
        fh = fh * size / ext.cx;
        SelectObject(dc, oldfont);
        DeleteObject(font);
        font = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SelectObject(dc, font);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));   /* white: the red channel is the coverage */
    RECT rc = { 0, 0, size, size };
    DrawTextW(dc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, bl = rgb & 255;
    unsigned char *px = bits;
    for (int i = 0; i < size * size; i++) {   /* premultiplied: color scaled by coverage */
        unsigned char *p = px + (size_t)i * 4;
        int cov = p[2];
        p[0] = (unsigned char)(bl * cov / 255);
        p[1] = (unsigned char)(g * cov / 255);
        p[2] = (unsigned char)(r * cov / 255);
        p[3] = (unsigned char)cov;
    }

    SelectObject(dc, oldfont);
    DeleteObject(font);
    SelectObject(dc, oldbmp);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, dib };
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(dib);
    return h;
}

static void refresh(void)
{
    Bat b = read_battery();
    wchar_t txt[8], tip[ARRAYSIZE(nid.szTip)];
    unsigned rgb;
    describe(&b, txt, ARRAYSIZE(txt), &rgb, tip, ARRAYSIZE(tip));

    if (added && lstrcmpW(txt, shown_txt) == 0 && rgb == shown_rgb) {
        if (lstrcmpW(tip, nid.szTip) == 0) return;
        lstrcpyW(nid.szTip, tip);
        nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        return;
    }

    HICON h = make_icon(txt, rgb);
    nid.hIcon = h ? h : LoadIconW(NULL, IDI_APPLICATION);
    lstrcpyW(nid.szTip, tip);
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    if (!added) {
        added = Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (icon) DestroyIcon(icon);
    icon = h;
    lstrcpyW(shown_txt, txt);
    shown_rgb = rgb;
}

/* Windows 11 parks every new tray icon in the overflow menu behind the chevron.
 * When this exe's icon first appears, mark it promoted so it sits next to the
 * battery icon. A value already there, set by the user either way, is left alone.
 * Returns 1 once the icon's entry has been seen. */
static int promote(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS) return 0;
    int seen = 0;
    wchar_t name[64];
    for (DWORD i = 0; !seen; i++) {
        DWORD n = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS) continue;
        wchar_t path[MAX_PATH];
        DWORD sz = sizeof path;
        if (RegGetValueW(k, NULL, L"ExecutablePath", RRF_RT_REG_SZ, NULL, path, &sz) == ERROR_SUCCESS
            && lstrcmpiW(path, exe) == 0) {
            DWORD v, vs = sizeof v;
            if (RegGetValueW(k, NULL, L"IsPromoted", RRF_RT_REG_DWORD, NULL, &v, &vs) != ERROR_SUCCESS) {
                v = 1;
                RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
            }
            seen = 1;
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return seen;
}

/* ---------- autostart, menu ---------- */

static int autostart_get(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    LONG r = RegQueryValueExW(k, L"phibat", NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void autostart_set(int on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", exe);
        cmd[ARRAYSIZE(cmd) - 1] = 0;
        RegSetValueExW(k, L"phibat", 0, REG_SZ, (const BYTE *)cmd, (DWORD)((lstrlenW(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"phibat");
    }
    RegCloseKey(k);
}

static void menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (autostart_get() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    SetForegroundWindow(wnd);   /* so the menu closes when the mouse leaves it */
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, wnd, NULL);
    PostMessageW(wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == ID_AUTOSTART) autostart_set(!autostart_get());
    else if (cmd == ID_QUIT) DestroyWindow(wnd);
}

/* ---------- window ---------- */

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == taskbar_created) {   /* explorer restarted: the icon is gone */
        added = 0;
        refresh();
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_CONTEXTMENU) menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
        else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT)
            ShellExecuteW(NULL, L"open", L"ms-settings:batterysaver", NULL, NULL, SW_SHOWNORMAL);
        return 0;
    case WM_TIMER:
        refresh();
        if (added && promote_tries > 0 && promote()) promote_tries = 0;   /* explorer files the icon a moment after NIM_ADD */
        else if (promote_tries > 0) promote_tries--;
        return 0;
    case WM_POWERBROADCAST:
        refresh();
        return 0;
    case WM_SETTINGCHANGE:   /* light/dark taskbar */
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        shown_rgb = ~0u;     /* forces a redraw */
        refresh();
        return 0;
    case WM_DESTROY:
        if (added) Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;
    CreateMutexW(NULL, TRUE, L"phibat-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"phibat";
    RegisterClassW(&wc);
    /* a real (hidden) top-level window: message-only windows miss the broadcasts */
    wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"phibat", L"phibat", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!wnd) return 1;

    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = wnd;
    nid.uID = 1;
    nid.uCallbackMessage = WM_TRAY;
    refresh();
    SetTimer(wnd, 1, TICK_MS, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (icon) DestroyIcon(icon);
    return 0;
}
