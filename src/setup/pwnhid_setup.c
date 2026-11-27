#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <wincrypt.h>
#include <stdio.h>
#include <wchar.h>

static const WCHAR* kHardwareId = L"ROOT\\PWNHID\0";
static const WCHAR* kCertSubject = L"PwnHero Aion2 VirtualHID";

static BOOL InstallCertToStore(const WCHAR* storeName, PCCERT_CONTEXT cert) {
    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
        storeName);
    if (!store) return FALSE;
    BOOL ok = CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_REPLACE_EXISTING, NULL);
    CertCloseStore(store, 0);
    return ok;
}

static BOOL RemoveCertFromStore(const WCHAR* storeName) {
    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
        storeName);
    if (!store) return FALSE;

    PCCERT_CONTEXT ctx = NULL;
    while ((ctx = CertFindCertificateInStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                0, CERT_FIND_SUBJECT_STR_W, kCertSubject, NULL)) != NULL) {
        CertDeleteCertificateFromStore(ctx);
    }
    CertCloseStore(store, 0);
    return TRUE;
}

static BOOL InstallCertificate(const WCHAR* cerPath) {
    PCCERT_CONTEXT cert = NULL;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, cerPath,
                          CERT_QUERY_CONTENT_FLAG_CERT, CERT_QUERY_FORMAT_FLAG_ALL,
                          0, NULL, NULL, NULL, NULL, NULL, (const void**)&cert)) {
        fwprintf(stderr, L"[setup] Failed to read certificate: %s (err=%lu)\n", cerPath, GetLastError());
        return FALSE;
    }
    BOOL ok1 = InstallCertToStore(L"Root", cert);
    BOOL ok2 = InstallCertToStore(L"TrustedPublisher", cert);
    CertFreeCertificateContext(cert);
    return ok1 && ok2;
}

static BOOL DeviceExists(void) {
    HDEVINFO devs = SetupDiGetClassDevsW(NULL, L"ROOT", NULL, DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) return FALSE;

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(devData);
    BOOL found = FALSE;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &devData); ++i) {
        WCHAR hwIds[512];
        if (SetupDiGetDeviceRegistryPropertyW(devs, &devData, SPDRP_HARDWAREID,
                                              NULL, (PBYTE)hwIds, sizeof(hwIds), NULL)) {
            for (const WCHAR* p = hwIds; *p; p += wcslen(p) + 1) {
                if (_wcsicmp(p, L"ROOT\\PWNHID") == 0) {
                    found = TRUE;
                    break;
                }
            }
        }
        if (found) break;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

static int DoInstall(const WCHAR* dir) {
    WCHAR cerPath[MAX_PATH], infPath[MAX_PATH];
    swprintf_s(cerPath, MAX_PATH, L"%s\\pwnhid.cer", dir);
    swprintf_s(infPath, MAX_PATH, L"%s\\pwnhid.inf", dir);

    if (!InstallCertificate(cerPath)) {
        return 1;
    }

    if (!DeviceExists()) {
        GUID sysGuid = {0x4d36e97d, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        HDEVINFO devs = SetupDiCreateDeviceInfoList(&sysGuid, NULL);
        if (devs == INVALID_HANDLE_VALUE) return 1;

        SP_DEVINFO_DATA devData;
        devData.cbSize = sizeof(devData);
        if (!SetupDiCreateDeviceInfoW(devs, L"System", &sysGuid, NULL, NULL, DICD_GENERATE_ID, &devData)) {
            SetupDiDestroyDeviceInfoList(devs);
            return 1;
        }
        if (!SetupDiSetDeviceRegistryPropertyW(devs, &devData, SPDRP_HARDWAREID,
                                               (const BYTE*)kHardwareId,
                                               (DWORD)((wcslen(L"ROOT\\PWNHID") + 2) * sizeof(WCHAR)))) {
            SetupDiDestroyDeviceInfoList(devs);
            return 1;
        }
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devs, &devData)) {
            SetupDiDestroyDeviceInfoList(devs);
            return 1;
        }
        SetupDiDestroyDeviceInfoList(devs);
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, L"ROOT\\PWNHID", infPath, INSTALLFLAG_FORCE, &reboot)) {
        fwprintf(stderr, L"[setup] UpdateDriverForPlugAndPlayDevicesW failed (err=%lu).\n", GetLastError());
        return 1;
    }

    wprintf(L"[setup] PwnHero Virtual HID installed successfully.%s\n",
            reboot ? L" (Reboot recommended)" : L"");
    return 0;
}

static int DoUninstall(void) {
    HDEVINFO devs = SetupDiGetClassDevsW(NULL, L"ROOT", NULL, DIGCF_ALLCLASSES);
    if (devs != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devData;
        devData.cbSize = sizeof(devData);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &devData); ++i) {
            WCHAR hwIds[512];
            if (SetupDiGetDeviceRegistryPropertyW(devs, &devData, SPDRP_HARDWAREID,
                                                  NULL, (PBYTE)hwIds, sizeof(hwIds), NULL)) {
                for (const WCHAR* p = hwIds; *p; p += wcslen(p) + 1) {
                    if (_wcsicmp(p, L"ROOT\\PWNHID") == 0) {
                        BOOL reboot = FALSE;
                        DiUninstallDevice(NULL, devs, &devData, 0, &reboot);
                        break;
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(devs);
    }

    RemoveCertFromStore(L"Root");
    RemoveCertFromStore(L"TrustedPublisher");
    wprintf(L"[setup] PwnHero Virtual HID uninstalled and certificate removed.\n");
    return 0;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        wprintf(L"Usage: pwnhid_setup.exe <install|uninstall> [driver_dir]\n");
        return 1;
    }
    if (_wcsicmp(argv[1], L"install") == 0) {
        WCHAR exeDir[MAX_PATH];
        if (argc >= 3) {
            wcscpy_s(exeDir, MAX_PATH, argv[2]);
        } else {
            GetModuleFileNameW(NULL, exeDir, MAX_PATH);
            WCHAR* slash = wcsrchr(exeDir, L'\\');
            if (slash) *slash = L'\0';
        }
        return DoInstall(exeDir);
    }
    if (_wcsicmp(argv[1], L"uninstall") == 0) {
        return DoUninstall();
    }
    return 1;
}
