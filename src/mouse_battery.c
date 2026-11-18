/*
 * MouseBattery - tiny native Windows tray battery indicator
 * Copyright (c) 2026 uJlu-dev
 * SPDX-License-Identifier: MIT
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define INITGUID

#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <devpkey.h>
#include <devpropdef.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "advapi32.lib")

#define APP_NAME L"Mouse Battery"
#define WM_TRAY (WM_APP + 1)
#define TIMER_REFRESH 1
#define POLL_MS (5u * 60u * 1000u)
#define IDM_REFRESH 1001
#define IDM_AUTOSTART 1002
#define IDM_EXIT 1003
#define HIDPP_VID 0x046D
#define HIDPP_SHORT_REPORT 0x10
#define HIDPP_SHORT_LEN 7
#define HIDPP_LONG_LEN 20
#define HIDPP_SOFTWARE_ID 0x0C

/* Windows publishes peripheral battery percentage under this device property. */
DEFINE_DEVPROPKEY(DEVPKEY_Device_BatteryLevel_Local,
    0x104ea319, 0x6ee2, 0x4701, 0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5, 2);

typedef struct BatteryReading {
    int percent;
    BOOL charging;
    BOOL estimated;
    WCHAR name[96];
} BatteryReading;

typedef struct HidEndpoint {
    HANDLE handle;
    WCHAR path[512];
    USHORT input_len;
    USHORT output_len;
    USHORT pid;
} HidEndpoint;

static HWND g_hwnd;
static HICON g_icon;
static UINT g_taskbar_created;
static int g_percent = -1;
static BOOL g_charging;
static BOOL g_estimated;
static WCHAR g_device_name[96] = L"Беспроводная мышь";

static BOOL guid_equal(const GUID *a, const GUID *b) {
    return memcmp(a, b, sizeof(GUID)) == 0;
}

static BOOL read_container_id(HDEVINFO set, SP_DEVINFO_DATA *dev, GUID *container) {
    DEVPROPTYPE type = 0;
    DWORD size = 0;
    return SetupDiGetDevicePropertyW(set, dev, &DEVPKEY_Device_ContainerId,
        &type, (PBYTE)container, sizeof(*container), &size, 0) &&
        type == DEVPROP_TYPE_GUID;
}

static void read_device_name(HDEVINFO set, SP_DEVINFO_DATA *dev, WCHAR *out, DWORD chars) {
    DWORD type = 0, size = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, SPDRP_FRIENDLYNAME,
            &type, (PBYTE)out, chars * sizeof(WCHAR), &size) &&
        !SetupDiGetDeviceRegistryPropertyW(set, dev, SPDRP_DEVICEDESC,
            &type, (PBYTE)out, chars * sizeof(WCHAR), &size)) {
        lstrcpynW(out, L"Беспроводная мышь", (int)chars);
    }
}

/* Read a battery value already exposed by Windows, and only accept a device in
   the same physical container as a present mouse. */
