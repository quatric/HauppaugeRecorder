/*
 * hdpvr-driver: puts a libusb-compatible driver on the HD PVR, the way
 * Zadig/libwdi do, so hdpvr-capture can open it.
 *
 * The HD PVR streams over bulk transfers, which WinUSB handles on every
 * Windows from 7 on, so WinUSB is used whenever Windows ships winusb.inf.
 * libusbK is the fallback for a Windows 7 without it (or with --libusbk).
 * The HD PVR has a single interface, so the driver goes on the device itself.
 *
 * The driver package (INF + catalog) is generated here and its catalog is
 * signed with a certificate made on this machine for this purpose only. The
 * certificate is trusted for code signing on this machine, and its private key
 * is deleted right after signing so nothing else can ever be signed with it.
 * Uninstalling hands the device back to whatever driver Windows picks next,
 * usually Hauppauge's own if it is installed.
 *
 *   hdpvr-driver [install] [--winusb | --libusbk] [--quiet]
 *   hdpvr-driver uninstall [--quiet]
 *   hdpvr-driver status
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <wincrypt.h>
#include <mssip.h>
#include <mscat.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "known_devices.h"

#define APP_NAME      L"HauppaugeRecorder"
#define REG_KEY       L"SOFTWARE\\HauppaugeRecorder"
#define CERT_CN       L"HauppaugeRecorder local driver certificate"
#define IFACE_GUID    "{3B0E6F2A-8C4D-4A71-9E25-7D1C5B8A6F40}"
#define WDF_COINST    L"WdfCoInstaller01009.dll"

#ifndef INSTALLFLAG_FORCE
#define INSTALLFLAG_FORCE 0x00000001
#endif
#ifndef SUOI_FORCEDELETE
#define SUOI_FORCEDELETE 0x00000001
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define INF_ARCH "NTarm64"
#define HAVE_LIBUSBK 0
#elif defined(__x86_64__) || defined(_M_X64)
#define INF_ARCH "NTamd64"
#define HAVE_LIBUSBK 1
#else
#define INF_ARCH "NTx86"
#define HAVE_LIBUSBK 1
#endif

static int g_quiet;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void fail(const char *what)
{
    DWORD e = GetLastError();
    char msg[512] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof(msg), NULL);
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\r' || msg[n - 1] == '\n'))
        msg[--n] = 0;
    say("error: %s failed: 0x%08lx %s\n", what, (unsigned long)e, msg);
}

/* ---- environment ---- */

static int is_elevated(void)
{
    HANDLE tok;
    TOKEN_ELEVATION el = { 0 };
    DWORD len;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return 0;
    int ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &len) && el.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

