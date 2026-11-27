#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>
#include <wchar.h>
#include "pwnhid_ioctl.h"

static HANDLE g_Device = INVALID_HANDLE_VALUE;
static HHOOK  g_KbHook = NULL;
static HHOOK  g_MouseHook = NULL;

static UCHAR g_Modifiers = 0;
static UCHAR g_Keys[6] = {0};
static UCHAR g_MouseButtons = 0;

static HWND  g_LastHwnd = NULL;
static BOOL  g_LastIsAion2 = FALSE;

static HANDLE OpenPwnHidDevice(void) {
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_PWNHID, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);
    if (!SetupDiEnumDeviceInterfaces(devInfo, NULL, &GUID_DEVINTERFACE_PWNHID, 0, &ifData)) {
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }

    DWORD requiredSize = 0;
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &requiredSize, NULL);
    if (requiredSize == 0) {
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }

    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, requiredSize);
    if (!detail) {
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }

    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    HANDLE h = INVALID_HANDLE_VALUE;
    if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, requiredSize, NULL, NULL)) {
        h = CreateFileW(detail->DevicePath, GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }

    HeapFree(GetProcessHeap(), 0, detail);
    SetupDiDestroyDeviceInfoList(devInfo);
    return h;
}

static void SendKeyboardReport(void) {
    if (g_Device == INVALID_HANDLE_VALUE) return;
    PWNHID_REPORT rep;
    ZeroMemory(&rep, sizeof(rep));
    rep.ReportId = PWNHID_REPORT_ID_KEYBOARD;
    rep.DataLength = PWNHID_KEYBOARD_DATA_LEN;
    rep.Data[0] = g_Modifiers;
    rep.Data[1] = 0;
    CopyMemory(&rep.Data[2], g_Keys, 6);
    DWORD bytes = 0;
    DeviceIoControl(g_Device, IOCTL_PWNHID_SUBMIT_REPORT, &rep, sizeof(rep), NULL, 0, &bytes, NULL);
}

static void SendMouseReport(CHAR wheel, CHAR hwheel) {
    if (g_Device == INVALID_HANDLE_VALUE) return;
    PWNHID_REPORT rep;
    ZeroMemory(&rep, sizeof(rep));
    rep.ReportId = PWNHID_REPORT_ID_MOUSE;
    rep.DataLength = PWNHID_MOUSE_DATA_LEN;
    rep.Data[0] = g_MouseButtons;
    rep.Data[1] = 0; // dx
    rep.Data[2] = 0; // dy
    rep.Data[3] = (UCHAR)wheel;
    rep.Data[4] = (UCHAR)hwheel;
    DWORD bytes = 0;
    DeviceIoControl(g_Device, IOCTL_PWNHID_SUBMIT_REPORT, &rep, sizeof(rep), NULL, 0, &bytes, NULL);
}

static void ReleaseAllInput(void) {
    BOOL dirtyKb = (g_Modifiers != 0);
    for (int i = 0; i < 6; ++i) {
        if (g_Keys[i] != 0) dirtyKb = TRUE;
        g_Keys[i] = 0;
    }
    g_Modifiers = 0;
    if (dirtyKb) SendKeyboardReport();

    if (g_MouseButtons != 0) {
        g_MouseButtons = 0;
        SendMouseReport(0, 0);
    }
}

static BOOL IsAion2Foreground(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) {
        if (g_LastIsAion2) ReleaseAllInput();
        g_LastHwnd = NULL;
        g_LastIsAion2 = FALSE;
        return FALSE;
    }
    if (fg == g_LastHwnd) {
        return g_LastIsAion2;
    }

    BOOL match = FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != 0) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            WCHAR path[MAX_PATH];
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
                const WCHAR* slash = wcsrchr(path, L'\\');
                const WCHAR* name = slash ? (slash + 1) : path;
                if (_wcsicmp(name, L"AION2.exe") == 0 ||
                    _wcsicmp(name, L"AION2-Win64-Shipping.exe") == 0) {
                    match = TRUE;
                }
            }
            CloseHandle(proc);
        }
    }

    if (g_LastIsAion2 && !match) {
        ReleaseAllInput();
    }
    g_LastHwnd = fg;
    g_LastIsAion2 = match;
    return match;
}