static BOOL read_windows_mouse_battery(BatteryReading *reading) {
    GUID mouse_containers[24];
    int mouse_count = 0;
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return FALSE;

    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA dev = { sizeof(dev) };
        if (!SetupDiEnumDeviceInfo(set, i, &dev)) break;
        if (guid_equal(&dev.ClassGuid, &GUID_DEVCLASS_MOUSE) && mouse_count < 24) {
            GUID id;
            if (read_container_id(set, &dev, &id)) mouse_containers[mouse_count++] = id;
        }
    }

    BOOL found = FALSE;
    for (DWORD i = 0; !found; ++i) {
        SP_DEVINFO_DATA dev = { sizeof(dev) };
        if (!SetupDiEnumDeviceInfo(set, i, &dev)) break;
        GUID id;
        if (!read_container_id(set, &dev, &id)) continue;
        BOOL belongs_to_mouse = FALSE;
        for (int j = 0; j < mouse_count; ++j) {
            if (guid_equal(&id, &mouse_containers[j])) { belongs_to_mouse = TRUE; break; }
        }
        if (!belongs_to_mouse) continue;

        BYTE value[16] = {0};
        DEVPROPTYPE type = 0;
        DWORD size = 0;
        if (!SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_BatteryLevel_Local,
                &type, value, sizeof(value), &size, 0)) continue;
        int percent = -1;
        if (type == DEVPROP_TYPE_BYTE && size >= 1) percent = value[0];
        else if (type == DEVPROP_TYPE_UINT16 && size >= 2) percent = *(USHORT*)value;
        else if (type == DEVPROP_TYPE_UINT32 && size >= 4) percent = *(ULONG*)value;
        if (percent < 0 || percent > 100) continue;
        reading->percent = percent;
        reading->charging = FALSE;
        reading->estimated = FALSE;
        read_device_name(set, &dev, reading->name, 96);
        found = TRUE;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static void close_endpoint(HidEndpoint *ep) {
    if (ep->handle && ep->handle != INVALID_HANDLE_VALUE) CloseHandle(ep->handle);
    ep->handle = INVALID_HANDLE_VALUE;
}

static BOOL open_hid_path(const WCHAR *path, HidEndpoint *ep) {
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    PHIDP_PREPARSED_DATA pp = NULL;
    HIDP_CAPS caps;
    if (!HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); return FALSE; }
    NTSTATUS status = HidP_GetCaps(pp, &caps);
    HidD_FreePreparsedData(pp);
    if (status != HIDP_STATUS_SUCCESS) { CloseHandle(h); return FALSE; }
    ep->handle = h;
    ep->input_len = caps.InputReportByteLength;
    ep->output_len = caps.OutputReportByteLength;
    lstrcpynW(ep->path, path, 512);
    HidD_SetNumInputBuffers(h, 8);
    return TRUE;
}

static BOOL endpoint_io(HANDLE h, BOOL write, BYTE *buffer, DWORD length,
                        DWORD timeout_ms, DWORD *transferred) {
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return FALSE;
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, buffer, length, &done, &ov)
                    : ReadFile(h, buffer, length, &done, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait == WAIT_OBJECT_0) ok = GetOverlappedResult(h, &ov, &done, FALSE);
        else {
            CancelIoEx(h, &ov);
            WaitForSingleObject(ov.hEvent, 100);
            ok = FALSE;
        }
    }
    if (ok && transferred) *transferred = done;
    CloseHandle(ov.hEvent);
    return ok;
}

static BOOL hid_write_report(HidEndpoint *ep, const BYTE report[HIDPP_SHORT_LEN]) {
    DWORD len = ep->output_len > HIDPP_SHORT_LEN ? ep->output_len : HIDPP_SHORT_LEN;
    if (len > 64) len = HIDPP_SHORT_LEN;
    BYTE packet[64] = {0};
    CopyMemory(packet, report, HIDPP_SHORT_LEN);
    DWORD done = 0;
    return endpoint_io(ep->handle, TRUE, packet, len, 1000, &done);
}

static int hid_read_report(HidEndpoint *ep, BYTE *buffer, DWORD cap, DWORD timeout_ms) {
    DWORD len = ep->input_len;
    if (len < HIDPP_SHORT_LEN) len = HIDPP_SHORT_LEN;
    if (len > cap) len = cap;
    ZeroMemory(buffer, cap);
    DWORD done = 0;
    if (!endpoint_io(ep->handle, FALSE, buffer, len, timeout_ms, &done)) return 0;
    return (int)done;
}

typedef enum AnswerKind { ANSWER_NONE, ANSWER_OK, ANSWER_FAILED } AnswerKind;