static int is_wow64(void)
{
    BOOL w = FALSE;
    typedef BOOL(WINAPI * fn_t)(HANDLE, PBOOL);
    fn_t f = (fn_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    return f && f(GetCurrentProcess(), &w) && w;
}

static void os_version(DWORD *major, DWORD *minor)
{
    /* GetVersionEx lies to unmanifested programs; ask ntdll */
    typedef LONG(WINAPI * fn_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW v = { sizeof(v) };
    fn_t f = (fn_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (f && f(&v) == 0) {
        *major = v.dwMajorVersion;
        *minor = v.dwMinorVersion;
    } else {
        *major = 6;
        *minor = 1;
    }
}

/* The generated INF includes Windows' own winusb.inf; use WinUSB when it is there. */
static int winusb_available(void)
{
    HINF h = SetupOpenInfFileW(L"winusb.inf", NULL, INF_STYLE_WIN4, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    INFCONTEXT c;
    int ok = SetupFindFirstLineW(h, L"WINUSB.NT", NULL, &c) && SetupFindFirstLineW(h, L"WINUSB.NT.Services", NULL, &c);
    SetupCloseInfFile(h);
    return ok;
}

static void exe_dir(wchar_t *out, size_t len)
{
    GetModuleFileNameW(NULL, out, (DWORD)len);
    wchar_t *s = wcsrchr(out, L'\\');
    if (s)
        *s = 0;
}

/* Re-runs this program elevated with the same arguments and returns its exit code. */
static int relaunch_elevated(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    const wchar_t *cmd = GetCommandLineW(), *args = cmd;
    /* skip argv[0] */
    if (*args == L'"') {
        args = wcschr(args + 1, L'"');
        args = args ? args + 1 : L"";
    } else {
        while (*args && *args != L' ')
            args++;
    }
    while (*args == L' ')
        args++;
    SHELLEXECUTEINFOW se = { sizeof(se) };
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    se.lpVerb = L"runas";
    se.lpFile = exe;
    se.lpParameters = args;
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se) || !se.hProcess) {
        say("Administrator rights are needed to install a driver.\n");
        return 1;
    }
    WaitForSingleObject(se.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(se.hProcess, &code);
    CloseHandle(se.hProcess);
    return (int)code;
}

/* ---- devices ---- */

typedef struct {
    wchar_t hwid[128];   /* the hardware ID to install on: the device node */
    wchar_t instance[256];
    wchar_t service[64];
    const char *name;
} target_t;

/* Matches "USB\VID_xxxx&PID_xxxx..." against the known devices; *mi is the interface number or -1. */
static int match_hwid(const wchar_t *id, int *index, int *mi)
{
    unsigned vid, pid;
    if (_wcsnicmp(id, L"USB\\VID_", 8) || swscanf(id + 8, L"%4x&PID_%4x", &vid, &pid) != 2)
        return 0;
    for (size_t i = 0; i < KNOWN_DEVICE_COUNT; i++)
        if (known_devices[i].vid == vid && known_devices[i].pid == pid) {
            const wchar_t *m = wcsstr(id, L"&MI_");
            if (!m)
                m = wcsstr(id, L"&mi_");
            *index = (int)i;
            *mi = m ? (int)wcstol(m + 4, NULL, 16) : -1;
            return 1;
        }
    return 0;
}

/* Finds the plugged-in HD PVRs. */
static int find_targets(target_t *t, int max)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return 0;
    int n = 0;
    SP_DEVINFO_DATA d = { sizeof(d) };
    for (DWORD i = 0; n < max && SetupDiEnumDeviceInfo(set, i, &d); i++) {
        wchar_t ids[2048] = { 0 };
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof(ids) - 4, NULL))
            continue;
        int idx, mi;
        if (!match_hwid(ids, &idx, &mi) || mi > 0)
            continue;
        target_t x = { 0 };
        x.name = known_devices[idx].name;
        /* the second ID drops REV_: "USB\VID_xxxx&PID_xxxx" */
        const wchar_t *second = ids + wcslen(ids) + 1;
        wcsncpy(x.hwid, *second ? second : ids, 127);
        SetupDiGetDeviceInstanceIdW(set, &d, x.instance, 255, NULL);
        SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_SERVICE, NULL, (BYTE *)x.service, sizeof(x.service) - 2, NULL);
        t[n++] = x;
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

/* ---- driver package ---- */

static int write_file(const wchar_t *path, const char *data)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    DWORD w;
    BOOL ok = WriteFile(h, data, (DWORD)strlen(data), &w, NULL);
    CloseHandle(h);
    return ok && w == strlen(data);
}

static void append(char *buf, size_t len, const char *fmt, ...)
{
    size_t used = strlen(buf);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, len - used, fmt, ap);
    va_end(ap);
}

/* The models section: every known HD PVR, plus any plugged-in one with an ID not listed. */
static void append_models(char *inf, size_t len, const target_t *t, int nt)
{
    append(inf, len, "[Manufacturer]\r\n\"HauppaugeRecorder\" = Devices," INF_ARCH "\r\n\r\n[Devices." INF_ARCH "]\r\n");
    for (size_t i = 0; i < KNOWN_DEVICE_COUNT; i++)
        append(inf, len, "\"%s\" = Install, USB\\VID_%04X&PID_%04X\r\n", known_devices[i].name, known_devices[i].vid,
               known_devices[i].pid);
    for (int i = 0; i < nt; i++)
        if (wcsstr(t[i].hwid, L"&MI_"))
            append(inf, len, "\"%s\" = Install, %ls\r\n", t[i].name, t[i].hwid);
    append(inf, len, "\r\n");
}

