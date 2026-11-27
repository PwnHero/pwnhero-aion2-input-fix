#pragma once

#include <windows.h>
#include <winioctl.h>

// {8B4E3C91-5A2F-4D7E-9E14-7C3B2A1F8D05}
DEFINE_GUID(GUID_DEVINTERFACE_PWNHID,
    0x8b4e3c91, 0x5a2f, 0x4d7e, 0x9e, 0x14, 0x7c, 0x3b, 0x2a, 0x1f, 0x8d, 0x05);

#define PWNHID_REPORT_ID_KEYBOARD 1
#define PWNHID_REPORT_ID_MOUSE    2

#define PWNHID_KEYBOARD_DATA_LEN  8
#define PWNHID_MOUSE_DATA_LEN     5

#define IOCTL_PWNHID_SUBMIT_REPORT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#pragma pack(push, 1)
typedef struct _PWNHID_REPORT {
    UCHAR ReportId;
    UCHAR DataLength;
    UCHAR Data[8];
} PWNHID_REPORT, *PPWNHID_REPORT;
#pragma pack(pop)