/* Send one HID++ request. Replies can arrive on either receiver collection. */
static AnswerKind hidpp_request(HidEndpoint *short_ep, HidEndpoint *long_ep,
                                BYTE slot, BYTE feature, BYTE function,
                                BYTE p0, BYTE p1, BYTE *data, int *data_len) {
    BYTE request[HIDPP_SHORT_LEN] = {
        HIDPP_SHORT_REPORT, slot, feature,
        (BYTE)((function << 4) | HIDPP_SOFTWARE_ID), p0, p1, 0
    };
    HidD_FlushQueue(short_ep->handle);
    HidD_FlushQueue(long_ep->handle);
    if (!hid_write_report(short_ep, request)) return ANSWER_NONE;

    ULONGLONG deadline = GetTickCount64() + 1700;
    BYTE reply[64];
    while (GetTickCount64() < deadline) {
        HidEndpoint *eps[2] = { long_ep, short_ep };
        DWORD waits[2] = { 50, 10 };
        for (int e = 0; e < 2; ++e) {
            int n = hid_read_report(eps[e], reply, sizeof(reply), waits[e]);
            if (n < HIDPP_SHORT_LEN || reply[1] != slot) continue;
            if (reply[2] == 0x8F && reply[3] == feature && reply[4] == request[3])
                return ANSWER_FAILED;
            if (reply[2] == 0xFF && reply[3] == feature && reply[4] == request[3])
                return ANSWER_FAILED;
            if (reply[2] == feature && reply[3] == request[3]) {
                int count = n - 4;
                if (count > *data_len) count = *data_len;
                CopyMemory(data, reply + 4, count);
                *data_len = count;
                return ANSWER_OK;
            }
        }
    }
    return ANSWER_NONE;
}

static int voltage_percent(USHORT mv) {
    static const USHORT volts[] = {4186,4067,3989,3922,3859,3811,3778,3751,3717,3671,3646,3579,3500};
    static const BYTE pct[] = {100,90,80,70,60,50,40,30,20,10,5,2,0};
    if (mv >= volts[0]) return 100;
    for (int i = 0; i < 12; ++i) {
        if (mv >= volts[i + 1]) {
            DWORD span = (DWORD)(pct[i] - pct[i + 1]) * (mv - volts[i + 1]);
            return pct[i + 1] + (int)(span / (volts[i] - volts[i + 1]));
        }
    }
    return 0;
}

static BOOL is_supported_logitech_pid(USHORT pid) {
    static const USHORT pids[] = {0xC539,0xC53A,0xC53D,0xC53F,0xC541,0xC545,0xC547,0xC54D,0xC52B,0xC532,0xC548};
    for (unsigned i = 0; i < sizeof(pids)/sizeof(pids[0]); ++i) if (pid == pids[i]) return TRUE;
    return FALSE;
}

static BOOL find_logitech_endpoints(HidEndpoint *short_ep, HidEndpoint *long_ep) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return FALSE;
    BOOL have_short = FALSE, have_long = FALSE;
    USHORT chosen_pid = 0;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA iface = { sizeof(iface) };
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &hid_guid, i, &iface)) break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, NULL, 0, &needed, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)LocalAlloc(LPTR, needed);
        if (!detail) continue;
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, NULL, NULL)) {
            HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attr = { sizeof(attr) };
                PHIDP_PREPARSED_DATA pp = NULL;
                HIDP_CAPS caps;
                BOOL candidate = FALSE;
                if (HidD_GetAttributes(h, &attr) && attr.VendorID == HIDPP_VID &&
                    is_supported_logitech_pid(attr.ProductID) && HidD_GetPreparsedData(h, &pp)) {
                    if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 0xFF00 &&
                        (caps.Usage == 1 || caps.Usage == 2)) candidate = TRUE;
                    HidD_FreePreparsedData(pp);
                }
                CloseHandle(h);
                if (candidate && (!chosen_pid || chosen_pid == attr.ProductID)) {
                    HidEndpoint temp = { INVALID_HANDLE_VALUE };
                    if (open_hid_path(detail->DevicePath, &temp)) {
                        temp.pid = attr.ProductID;
                        if (caps.Usage == 1 && !have_short) {
                            *short_ep = temp; have_short = TRUE; chosen_pid = attr.ProductID;
                        } else if (caps.Usage == 2 && !have_long) {
                            *long_ep = temp; have_long = TRUE; chosen_pid = attr.ProductID;
                        } else close_endpoint(&temp);
                    }
                }
            }
        }
        LocalFree(detail);
        if (have_short && have_long) break;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!(have_short && have_long)) {
        if (have_short) close_endpoint(short_ep);
        if (have_long) close_endpoint(long_ep);
        return FALSE;
    }
    return TRUE;
}