static void make_inf(char *inf, size_t len, int libusbk, const target_t *t, int nt)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    inf[0] = 0;
    if (libusbk) {
        append(inf, len,
               "; libusbK driver for the Hauppauge HD PVR, generated by hdpvr-driver\r\n"
               "[Version]\r\nSignature = \"$Windows NT$\"\r\nClass = libusbKDevices\r\n"
               "ClassGuid = {ECFB0CFD-74C4-4f52-BBF7-343461CD72AC}\r\nProvider = \"HauppaugeRecorder\"\r\n"
               "CatalogFile = hdpvr.cat\r\nDriverVer = %02d/%02d/%04d,3.1.0.0\r\n\r\n"
               "[ClassInstall32]\r\nAddReg = Class_AddReg\r\n\r\n"
               "[Class_AddReg]\r\nHKR,,,0,\"libusbK USB Devices\"\r\nHKR,,Icon,,-20\r\n\r\n",
               st.wMonth, st.wDay, st.wYear);
        append_models(inf, len, t, nt);
        append(inf, len,
               "[Install]\r\nCopyFiles = Drivers_CopyFiles\r\n\r\n"
               "[Install.HW]\r\nAddReg = Dev_AddReg\r\n\r\n"
               "[Dev_AddReg]\r\nHKR,,DeviceInterfaceGUIDs,0x10000,\"" IFACE_GUID "\"\r\n\r\n"
               "[Install.Services]\r\nAddService = libusbK, 0x00000002, Service_Install\r\n\r\n"
               "[Service_Install]\r\nDisplayName = \"libusbK USB driver\"\r\nServiceType = 1\r\nStartType = 3\r\n"
               "ErrorControl = 1\r\nServiceBinary = %%12%%\\libusbK.sys\r\n\r\n"
               "[Install.CoInstallers]\r\nAddReg = CoInstallers_AddReg\r\nCopyFiles = CoInstallers_CopyFiles\r\n\r\n"
               "[CoInstallers_AddReg]\r\nHKR,,CoInstallers32,0x00010000,\"WdfCoInstaller01009.dll,WdfCoInstaller\"\r\n\r\n"
               "[Install.Wdf]\r\nKmdfService = libusbK, Wdf_Install\r\n\r\n"
               "[Wdf_Install]\r\nKmdfLibraryVersion = 1.9\r\n\r\n"
               "[DestinationDirs]\r\nDrivers_CopyFiles = 12\r\nCoInstallers_CopyFiles = 11\r\n\r\n"
               "[Drivers_CopyFiles]\r\nlibusbK.sys\r\n\r\n"
               "[CoInstallers_CopyFiles]\r\nWdfCoInstaller01009.dll\r\n\r\n"
               "[SourceDisksNames]\r\n1 = \"HauppaugeRecorder driver\",,,\r\n\r\n"
               "[SourceDisksFiles]\r\nlibusbK.sys = 1\r\nWdfCoInstaller01009.dll = 1\r\n");
    } else {
        append(inf, len,
               "; WinUSB driver for the Hauppauge HD PVR, generated by hdpvr-driver\r\n"
               "[Version]\r\nSignature = \"$Windows NT$\"\r\nClass = USBDevice\r\n"
               "ClassGuid = {88BAE032-5A81-49f0-BC3D-A4FF138216D6}\r\nProvider = \"HauppaugeRecorder\"\r\n"
               "CatalogFile = hdpvr.cat\r\nDriverVer = %02d/%02d/%04d,1.0.0.0\r\n\r\n",
               st.wMonth, st.wDay, st.wYear);
        append_models(inf, len, t, nt);
        append(inf, len,
               "[Install]\r\nInclude = winusb.inf\r\nNeeds = WINUSB.NT\r\n\r\n"
               "[Install.Services]\r\nInclude = winusb.inf\r\nNeeds = WINUSB.NT.Services\r\n\r\n"
               "[Install.HW]\r\nAddReg = Dev_AddReg\r\n\r\n"
               "[Dev_AddReg]\r\nHKR,,DeviceInterfaceGUIDs,0x10000,\"" IFACE_GUID "\"\r\n");
    }
}

/* ---- catalog and its signature (as libwdi's pki.c) ---- */

