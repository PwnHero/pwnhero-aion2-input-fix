#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <wdf.h>
#include <vhf.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include "pwnhid_ioctl.h"

typedef struct _PWNHID_DEVICE_CONTEXT {
    VHFHANDLE   VhfHandle;
    WDFIOTARGET VhfIoTarget;
    WDFWORKITEM StartWorkItem;
    NTSTATUS    StartError;
} PWNHID_DEVICE_CONTEXT, *PPWNHID_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PWNHID_DEVICE_CONTEXT, PwnHidGetContext)

// Combined HID Report Descriptor:
//   Report ID 1: Standard 8-byte Boot Keyboard (with 5-bit LED output report)
//   Report ID 2: Standard 5-byte Relative Mouse (5 buttons, X, Y, Wheel, HWheel)
static UCHAR g_HidReportDescriptor[] = {
    // --- Keyboard (Report ID 1) ---
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, PWNHID_REPORT_ID_KEYBOARD, // Report ID (1)
    0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,       //   Usage Minimum (Left Control)
    0x29, 0xE7,       //   Usage Maximum (Right GUI)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data, Variable, Absolute) - Modifiers
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x01,       //   Input (Constant) - Reserved
    0x95, 0x05,       //   Report Count (5)
    0x75, 0x01,       //   Report Size (1)
    0x05, 0x08,       //   Usage Page (LEDs)
    0x19, 0x01,       //   Usage Minimum (Num Lock)
    0x29, 0x05,       //   Usage Maximum (Kana)
    0x91, 0x02,       //   Output (Data, Variable, Absolute)
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x03,       //   Report Size (3)
    0x91, 0x01,       //   Output (Constant)
    0x95, 0x06,       //   Report Count (6)
    0x75, 0x08,       //   Report Size (8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,       //   Usage Minimum (0)
    0x29, 0xFF,       //   Usage Maximum (255)
    0x81, 0x00,       //   Input (Data, Array) - Keycodes
    0xC0,             // End Collection

    // --- Mouse (Report ID 2) ---
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x85, PWNHID_REPORT_ID_MOUSE, // Report ID (2)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Button)
    0x19, 0x01,       //     Usage Minimum (Button 1)
    0x29, 0x05,       //     Usage Maximum (Button 5)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x95, 0x05,       //     Report Count (5)
    0x75, 0x01,       //     Report Size (1)
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
    0x95, 0x01,       //     Report Count (1)
    0x75, 0x03,       //     Report Size (3)
    0x81, 0x01,       //     Input (Constant) - Padding
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x09, 0x38,       //     Usage (Wheel)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x03,       //     Report Count (3)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0x05, 0x0C,       //     Usage Page (Consumer)
    0x0A, 0x38, 0x02, //     Usage (AC Pan)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x01,       //     Report Count (1)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0xC0,             //   End Collection (Physical)
    0xC0              // End Collection (Application)
};

EVT_WDF_DRIVER_DEVICE_ADD PwnHidEvtDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY PwnHidEvtD0Entry;
EVT_WDF_DEVICE_RELEASE_HARDWARE PwnHidEvtReleaseHardware;
EVT_WDF_WORKITEM PwnHidStartWorkItem;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL PwnHidEvtIoDeviceControl;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, PwnHidEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS
PwnHidEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceD0Entry = PwnHidEvtD0Entry;
    pnpCallbacks.EvtDeviceReleaseHardware = PwnHidEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES deviceAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttrs, PWNHID_DEVICE_CONTEXT);

    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, &deviceAttrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PPWNHID_DEVICE_CONTEXT ctx = PwnHidGetContext(device);
    ctx->VhfHandle = NULL;
    ctx->VhfIoTarget = NULL;
    ctx->StartWorkItem = NULL;
    ctx->StartError = STATUS_PENDING;

    DECLARE_CONST_UNICODE_STRING(symLink, L"\\DosDevices\\PwnHidVhf");
    status = WdfDeviceCreateSymbolicLink(device, &symLink);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_PWNHID, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_WORKITEM_CONFIG wiConfig;
    WDF_WORKITEM_CONFIG_INIT(&wiConfig, PwnHidStartWorkItem);
    WDF_OBJECT_ATTRIBUTES wiAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&wiAttrs);
    wiAttrs.ParentObject = device;
    status = WdfWorkItemCreate(&wiConfig, &wiAttrs, &ctx->StartWorkItem);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = PwnHidEvtIoDeviceControl;

    return WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS
PwnHidEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
    UNREFERENCED_PARAMETER(PreviousState);
    PPWNHID_DEVICE_CONTEXT ctx = PwnHidGetContext(Device);
    if (ctx->VhfHandle == NULL && ctx->StartWorkItem != NULL) {
        WdfWorkItemEnqueue(ctx->StartWorkItem);
    }
    return STATUS_SUCCESS;
}