static BOOL read_logitech_battery(BatteryReading *reading) {
    HidEndpoint short_ep = { INVALID_HANDLE_VALUE }, long_ep = { INVALID_HANDLE_VALUE };
    if (!find_logitech_endpoints(&short_ep, &long_ep)) return FALSE;

    static const USHORT features[] = {0x1004, 0x1000, 0x1001, 0x1F20};
    BOOL found = FALSE;
    for (BYTE slot = 1; slot <= 6 && !found; ++slot) {
        for (int f = 0; f < 4 && !found; ++f) {
            BYTE data[32] = {0}; int len = sizeof(data);
            AnswerKind ans = hidpp_request(&short_ep, &long_ep, slot, 0, 0,
                (BYTE)(features[f] >> 8), (BYTE)features[f], data, &len);
            if (ans != ANSWER_OK || len < 1 || data[0] == 0) continue;
            BYTE feature_index = data[0];
            len = sizeof(data); ZeroMemory(data, sizeof(data));
            BYTE function = (features[f] == 0x1004) ? 1 : 0;
            ans = hidpp_request(&short_ep, &long_ep, slot, feature_index, function,
                0, 0, data, &len);
            if (ans != ANSWER_OK || len < 3) continue;
            int percent = -1; BOOL charging = FALSE, estimated = FALSE;
            if (features[f] == 0x1004) {
                percent = data[0]; charging = data[2] >= 1 && data[2] <= 4;
                if (percent == 0) {
                    percent = (data[1] & 8) ? 100 : (data[1] & 4) ? 50 : (data[1] & 2) ? 20 : (data[1] & 1) ? 5 : -1;
                    estimated = TRUE;
                }
            } else if (features[f] == 0x1000) {
                if (data[0]) percent = data[0];
                charging = data[2] >= 1 && data[2] <= 4;
            } else {
                if (features[f] == 0x1F20 && !(data[2] & 1)) continue;
                percent = voltage_percent((USHORT)((data[0] << 8) | data[1]));
                charging = features[f] == 0x1001 ? !!(data[2] & 0x80) : !!(data[2] & 2);
                estimated = TRUE;
            }
            if (percent >= 0 && percent <= 100) {
                reading->percent = percent;
                reading->charging = charging;
                reading->estimated = estimated;
                lstrcpynW(reading->name, short_ep.pid == 0xC53F ? L"Logitech G305 SE" : L"Logitech LIGHTSPEED", 96);
                found = TRUE;
            }
        }
    }
    close_endpoint(&short_ep);
    close_endpoint(&long_ep);
    return found;
}

static BOOL read_mouse_battery(BatteryReading *reading) {
    ZeroMemory(reading, sizeof(*reading));
    reading->percent = -1;
    if (read_windows_mouse_battery(reading)) return TRUE;
    return read_logitech_battery(reading);
}