static int cat_add_file(HANDLE cat, const wchar_t *path, const wchar_t *name)
{
    int ok = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        fail("open file for catalog");
        return 0;
    }
    BYTE hash[64];
    DWORD hlen = sizeof(hash);
    GUID subject;
    SIP_DISPATCH_INFO sip = { sizeof(sip) };
    SIP_INDIRECT_DATA *ind = NULL;
    DWORD ilen = 0;
    if (!CryptCATAdminCalcHashFromFileHandle(f, &hlen, hash, 0)) {
        fail("hash file");
        goto out;
    }
    wchar_t tag[129];
    for (DWORD i = 0; i < hlen && i < 64; i++)
        swprintf(tag + 2 * i, 3, L"%02X", hash[i]);
    if (!CryptSIPRetrieveSubjectGuidForCatalogFile(path, f, &subject) || !CryptSIPLoad(&subject, 0, &sip)) {
        fail("load SIP");
        goto out;
    }
    SIP_SUBJECTINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.pgSubjectType = &subject;
    si.hFile = f;
    si.pwsFileName = path;
    si.DigestAlgorithm.pszObjId = (LPSTR)szOID_OIWSEC_sha1;
    if (!sip.pfCreate(&si, &ilen, NULL) || !(ind = (SIP_INDIRECT_DATA *)LocalAlloc(LPTR, ilen)) ||
        !sip.pfCreate(&si, &ilen, ind)) {
        fail("create indirect data");
        goto out;
    }
    CRYPTCATMEMBER *m = CryptCATPutMemberInfo(cat, NULL, tag, &subject, 0x200, ilen, (BYTE *)ind);
    const DWORD fl = CRYPTCAT_ATTR_AUTHENTICATED | CRYPTCAT_ATTR_NAMEASCII | CRYPTCAT_ATTR_DATAASCII;
    static wchar_t osattr[] = L"2:6.1,2:6.2,2:6.3,2:10.0";
    if (!m || !CryptCATPutAttrInfo(cat, m, (WCHAR *)L"File", fl, (DWORD)(wcslen(name) + 1) * 2, (BYTE *)name) ||
        !CryptCATPutAttrInfo(cat, m, (WCHAR *)L"OSAttr", fl, sizeof(osattr), (BYTE *)osattr)) {
        fail("add catalog member");
        goto out;
    }
    ok = 1;
out:
    if (ind)
        LocalFree(ind);
    CloseHandle(f);
    return ok;
}

static int make_catalog(const wchar_t *dir, const wchar_t *cat_path, int libusbk)
{
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        fail("CryptAcquireContext");
        return 0;
    }
    DeleteFileW(cat_path);
    HANDLE cat = CryptCATOpen((LPWSTR)cat_path, CRYPTCAT_OPEN_CREATENEW, prov, 0, 0);
    int ok = cat != INVALID_HANDLE_VALUE;
    if (!ok)
        fail("CryptCATOpen");
    static wchar_t os[] = L"7X86,7X64,8X86,8X64,8ARM,_v63,_v63_X64,_v63_ARM,_v100,_v100_X64,_v100_ARM64";
    if (ok && !CryptCATPutCatAttrInfo(cat, (WCHAR *)L"OS",
                                      CRYPTCAT_ATTR_AUTHENTICATED | CRYPTCAT_ATTR_NAMEASCII | CRYPTCAT_ATTR_DATAASCII,
                                      sizeof(os), (BYTE *)os)) {
        fail("set catalog OS attribute");
        ok = 0;
    }
    const wchar_t *files[] = { L"hdpvr.inf", L"libusbK.sys", WDF_COINST };
    int nfiles = libusbk ? 3 : 1;
    for (int i = 0; ok && i < nfiles; i++) {
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%ls\\%ls", dir, files[i]);
        /* catalog member names are lower case */
        wchar_t lower[64];
        wcsncpy(lower, files[i], 63);
        lower[63] = 0;
        CharLowerW(lower);
        ok = cat_add_file(cat, p, lower);
    }
    if (ok && !CryptCATPersistStore(cat)) {
        fail("CryptCATPersistStore");
        ok = 0;
    }
    if (cat != INVALID_HANDLE_VALUE)
        CryptCATClose(cat);
    CryptReleaseContext(prov, 0);
    return ok;
}