VOID
PwnHidStartWorkItem(
    _In_ WDFWORKITEM WorkItem
)
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PPWNHID_DEVICE_CONTEXT ctx = PwnHidGetContext(device);
    if (ctx->VhfHandle != NULL) {
        return;
    }

    WDF_OBJECT_ATTRIBUTES targetAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&targetAttrs);
    targetAttrs.ParentObject = device;

    WDFIOTARGET ioTarget = NULL;
    NTSTATUS status = WdfIoTargetCreate(device, &targetAttrs, &ioTarget);
    if (!NT_SUCCESS(status)) {
        ctx->StartError = status;
        return;
    }

    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_FILE(&openParams, NULL);
    status = WdfIoTargetOpen(ioTarget, &openParams);
    if (!NT_SUCCESS(status)) {
        ctx->StartError = status;
        WdfObjectDelete(ioTarget);
        return;
    }

    HANDLE fileHandle = WdfIoTargetWdmGetTargetFileHandle(ioTarget);
    if (fileHandle == NULL || fileHandle == INVALID_HANDLE_VALUE) {
        ctx->StartError = STATUS_INVALID_HANDLE;
        WdfIoTargetClose(ioTarget);
        WdfObjectDelete(ioTarget);
        return;
    }

    VHF_CONFIG vhfConfig;
    VHF_CONFIG_INIT(&vhfConfig, fileHandle, (USHORT)sizeof(g_HidReportDescriptor), g_HidReportDescriptor);
    vhfConfig.VendorID = 0xF055;
    vhfConfig.ProductID = 0xA202;
    vhfConfig.VersionNumber = 0x0100;

    VHFHANDLE vhfHandle = NULL;
    status = VhfCreate(&vhfConfig, &vhfHandle);
    if (!NT_SUCCESS(status)) {
        ctx->StartError = status;
        WdfIoTargetClose(ioTarget);
        WdfObjectDelete(ioTarget);
        return;
    }

    status = VhfStart(vhfHandle);
    if (!NT_SUCCESS(status)) {
        ctx->StartError = status;
        VhfDelete(vhfHandle, TRUE);
        WdfIoTargetClose(ioTarget);
        WdfObjectDelete(ioTarget);
        return;
    }

    ctx->VhfIoTarget = ioTarget;
    ctx->VhfHandle = vhfHandle;
    ctx->StartError = STATUS_SUCCESS;
}

NTSTATUS
PwnHidEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PPWNHID_DEVICE_CONTEXT ctx = PwnHidGetContext(Device);
    if (ctx->VhfHandle != NULL) {
        VhfDelete(ctx->VhfHandle, TRUE);
        ctx->VhfHandle = NULL;
    }
    if (ctx->VhfIoTarget != NULL) {
        WdfIoTargetClose(ctx->VhfIoTarget);
        WdfObjectDelete(ctx->VhfIoTarget);
        ctx->VhfIoTarget = NULL;
    }
    return STATUS_SUCCESS;
}

VOID
PwnHidEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    if (IoControlCode != IOCTL_PWNHID_SUBMIT_REPORT) {
        WDF_REQUEST_SEND_OPTIONS opts;
        WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
        WdfRequestFormatRequestUsingCurrentType(Request);
        if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &opts)) {
            WdfRequestComplete(Request, WdfRequestGetStatus(Request));
        }
        return;
    }

    if (InputBufferLength < sizeof(PWNHID_REPORT)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    PPWNHID_REPORT report = NULL;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(PWNHID_REPORT), (PVOID*)&report, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    BOOLEAN validKeyboard = (report->ReportId == PWNHID_REPORT_ID_KEYBOARD && report->DataLength == PWNHID_KEYBOARD_DATA_LEN);
    BOOLEAN validMouse = (report->ReportId == PWNHID_REPORT_ID_MOUSE && report->DataLength == PWNHID_MOUSE_DATA_LEN);
    if (!validKeyboard && !validMouse) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    PPWNHID_DEVICE_CONTEXT ctx = PwnHidGetContext(WdfIoQueueGetDevice(Queue));
    if (ctx->VhfHandle == NULL) {
        WdfRequestComplete(Request, ctx->StartError != STATUS_SUCCESS ? ctx->StartError : STATUS_DEVICE_NOT_READY);
        return;
    }

    UCHAR rawReport[1 + PWNHID_KEYBOARD_DATA_LEN];
    rawReport[0] = report->ReportId;
    RtlCopyMemory(&rawReport[1], report->Data, report->DataLength);

    HID_XFER_PACKET packet;
    packet.reportId = report->ReportId;
    packet.reportBuffer = rawReport;
    packet.reportBufferLen = (ULONG)(1 + report->DataLength);

    status = VhfReadReportSubmit(ctx->VhfHandle, &packet);
    WdfRequestComplete(Request, status);
}