static HICON make_icon(int percent, BOOL charging) {
    const int s = 32;
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bitmap = {0};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = s;
    bitmap.bmiHeader.biHeight = -s;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    DWORD *pixels = NULL;
    HBITMAP color = CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, NULL);
    if (!dc || !color || !mask) {
        if (mask) DeleteObject(mask);
        if (color) DeleteObject(color);
        if (dc) DeleteDC(dc);
        ReleaseDC(NULL, screen);
        return NULL;
    }
    HGDIOBJ old = SelectObject(dc, color);
    DWORD light = 0x42be86, dark = 0x08754b;
    if (percent < 0) { light = 0x77818e; dark = 0x404854; }
    else if (!charging && percent <= 15) { light = 0xe65e60; dark = 0xa12231; }
    else if (!charging && percent <= 35) { light = 0xb87922; dark = 0x784207; }
    const int span = 2 * (s - 1);
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            DWORD rgb = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                int hi = (light >> shift) & 255, lo = (dark >> shift) & 255;
                rgb |= (DWORD)((hi * (span - x - y) + lo * (x + y)) / span) << shift;
            }
            pixels[y * s + x] = rgb;
        }
    }
    RECT r = {0, 0, s, s};
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255));
    WCHAR text[8];
    if (percent < 0) lstrcpyW(text, L"?"); else wsprintfW(text, L"%d", percent);
    int height = percent >= 100 ? 17 : 22;
    HFONT font = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldfont = SelectObject(dc, font);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldfont); DeleteObject(font); SelectObject(dc, old);
    GdiFlush();
    /* Supersample the rounded corners and premultiply RGB for the tray. */
    const int radius = 8, samples = 4, edge = radius * samples;
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            int near_x = x < s / 2 ? x : s - 1 - x;
            int near_y = y < s / 2 ? y : s - 1 - y;
            int alpha = 255;
            if (near_x < radius && near_y < radius) {
                int inside = 0;
                for (int j = 0; j < samples; ++j) {
                    for (int i = 0; i < samples; ++i) {
                        int dx = 2 * (edge - near_x * samples - i) - 1;
                        int dy = 2 * (edge - near_y * samples - j) - 1;
                        if (dx * dx + dy * dy <= 4 * edge * edge) ++inside;
                    }
                }
                alpha = inside * 255 / (samples * samples);
            }
            DWORD rgb = pixels[y * s + x], argb = (DWORD)alpha << 24;
            for (int shift = 0; shift <= 16; shift += 8)
                argb |= (((rgb >> shift) & 255) * alpha / 255) << shift;
            pixels[y * s + x] = argb;
        }
    }

    HDC maskdc = CreateCompatibleDC(screen);
    HGDIOBJ oldmask = SelectObject(maskdc, mask);
    PatBlt(maskdc, 0, 0, s, s, BLACKNESS);
    SelectObject(maskdc, oldmask); DeleteDC(maskdc);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(color); DeleteDC(dc); ReleaseDC(NULL, screen);
    return icon;
}

static BOOL add_or_update_tray(BOOL add) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid); nid.hWnd = g_hwnd; nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY; nid.hIcon = g_icon;
    if (g_percent >= 0) {
        wsprintfW(nid.szTip, g_estimated ? L"%s: ~%d%%%s" : L"%s: %d%%%s",
            g_device_name, g_percent, g_charging ? L" (заряжается)" : L"");
    } else lstrcpyW(nid.szTip, L"Мышь: заряд недоступен");
    BOOL ok = Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
    if (add && ok) { nid.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &nid); }
    return ok;
}

static BOOL autostart_enabled(void) {
    WCHAR value[1024]; DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"MouseBattery", RRF_RT_REG_SZ, NULL, value, &size) == ERROR_SUCCESS;
}

static void toggle_autostart(void) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    if (autostart_enabled()) RegDeleteValueW(key, L"MouseBattery");
    else {
        WCHAR path[MAX_PATH], command[MAX_PATH + 4];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wsprintfW(command, L"\"%s\"", path);
        RegSetValueExW(key, L"MouseBattery", 0, REG_SZ, (BYTE*)command,
            ((DWORD)lstrlenW(command) + 1) * sizeof(WCHAR));
    }
    RegCloseKey(key);
}