/* mssign32.dll's SignerSignEx; declared here as MinGW has no header for it */
typedef struct { DWORD cbSize; LPCWSTR pwszFileName; HANDLE hFile; } SIGNER_FILE_INFO;
typedef struct { DWORD cbSize; DWORD *pdwIndex; DWORD dwSubjectChoice; SIGNER_FILE_INFO *pSignerFileInfo; } SIGNER_SUBJECT_INFO;
typedef struct { DWORD cbSize; PCCERT_CONTEXT pSigningCert; DWORD dwCertPolicy; HCERTSTORE hCertStore; } SIGNER_CERT_STORE_INFO;
typedef struct { DWORD cbSize; DWORD dwCertChoice; SIGNER_CERT_STORE_INFO *pCertStoreInfo; HWND hwnd; } SIGNER_CERT;
typedef struct { DWORD cbSize; ALG_ID algidHash; DWORD dwAttrChoice; void *pAttrAuthcode; PCRYPT_ATTRIBUTES psAuthenticated; PCRYPT_ATTRIBUTES psUnauthenticated; } SIGNER_SIGNATURE_INFO;
typedef struct { DWORD cbSize; DWORD cbBlob; BYTE *pbBlob; } SIGNER_CONTEXT;
typedef HRESULT(WINAPI *SignerSignEx_t)(DWORD, SIGNER_SUBJECT_INFO *, SIGNER_CERT *, SIGNER_SIGNATURE_INFO *, void *,
                                        LPCWSTR, PCRYPT_ATTRIBUTES, LPVOID, SIGNER_CONTEXT **);
typedef HRESULT(WINAPI *SignerFreeSignerContext_t)(SIGNER_CONTEXT *);
#define SIGNER_SUBJECT_FILE 1
#define SIGNER_CERT_STORE 2
#define SIGNER_CERT_POLICY_CHAIN 2
#define SIGNER_NO_ATTR 0

static void remove_our_certs(void)
{
    const wchar_t *stores[] = { L"Root", L"TrustedPublisher" };
    for (int s = 0; s < 2; s++) {
        HCERTSTORE st = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, stores[s]);
        if (!st)
            continue;
        PCCERT_CONTEXT c;
        while ((c = CertFindCertificateInStore(st, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, CERT_CN, NULL)))
            CertDeleteCertificateFromStore(c); /* frees c */
        CertCloseStore(st, 0);
    }
}

static int trust_cert(PCCERT_CONTEXT cert)
{
    /* a copy without the key-provider property, so the stores hold the certificate only */
    PCCERT_CONTEXT bare = CertCreateCertificateContext(X509_ASN_ENCODING, cert->pbCertEncoded, cert->cbCertEncoded);
    if (!bare)
        return 0;
    const wchar_t *stores[] = { L"Root", L"TrustedPublisher" };
    int ok = 1;
    for (int s = 0; s < 2; s++) {
        HCERTSTORE st = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, stores[s]);
        if (!st || !CertAddCertificateContextToStore(st, bare, CERT_STORE_ADD_REPLACE_EXISTING, NULL)) {
            fail("add certificate to the machine store");
            ok = 0;
        }
        if (st)
            CertCloseStore(st, 0);
    }
    CertFreeCertificateContext(bare);
    return ok;
}