static UCHAR VkToModifierMask(DWORD vk, DWORD flags) {
    BOOL ext = (flags & LLKHF_EXTENDED) != 0;
    switch (vk) {
        case VK_LCONTROL: return 0x01;
        case VK_LSHIFT:   return 0x02;
        case VK_LMENU:    return 0x04;
        case VK_LWIN:     return 0x08;
        case VK_RCONTROL: return 0x10;
        case VK_RSHIFT:   return 0x20;
        case VK_RMENU:    return 0x40;
        case VK_RWIN:     return 0x80;
        case VK_CONTROL:  return ext ? 0x10 : 0x01;
        case VK_SHIFT:    return 0x02;
        case VK_MENU:     return ext ? 0x40 : 0x04;
        default:          return 0;
    }
}

static UCHAR VkToHidUsage(DWORD vk, DWORD flags) {
    BOOL ext = (flags & LLKHF_EXTENDED) != 0;
    if (vk >= 'A' && vk <= 'Z') return (UCHAR)(0x04 + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (UCHAR)(0x1E + (vk - '1'));
    if (vk == '0') return 0x27;
    if (vk >= VK_F1 && vk <= VK_F12) return (UCHAR)(0x3A + (vk - VK_F1));
    if (vk >= VK_F13 && vk <= VK_F24) return (UCHAR)(0x68 + (vk - VK_F13));
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return (UCHAR)(0x59 + (vk - VK_NUMPAD1));

    switch (vk) {
        case VK_RETURN:    return ext ? 0x58 : 0x28;
        case VK_ESCAPE:    return 0x29;
        case VK_BACK:      return 0x2A;
        case VK_TAB:       return 0x2B;
        case VK_SPACE:     return 0x2C;
        case VK_OEM_MINUS: return 0x2D;
        case VK_OEM_PLUS:  return 0x2E;
        case VK_OEM_4:     return 0x2F; // [
        case VK_OEM_6:     return 0x30; // ]
        case VK_OEM_5:     return 0x31; // backslash
        case VK_OEM_1:     return 0x33; // ;
        case VK_OEM_7:     return 0x34; // '
        case VK_OEM_3:     return 0x35; // `
        case VK_OEM_COMMA: return 0x36;
        case VK_OEM_PERIOD:return 0x37;
        case VK_OEM_2:     return 0x38; // /
        case VK_CAPITAL:   return 0x39;
        case VK_SNAPSHOT:  return 0x46;
        case VK_SCROLL:    return 0x47;
        case VK_PAUSE:     return 0x48;
        case VK_INSERT:    return 0x49;
        case VK_HOME:      return 0x4A;
        case VK_PRIOR:     return 0x4B; // PageUp
        case VK_DELETE:    return 0x4C;
        case VK_END:       return 0x4D;
        case VK_NEXT:      return 0x4E; // PageDown
        case VK_RIGHT:     return 0x4F;
        case VK_LEFT:      return 0x50;
        case VK_DOWN:      return 0x51;
        case VK_UP:        return 0x52;
        case VK_NUMLOCK:   return 0x53;
        case VK_DIVIDE:    return 0x54;
        case VK_MULTIPLY:  return 0x55;
        case VK_SUBTRACT:  return 0x56;
        case VK_ADD:       return 0x57;
        case VK_NUMPAD0:   return 0x62;
        case VK_DECIMAL:   return 0x63;
        case VK_OEM_102:   return 0x64;
        case VK_APPS:      return 0x65;
        default:           return 0;
    }
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;
        if ((kb->flags & LLKHF_INJECTED) && IsAion2Foreground()) {
            BOOL down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            BOOL up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
            if (down || up) {
                UCHAR mod = VkToModifierMask(kb->vkCode, kb->flags);
                if (mod != 0) {
                    if (down) g_Modifiers |= mod;
                    else      g_Modifiers &= (UCHAR)~mod;
                    SendKeyboardReport();
                    return 1;
                }
                UCHAR usage = VkToHidUsage(kb->vkCode, kb->flags);
                if (usage != 0) {
                    if (down) {
                        BOOL found = FALSE;
                        int empty = -1;
                        for (int i = 0; i < 6; ++i) {
                            if (g_Keys[i] == usage) { found = TRUE; break; }
                            if (g_Keys[i] == 0 && empty < 0) empty = i;
                        }
                        if (!found && empty >= 0) g_Keys[empty] = usage;
                    } else {
                        for (int i = 0; i < 6; ++i) {
                            if (g_Keys[i] == usage) g_Keys[i] = 0;
                        }
                    }
                    SendKeyboardReport();
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static CHAR ClampWheel(SHORT delta) {
    int notches = delta / WHEEL_DELTA;
    if (notches == 0 && delta != 0) notches = (delta > 0) ? 1 : -1;
    if (notches > 127) notches = 127;
    if (notches < -127) notches = -127;
    return (CHAR)notches;
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lParam;
        if ((ms->flags & LLMHF_INJECTED) && IsAion2Foreground()) {
            switch (wParam) {
                case WM_MOUSEMOVE:
                    return CallNextHookEx(NULL, nCode, wParam, lParam);
                case WM_LBUTTONDOWN: g_MouseButtons |= 0x01; SendMouseReport(0, 0); return 1;
                case WM_LBUTTONUP:   g_MouseButtons &= (UCHAR)~0x01; SendMouseReport(0, 0); return 1;
                case WM_RBUTTONDOWN: g_MouseButtons |= 0x02; SendMouseReport(0, 0); return 1;
                case WM_RBUTTONUP:   g_MouseButtons &= (UCHAR)~0x02; SendMouseReport(0, 0); return 1;
                case WM_MBUTTONDOWN: g_MouseButtons |= 0x04; SendMouseReport(0, 0); return 1;
                case WM_MBUTTONUP:   g_MouseButtons &= (UCHAR)~0x04; SendMouseReport(0, 0); return 1;
                case WM_XBUTTONDOWN: {
                    WORD xb = HIWORD(ms->mouseData);
                    if (xb == XBUTTON1) g_MouseButtons |= 0x08;
                    if (xb == XBUTTON2) g_MouseButtons |= 0x10;
                    SendMouseReport(0, 0);
                    return 1;
                }
                case WM_XBUTTONUP: {
                    WORD xb = HIWORD(ms->mouseData);
                    if (xb == XBUTTON1) g_MouseButtons &= (UCHAR)~0x08;
                    if (xb == XBUTTON2) g_MouseButtons &= (UCHAR)~0x10;
                    SendMouseReport(0, 0);
                    return 1;
                }
                case WM_MOUSEWHEEL: {
                    CHAR w = ClampWheel((SHORT)HIWORD(ms->mouseData));
                    SendMouseReport(w, 0);
                    return 1;
                }
                case WM_MOUSEHWHEEL: {
                    CHAR hw = ClampWheel((SHORT)HIWORD(ms->mouseData));
                    SendMouseReport(0, hw);
                    return 1;
                }
                default:
                    break;
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    (void)ctrlType;
    ReleaseAllInput();
    PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, 0, 0);
    return TRUE;
}

int wmain(void) {
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\PwnHeroAion2InputBridge");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g_Device = OpenPwnHidDevice();
    if (g_Device == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"[PwnHero Aion2 Bridge] Virtual HID device not found. Run install.ps1 as Administrator first.\n");
        CloseHandle(mutex);
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    g_KbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(NULL), 0);
    g_MouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
    if (!g_KbHook || !g_MouseHook) {
        fwprintf(stderr, L"[PwnHero Aion2 Bridge] Failed to install input hooks (err=%lu).\n", GetLastError());
        if (g_KbHook) UnhookWindowsHookEx(g_KbHook);
        if (g_MouseHook) UnhookWindowsHookEx(g_MouseHook);
        CloseHandle(g_Device);
        CloseHandle(mutex);
        return 1;
    }

    wprintf(L"[PwnHero Aion2 Bridge] Active. Forwarding injected input for AION2.exe.\n");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseAllInput();
    UnhookWindowsHookEx(g_KbHook);
    UnhookWindowsHookEx(g_MouseHook);
    CloseHandle(g_Device);
    CloseHandle(mutex);
    return 0;
}