static void refresh_battery(void) {
    BatteryReading r;
    if (read_mouse_battery(&r)) {
        g_percent = r.percent; g_charging = r.charging; g_estimated = r.estimated;
        lstrcpynW(g_device_name, r.name, 96);
    } else g_percent = -1;
    HICON next = make_icon(g_percent, g_charging);
    if (!next) return;
    HICON old = g_icon; g_icon = next;
    /* Explorer can be starting or restarting while the app launches. If an
       update says the icon is absent, add it again with the current value. */
    if (!add_or_update_tray(FALSE)) add_or_update_tray(TRUE);
    if (old) DestroyIcon(old);
}

static HWND g_panel;
static const WCHAR panel_class[] = L"MouseBatteryPanel";
static int panel_width = 288, panel_height = 210;
static int panel_dpi = 96;

static int px(int value) { return MulDiv(value, panel_dpi, 96); }

static const COLORREF panel_bg = RGB(30, 37, 48);

static COLORREF blend_color(COLORREF front, COLORREF back, int coverage) {
    return RGB((GetRValue(front) * coverage + GetRValue(back) * (16 - coverage)) / 16,
               (GetGValue(front) * coverage + GetGValue(back) * (16 - coverage)) / 16,
               (GetBValue(front) * coverage + GetBValue(back) * (16 - coverage)) / 16);
}

static void panel_box(HDC dc, RECT r, int diameter, COLORREF color, COLORREF background) {
    int radius = min(px(diameter) / 2, min((r.right-r.left)/2, (r.bottom-r.top)/2));
    SetDCBrushColor(dc, color);
    RECT middle = {r.left + radius, r.top, r.right - radius, r.bottom};
    RECT left = {r.left, r.top + radius, r.left + radius, r.bottom - radius};
    RECT right = {r.right - radius, r.top + radius, r.right, r.bottom - radius};
    FillRect(dc, &middle, GetStockObject(DC_BRUSH));
    FillRect(dc, &left, GetStockObject(DC_BRUSH));
    FillRect(dc, &right, GetStockObject(DC_BRUSH));
    for (int corner = 0; corner < 4; ++corner) {
        for (int y = 0; y < radius; ++y) {
            for (int x = 0; x < radius; ++x) {
                int coverage = 0;
                for (int j = 1; j < 8; j += 2) {
                    for (int i = 1; i < 8; i += 2) {
                        int dx = 8 * (radius - x) - i;
                        int dy = 8 * (radius - y) - j;
                        coverage += dx * dx + dy * dy <= 64 * radius * radius;
                    }
                }
                SetPixelV(dc, corner & 1 ? r.right - 1 - x : r.left + x,
                    corner & 2 ? r.bottom - 1 - y : r.top + y,
                    blend_color(color, background, coverage));
            }
        }
    }
}

static BOOL near_segment(double x, double y, double ax, double ay, double bx, double by) {
    double vx = bx - ax, vy = by - ay;
    double t = ((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy);
    t = max(0.0, min(1.0, t));
    double dx = x - ax - t * vx, dy = y - ay - t * vy;
    return dx * dx + dy * dy <= 0.85 * 0.85;
}

static void panel_refresh_arrow(HDC dc, int left, int top, COLORREF background) {
    const int size = px(18);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int coverage = 0;
            for (int j = 1; j < 8; j += 2) {
                for (int i = 1; i < 8; i += 2) {
                    double dx = 18.0 * (x + i / 8.0) / size - 9;
                    double dy = 18.0 * (y + j / 8.0) / size - 9;
                    double rr = dx * dx + dy * dy;
                    BOOL arc = rr >= 4.65 * 4.65 && rr <= 6.35 * 6.35 &&
                        (dy >= 0 || dx <= 0 || dx <= -dy);
                    BOOL head = near_segment(dx, dy, 3.89, -3.89, 0.1, -3.89) ||
                        near_segment(dx, dy, 3.89, -3.89, 3.89, -7.68);
                    coverage += arc || head;
                }
            }
            if (coverage) SetPixelV(dc, left + x, top + y,
                blend_color(RGB(101, 223, 164), background, coverage));
        }
    }
}