/* Makes a one-off code-signing certificate, signs the catalog with it, trusts it, deletes its key. */
static int sign_catalog(const wchar_t *cat_path, int sha256)
{
    wchar_t container[64];
    swprintf(container, 64, L"HauppaugeRecorder-driver-%08lx", (unsigned long)GetTickCount());
    HCRYPTPROV prov = 0;
    HCRYPTKEY key = 0;
    PCCERT_CONTEXT cert = NULL;
    int ok = 0;
    BYTE name[256], eku_buf[64];
    DWORD name_len = sizeof(name), eku_len = sizeof(eku_buf);

    if (!CryptAcquireContextW(&prov, container, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                              CRYPT_NEWKEYSET | CRYPT_MACHINE_KEYSET)) {
        fail("create key container");
        return 0;
    }
    if (!CryptGenKey(prov, AT_SIGNATURE, 2048u << 16, &key)) {
        fail("generate key");
        goto out;
    }
    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=" CERT_CN, CERT_X500_NAME_STR, NULL, name, &name_len, NULL)) {
        fail("CertStrToName");
        goto out;
    }
    LPSTR usages[] = { (LPSTR)szOID_PKIX_KP_CODE_SIGNING };
    CERT_ENHKEY_USAGE eku = { 1, usages };
    if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, eku_buf, &eku_len)) {
        fail("encode key usage");
        goto out;
    }
    CERT_EXTENSION ext = { (LPSTR)szOID_ENHANCED_KEY_USAGE, FALSE, { eku_len, eku_buf } };
    CERT_EXTENSIONS exts = { 1, &ext };
    CERT_NAME_BLOB subject = { name_len, name };
    CRYPT_KEY_PROV_INFO kpi = { container, (LPWSTR)MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_MACHINE_KEYSET, 0, NULL,
                                AT_SIGNATURE };
    CRYPT_ALGORITHM_IDENTIFIER alg = { (LPSTR)(sha256 ? szOID_RSA_SHA256RSA : szOID_RSA_SHA1RSA), { 0, NULL } };
    SYSTEMTIME end;
    GetSystemTime(&end);
    end.wYear += 20;
    if (end.wMonth == 2 && end.wDay == 29)
        end.wDay = 28;
    cert = CertCreateSelfSignCertificate(prov, &subject, 0, &kpi, &alg, NULL, &end, &exts);
    if (!cert) {
        fail("create certificate");
        goto out;
    }

    HMODULE ms = LoadLibraryW(L"mssign32.dll");
    SignerSignEx_t sign = ms ? (SignerSignEx_t)(void *)GetProcAddress(ms, "SignerSignEx") : NULL;
    SignerFreeSignerContext_t free_ctx =
        ms ? (SignerFreeSignerContext_t)(void *)GetProcAddress(ms, "SignerFreeSignerContext") : NULL;
    if (!sign) {
        fail("load mssign32.dll");
        goto out;
    }
    DWORD index = 0;
    SIGNER_FILE_INFO fi = { sizeof(fi), cat_path, NULL };
    SIGNER_SUBJECT_INFO subj = { sizeof(subj), &index, SIGNER_SUBJECT_FILE, &fi };
    SIGNER_CERT_STORE_INFO csi = { sizeof(csi), cert, SIGNER_CERT_POLICY_CHAIN, NULL };
    SIGNER_CERT sc = { sizeof(sc), SIGNER_CERT_STORE, &csi, NULL };
    SIGNER_SIGNATURE_INFO sig = { sizeof(sig), sha256 ? CALG_SHA_256 : CALG_SHA1, SIGNER_NO_ATTR, NULL, NULL, NULL };
    SIGNER_CONTEXT *ctx = NULL;
    HRESULT hr = sign(0, &subj, &sc, &sig, NULL, NULL, NULL, NULL, &ctx);
    if (ctx && free_ctx)
        free_ctx(ctx);
    if (FAILED(hr)) {
        SetLastError((DWORD)hr);
        fail("sign catalog");
        goto out;
    }
    remove_our_certs();
    ok = trust_cert(cert);
out:
    if (cert)
        CertFreeCertificateContext(cert);
    if (key)
        CryptDestroyKey(key);
    if (prov)
        CryptReleaseContext(prov, 0);
    /* the private key is never needed again */
    HCRYPTPROV gone;
    CryptAcquireContextW(&gone, container, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET | CRYPT_MACHINE_KEYSET);
    return ok;
}

/* ---- registry: which staged INF is ours ---- */

static void reg_set(const wchar_t *name, const wchar_t *val)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_KEY, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &k, NULL))
        return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)(wcslen(val) + 1) * 2);
    RegCloseKey(k);
}

static int reg_get(const wchar_t *name, wchar_t *out, DWORD len)
{
    HKEY k;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_KEY, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k))
        return 0;
    DWORD sz = len * 2, type;
    int ok = !RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &sz) && type == REG_SZ;
    RegCloseKey(k);
    return ok && out[0];
}

static void unstage_previous(void)
{
    wchar_t prev[MAX_PATH];
    if (reg_get(L"DriverInf", prev, MAX_PATH)) {
        if (SetupUninstallOEMInfW(prev, SUOI_FORCEDELETE, NULL))
            say("Removed the earlier driver package (%ls).\n", prev);
    }
}

/* ---- commands ---- */

static int copy_into(const wchar_t *src_dir, const wchar_t *dst_dir, const wchar_t *name)
{
    wchar_t a[MAX_PATH], b[MAX_PATH];
    swprintf(a, MAX_PATH, L"%ls\\%ls", src_dir, name);
    swprintf(b, MAX_PATH, L"%ls\\%ls", dst_dir, name);
    if (!CopyFileW(a, b, FALSE)) {
        say("error: cannot copy %ls (from %ls)\n", name, src_dir);
        return 0;
    }
    return 1;
}

static int cmd_install(int force_winusb, int force_libusbk)
{
    DWORD major, minor;
    os_version(&major, &minor);
    int libusbk = HAVE_LIBUSBK && !winusb_available();
    if (force_winusb)
        libusbk = 0;
    if (force_libusbk && HAVE_LIBUSBK)
        libusbk = 1;
    int sha256 = major > 6 || (major == 6 && minor >= 2);
    say("Windows %lu.%lu: using %s.\n", (unsigned long)major, (unsigned long)minor,
        libusbk ? "libusbK" : "WinUSB");

    target_t t[16];
    int nt = find_targets(t, 16);
    for (int i = 0; i < nt; i++)
        say("Found %s: %ls (driver: %ls)\n", t[i].name, t[i].hwid, t[i].service[0] ? t[i].service : L"none");

    /* %ProgramData%\HauppaugeRecorder\driver */
    wchar_t dir[MAX_PATH], base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, base))) {
        fail("find ProgramData");
        return 1;
    }
    swprintf(dir, MAX_PATH, L"%ls\\" APP_NAME, base);
    CreateDirectoryW(dir, NULL);
    wcscat(dir, L"\\driver");
    CreateDirectoryW(dir, NULL);

    if (libusbk) {
        wchar_t src[MAX_PATH];
        exe_dir(src, MAX_PATH);
        wcscat(src, L"\\driver\\libusbK");
        if (!copy_into(src, dir, L"libusbK.sys") || !copy_into(src, dir, WDF_COINST))
            return 1;
    }

    static char inf[16384];
    make_inf(inf, sizeof(inf), libusbk, t, nt);
    wchar_t inf_path[MAX_PATH], cat_path[MAX_PATH];
    swprintf(inf_path, MAX_PATH, L"%ls\\hdpvr.inf", dir);
    swprintf(cat_path, MAX_PATH, L"%ls\\hdpvr.cat", dir);
    if (!write_file(inf_path, inf)) {
        fail("write INF");
        return 1;
    }
    say("Signing the driver package...\n");
    if (!make_catalog(dir, cat_path, libusbk) || !sign_catalog(cat_path, sha256))
        return 1;

    unstage_previous();
    wchar_t oem[MAX_PATH] = L"";
    if (!SetupCopyOEMInfW(inf_path, NULL, SPOST_PATH, 0, oem, MAX_PATH, NULL, NULL)) {
        fail("add the driver package to Windows");
        return 1;
    }
    const wchar_t *oem_name = wcsrchr(oem, L'\\') ? wcsrchr(oem, L'\\') + 1 : oem;
    reg_set(L"DriverInf", oem_name);
    reg_set(L"Driver", libusbk ? L"libusbK" : L"WinUSB");
    say("Driver package added (%ls).\n", oem_name);

    if (!nt) {
        say("No HD PVR is plugged in. Plug it in and it should pick up this driver; if it gets "
            "Hauppauge's driver instead, run this again with the HD PVR plugged in.\n");
        return 2;
    }

    typedef BOOL(WINAPI * update_t)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
    HMODULE nd = LoadLibraryW(L"newdev.dll");
    update_t update = nd ? (update_t)(void *)GetProcAddress(nd, "UpdateDriverForPlugAndPlayDevicesW") : NULL;
    if (!update) {
        fail("load newdev.dll");
        return 1;
    }
    int done = 0, reboot_any = 0;
    for (int i = 0; i < nt; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            dup |= !_wcsicmp(t[i].hwid, t[j].hwid);
        if (dup)
            continue;
        say("Installing on %s (%ls)...\n", t[i].name, t[i].hwid);
        BOOL reboot = FALSE;
        if (update(NULL, t[i].hwid, inf_path, INSTALLFLAG_FORCE, &reboot)) {
            done++;
            reboot_any |= reboot;
        } else {
            fail("install the driver on the device");
        }
    }
    if (!done)
        return 1;
    say("Done. The HD PVR now uses %s.%s\n", libusbk ? "libusbK" : "WinUSB",
        reboot_any ? " Restart Windows to finish." : "");
    return 0;
}