static void panel_text(HDC dc, RECT r, const WCHAR *text, int size, int weight, COLORREF color) {
    HFONT font = CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = font ? SelectObject(dc, font) : NULL;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (font) { SelectObject(dc, old); DeleteObject(font); }
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static const WCHAR *labels[] = { L"Refresh now", L"Start with Windows", L"Exit" };
    static const int commands[] = { IDM_REFRESH, IDM_AUTOSTART, IDM_EXIT };
    switch (msg) {
    case WM_CREATE:
        for (int i = 0; i < 3; ++i)
            CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                px(12), px(78 + 40 * i), px(264), px(36), hwnd,
                (HMENU)(INT_PTR)commands[i], GetModuleHandleW(NULL), NULL);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        g_panel = NULL;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT bounds = {0, 0, panel_width, panel_height};
        SetDCBrushColor(dc, panel_bg);
        FillRect(dc, &bounds, GetStockObject(DC_BRUSH));
        RECT card = {px(12), px(12), px(276), px(68)};
        panel_box(dc, card, 12, RGB(42, 52, 66), panel_bg);
        if (g_icon) DrawIconEx(dc, px(22), px(24), g_icon, px(32), px(32), 0, NULL, DI_NORMAL);
        RECT title = {px(66), px(18), px(262), px(40)};
        RECT subtitle = {px(66), px(39), px(262), px(61)};
        WCHAR status[80];
        if (g_percent < 0) lstrcpyW(status, L"Battery unavailable");
        else wsprintfW(status, g_estimated ? L"Battery level: ≈%d%%" : L"Battery level: %d%%", g_percent);
        panel_text(dc, title, g_percent < 0 ? L"Wireless mouse" : g_device_name,
            15, FW_SEMIBOLD, RGB(245, 248, 255));
        panel_text(dc, subtitle, status, 13, FW_NORMAL, RGB(170, 182, 200));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lp;
        RECT r = item->rcItem;
        HDC dc = item->hDC;
        SetDCBrushColor(dc, panel_bg);
        FillRect(dc, &r, GetStockObject(DC_BRUSH));
        COLORREF row = panel_bg;
        if (item->itemState & (ODS_FOCUS | ODS_SELECTED)) {
            row = RGB(47, 60, 76);
            panel_box(dc, r, 9, row, panel_bg);
        }
        RECT label = r;
        label.left += px(10);
        int index = (int)item->CtlID - IDM_REFRESH;
        if (index < 0 || index >= 3) return FALSE;
        panel_text(dc, label, labels[index], 14, FW_NORMAL, RGB(233, 238, 247));
        if (item->CtlID == IDM_AUTOSTART) {
            BOOL on = autostart_enabled();
            RECT track = {r.right - px(49), px(8), r.right - px(9), px(28)};
            COLORREF track_color = on ? RGB(26, 144, 94) : RGB(68, 81, 99);
            panel_box(dc, track, 20, track_color, row);
            int diameter = (track.bottom - track.top) - 2 * px(3);
            int left = on ? track.right - px(3) - diameter : track.left + px(3);
            int top = track.top + px(3);
            RECT knob = {left, top, left + diameter, top + diameter};
            panel_box(dc, knob, 14, RGB(240, 246, 252), track_color);
        } else if (item->CtlID == IDM_REFRESH) {
            panel_refresh_arrow(dc, r.right - px(31), (r.bottom - px(18)) / 2, row);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_EXIT) {
            DestroyWindow(hwnd);
            PostMessageW(g_hwnd, WM_COMMAND, IDM_EXIT, 0);
        } else if (LOWORD(wp) == IDM_AUTOSTART) {
            toggle_autostart();
            InvalidateRect(GetDlgItem(hwnd, IDM_AUTOSTART), NULL, FALSE);
        } else if (LOWORD(wp) == IDM_REFRESH) {
            PostMessageW(g_hwnd, WM_COMMAND, IDM_REFRESH, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_menu(void) {
    if (g_panel) { SetForegroundWindow(g_panel); return; }
    POINT anchor;
    GetCursorPos(&anchor);
    NOTIFYICONIDENTIFIER id = {sizeof(id)};
    id.hWnd = g_hwnd;
    id.uID = 1;
    RECT icon;
    BOOL located = SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon));
    if (located) { anchor.x = icon.right; anchor.y = icon.top; }
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return;
    panel_dpi = GetDpiForWindow(g_hwnd);
    if (!panel_dpi) panel_dpi = 96;
    panel_width = px(288);
    panel_height = px(210);
    RECT work = info.rcWork;
    int x = max(work.left, min(anchor.x - panel_width, work.right - panel_width));
    int y = anchor.y - panel_height - px(6);
    if (y < work.top && located) y = icon.bottom + px(6);
    y = max(work.top, min(y, work.bottom - panel_height));
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, panel_class, APP_NAME,
        WS_POPUP, x, y, panel_width, panel_height, g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_panel) return;
    HRGN region = CreateRoundRectRgn(0, 0, panel_width, panel_height, px(18), px(18));
    if (region && !SetWindowRgn(g_panel, region, FALSE)) DeleteObject(region);
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
    if (g_panel) SetFocus(GetDlgItem(g_panel, IDM_REFRESH));
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbar_created) { add_or_update_tray(TRUE); return 0; }
    switch (msg) {
    case WM_CREATE:
        /* WM_CREATE runs before CreateWindowEx returns, so the global is not yet
           assigned by wWinMain. Shell_NotifyIcon needs the real window handle. */
        g_hwnd = hwnd;
        g_icon = make_icon(-1, FALSE); add_or_update_tray(TRUE);
        SetTimer(hwnd, TIMER_REFRESH, 200, NULL); return 0;
    case WM_TIMER:
        KillTimer(hwnd, TIMER_REFRESH); refresh_battery();
        if (g_panel) InvalidateRect(g_panel, NULL, FALSE);
        SetTimer(hwnd, TIMER_REFRESH, POLL_MS, NULL); return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_RBUTTONUP ||
            LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) show_menu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_REFRESH) { KillTimer(hwnd, TIMER_REFRESH); refresh_battery(); SetTimer(hwnd, TIMER_REFRESH, POLL_MS, NULL); }
        else if (LOWORD(wp) == IDM_AUTOSTART) toggle_autostart();
        else if (LOWORD(wp) == IDM_EXIT) DestroyWindow(hwnd);
        if (g_panel) InvalidateRect(g_panel, NULL, FALSE);
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid = { sizeof(nid) }; nid.hWnd = hwnd; nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid); if (g_icon) DestroyIcon(g_icon); PostQuitMessage(0); return 0;
    }}
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#ifdef PROBE
int wmain(void) {
    BatteryReading r;
    if (read_mouse_battery(&r)) {
        wprintf(L"%ls\npercent=%d\ncharging=%d\nestimated=%d\n", r.name, r.percent, r.charging, r.estimated);
        return 0;
    }
    wprintf(L"battery unavailable\n");
    return 2;
}
#else
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    (void)previous; (void)command; (void)show;
    SetProcessDPIAware();
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\MouseBattery.Singleton");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = L"MouseBatteryHiddenWindow";
    if (!RegisterClassW(&wc)) return 1;
    wc.lpfnWndProc = panel_proc;
    wc.lpszClassName = panel_class;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassW(&wc)) return 1;
    wc.lpszClassName = L"MouseBatteryHiddenWindow";
    /* A normal hidden top-level window is the most compatible notification-area owner. */
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, 0, 0,0,0,0, NULL, NULL, instance, NULL);
    if (!g_hwnd) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_panel && (msg.hwnd == g_panel || IsChild(g_panel, msg.hwnd))) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                DestroyWindow(g_panel);
                continue;
            }
            if (IsDialogMessageW(g_panel, &msg)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(mutex);
    return 0;
}
#endif