static int cmd_uninstall(void)
{
    wchar_t oem[MAX_PATH];
    if (!reg_get(L"DriverInf", oem, MAX_PATH)) {
        say("No HauppaugeRecorder driver package is installed.\n");
        remove_our_certs();
        return 0;
    }
    /* take our driver off the devices that have it, then let Windows pick again */
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    typedef BOOL(WINAPI * uninst_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
    HMODULE nd = LoadLibraryW(L"newdev.dll");
    uninst_t uninst = nd ? (uninst_t)(void *)GetProcAddress(nd, "DiUninstallDevice") : NULL;
    SP_DEVINFO_DATA d = { sizeof(d) };
    for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInfo(set, i, &d); i++) {
        wchar_t ids[2048] = { 0 };
        int idx, mi;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof(ids) - 4, NULL) ||
            !match_hwid(ids, &idx, &mi))
            continue;
        HKEY k = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
        if (k == INVALID_HANDLE_VALUE)
            continue;
        wchar_t infpath[MAX_PATH] = L"";
        DWORD sz = sizeof(infpath) - 2, type;
        RegQueryValueExW(k, L"InfPath", NULL, &type, (BYTE *)infpath, &sz);
        RegCloseKey(k);
        if (_wcsicmp(infpath, oem))
            continue;
        BOOL reboot = FALSE;
        say("Removing the driver from %s...\n", known_devices[idx].name);
        if (uninst ? !uninst(NULL, set, &d, 0, &reboot) : !SetupDiRemoveDevice(set, &d))
            fail("remove device");
    }
    if (set != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(set);
    if (!SetupUninstallOEMInfW(oem, SUOI_FORCEDELETE, NULL))
        fail("remove the driver package");
    remove_our_certs();
    reg_set(L"DriverInf", L"");
    DEVINST root;
    if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
    say("The HauppaugeRecorder driver was removed.\n");
    return 0;
}

static int cmd_status(void)
{
    target_t t[16];
    int nt = find_targets(t, 16);
    wchar_t oem[MAX_PATH], drv[32];
    if (reg_get(L"DriverInf", oem, MAX_PATH))
        say("Installed package: %ls (%ls)\n", oem, reg_get(L"Driver", drv, 32) ? drv : L"?");
    else
        say("No HauppaugeRecorder driver package installed.\n");
    if (!nt)
        say("No HD PVR plugged in.\n");
    for (int i = 0; i < nt; i++)
        say("%s: %ls, driver %ls\n", t[i].name, t[i].instance, t[i].service[0] ? t[i].service : L"none");
    say("WinUSB available: %s\n", winusb_available() ? "yes" : "no (libusbK is used)");
    return 0;
}

int main(void)
{
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *cmd = L"install";
    int winusb = 0, libusbk = 0;
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--quiet")) g_quiet = 1;
        else if (!wcscmp(argv[i], L"--winusb")) winusb = 1;
        else if (!wcscmp(argv[i], L"--libusbk")) libusbk = 1;
        else if (argv[i][0] != L'-') cmd = argv[i];
        else {
            say("usage: hdpvr-driver [install [--winusb|--libusbk] | uninstall | status] [--quiet]\n");
            return 1;
        }
    }

    int r;
    if (!wcscmp(cmd, L"status")) {
        r = cmd_status();
    } else if (wcscmp(cmd, L"install") && wcscmp(cmd, L"uninstall")) {
        say("unknown command; use install, uninstall or status\n");
        r = 1;
    } else if (is_wow64()) {
        say("This is the 32-bit build. On 64-bit Windows, use the x86_64 download to install the driver.\n");
        r = 1;
    } else if (!is_elevated()) {
        r = relaunch_elevated();
        LocalFree(argv);
        return r;
    } else {
        r = !wcscmp(cmd, L"install") ? cmd_install(winusb, libusbk) : cmd_uninstall();
    }

    /* opened by double-click: keep the window up so the result can be read */
    DWORD pids[2];
    if (!g_quiet && GetConsoleProcessList(pids, 2) == 1) {
        say("\nPress Enter to close.");
        getchar();
    }
    LocalFree(argv);
    return r;
}
