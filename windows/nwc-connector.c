/*
 * nwc-connector.exe  -  all-in-one launcher for the Nintendo Wi-Fi USB Connector
 *                       (native Windows DS-online stack).
 *
 * A single self-contained executable. It embeds the three runtime payloads
 * (nwcusb_probe.exe, wintun.dll, libusb-1.0.dll) as resources, extracts them to
 * %LOCALAPPDATA%\NWC-Connector, self-elevates (Wintun + NAT need admin), starts
 * the probe with its logging streamed into this console window, and configures
 * the NWC-DS adapter IP + NAT once the probe has created the Wintun adapter.
 *
 *   nwc-connector.exe                     run the connector (default)
 *   nwc-connector.exe --install-autostart register a per-logon elevated task
 *   nwc-connector.exe --uninstall-autostart  remove that task
 *   nwc-connector.exe --help
 *
 * One prerequisite the launcher cannot embed: the RT2570 dongle must be bound to
 * the WinUSB/libusbK driver (see install-driver.cmd / Zadig). If the probe reports
 * "device not found", run that one-time step and relaunch.
 *
 * Build (done by .github/workflows/release.yml):
 *   rc.exe nwc-connector.rc
 *   cl.exe /O2 nwc-connector.c nwc-connector.res /Fe:nwc-connector.exe ^
 *          /link advapi32.lib shell32.lib ole32.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include "resource.h"

#define ADAPTER_NAME  "NWC-DS"
#define TASK_NAME     "NWC-DS-Connector"
#define APP_SUBDIR    "NWC-Connector"

/* --------------------------------------------------------------- helpers */

static void die(const char *msg) {
    DWORD e = GetLastError();
    fprintf(stderr, "[nwc] ERROR: %s (gle=%lu)\n", msg, e);
    fprintf(stderr, "[nwc] Press Enter to exit.\n");
    getchar();
    ExitProcess(1);
}

static int is_elevated(void) {
    HANDLE tok = NULL; TOKEN_ELEVATION el; DWORD sz = sizeof(el); int r = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz))
            r = el.TokenIsElevated ? 1 : 0;
        CloseHandle(tok);
    }
    return r;
}

/* Relaunch this exe elevated, forwarding argv, and wait for it to finish. */
static int relaunch_elevated(int argc, char **argv) {
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    char args[1024] = {0};
    for (int i = 1; i < argc; i++) {
        strncat(args, "\"", sizeof(args) - strlen(args) - 1);
        strncat(args, argv[i], sizeof(args) - strlen(args) - 1);
        strncat(args, "\" ", sizeof(args) - strlen(args) - 1);
    }
    SHELLEXECUTEINFOA sei = {0};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "runas";                 /* triggers the UAC prompt */
    sei.lpFile = self;
    sei.lpParameters = args[0] ? args : NULL;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&sei)) {
        fprintf(stderr, "[nwc] elevation was declined; the connector needs administrator rights.\n");
        return 1;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        return (int)code;
    }
    return 0;
}

/* Path to %LOCALAPPDATA%\NWC-Connector (created if missing). */
static void app_dir(char *out, size_t n) {
    char base[MAX_PATH];
    DWORD got = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
    if (!got || got >= sizeof(base)) { GetTempPathA(sizeof(base), base); }
    snprintf(out, n, "%s\\%s", base, APP_SUBDIR);
    CreateDirectoryA(out, NULL);
}

/* Write an embedded RCDATA resource to <dir>\<name> unless an identical-size copy
 * already exists. Returns 1 on success. */
static int extract_resource(int id, const char *dir, const char *name) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    HRSRC h = FindResourceA(NULL, MAKEINTRESOURCEA(id), RT_RCDATA);
    if (!h) { fprintf(stderr, "[nwc] missing embedded resource %d\n", id); return 0; }
    DWORD sz = SizeofResource(NULL, h);
    HGLOBAL g = LoadResource(NULL, h);
    const void *data = LockResource(g);
    if (!data || !sz) { fprintf(stderr, "[nwc] bad embedded resource %d\n", id); return 0; }

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)
        && fad.nFileSizeLow == sz && fad.nFileSizeHigh == 0) {
        return 1;   /* already extracted, same size */
    }
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { fprintf(stderr, "[nwc] cannot write %s\n", path); return 0; }
    DWORD wrote = 0; BOOL ok = WriteFile(f, data, sz, &wrote, NULL);
    CloseHandle(f);
    if (!ok || wrote != sz) { fprintf(stderr, "[nwc] short write %s\n", path); return 0; }
    printf("[nwc] extracted %s (%lu bytes)\n", name, sz);
    return 1;
}

/* Run a command line, optionally waiting; returns process exit code (or -1). */
static int run(const char *cmdline, int wait, int inherit_console) {
    char buf[2048]; strncpy(buf, cmdline, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    STARTUPINFOA si = {0}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    DWORD flags = inherit_console ? 0 : CREATE_NO_WINDOW;
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi))
        return -1;
    int code = 0;
    if (wait) { WaitForSingleObject(pi.hProcess, INFINITE); DWORD c = 0; GetExitCodeProcess(pi.hProcess, &c); code = (int)c; }
    CloseHandle(pi.hThread);
    if (wait) CloseHandle(pi.hProcess);
    return code;
}

/* Configure the NWC-DS adapter (IP 192.168.44.1/24) + Windows NAT for the
 * 192.168.44.0/24 subnet, retrying while the probe brings the adapter up. Reuses
 * the exact PowerShell that the manual bring-up uses. */
static void configure_network(const char *dir) {
    char ps_path[MAX_PATH];
    snprintf(ps_path, sizeof(ps_path), "%s\\nwc-setup.ps1", dir);
    static const char *PS =
        "$ErrorActionPreference='SilentlyContinue'\n"
        "for($i=0;$i -lt 20;$i++){\n"
        "  $a=Get-NetAdapter -Name 'NWC-DS' -EA SilentlyContinue\n"
        "  if($a -and $a.ifIndex){\n"
        "    Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -EA SilentlyContinue | Remove-NetIPAddress -Confirm:$false -EA SilentlyContinue\n"
        "    New-NetIPAddress -InterfaceIndex $a.ifIndex -IPAddress 192.168.44.1 -PrefixLength 24 -EA SilentlyContinue | Out-Null\n"
        "    Set-NetIPInterface -InterfaceIndex $a.ifIndex -Forwarding Enabled -EA SilentlyContinue\n"
        "    Start-Sleep 1\n"
        "    if((Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -EA SilentlyContinue).IPAddress -eq '192.168.44.1'){ break }\n"
        "  }\n"
        "  Start-Sleep 2\n"
        "}\n"
        "Get-NetNat -Name NWC -EA SilentlyContinue | Remove-NetNat -Confirm:$false -EA SilentlyContinue\n"
        "New-NetNat -Name NWC -InternalIPInterfaceAddressPrefix '192.168.44.0/24' -EA SilentlyContinue | Out-Null\n"
        "# enable forwarding on every 'up' physical adapter so NAT can reach the internet\n"
        "Get-NetAdapter -Physical | Where-Object Status -eq 'Up' | ForEach-Object { Set-NetIPInterface -InterfaceIndex $_.ifIndex -Forwarding Enabled -EA SilentlyContinue }\n"
        "$ip=(Get-NetIPAddress -InterfaceIndex (Get-NetAdapter -Name 'NWC-DS').ifIndex -AddressFamily IPv4 -EA SilentlyContinue).IPAddress\n"
        "Write-Host ('[nwc] NWC-DS IP='+$ip+' NAT='+((Get-NetNat -Name NWC -EA SilentlyContinue).Active))\n";
    FILE *f = fopen(ps_path, "wb");
    if (!f) { fprintf(stderr, "[nwc] cannot write setup script\n"); return; }
    fwrite(PS, 1, strlen(PS), f); fclose(f);
    char cmd[MAX_PATH + 128];
    snprintf(cmd, sizeof(cmd),
        "powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps_path);
    printf("[nwc] configuring NWC-DS adapter + NAT ...\n");
    run(cmd, 1, 1);
}

static void teardown_nat(void) {
    run("powershell -NoProfile -ExecutionPolicy Bypass -Command "
        "\"Get-NetNat -Name NWC -EA SilentlyContinue | Remove-NetNat -Confirm:$false -EA SilentlyContinue\"",
        1, 0);
}

/* ------------------------------------------------------------- autostart */

static int autostart_install(void) {
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, sizeof(self));
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "schtasks /Create /TN \"%s\" /TR \"'%s'\" /SC ONLOGON /RL HIGHEST /F",
        TASK_NAME, self);
    int rc = run(cmd, 1, 1);
    if (rc == 0) printf("[nwc] autostart installed: '%s' runs at logon (elevated).\n", TASK_NAME);
    else fprintf(stderr, "[nwc] schtasks failed (rc=%d)\n", rc);
    return rc;
}

static int autostart_uninstall(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "schtasks /Delete /TN \"%s\" /F", TASK_NAME);
    int rc = run(cmd, 1, 1);
    if (rc == 0) printf("[nwc] autostart removed.\n");
    return rc;
}

/* ------------------------------------------------------------------ main */

static PROCESS_INFORMATION g_probe = {0};

static BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;
    printf("\n[nwc] shutting down ...\n");
    if (g_probe.hProcess) TerminateProcess(g_probe.hProcess, 0);
    teardown_nat();
    return FALSE;   /* let default handler terminate us */
}

static void usage(void) {
    printf(
      "Nintendo Wi-Fi USB Connector (native Windows)\n"
      "  nwc-connector.exe                      run the connector\n"
      "  nwc-connector.exe --install-autostart  start automatically at every logon\n"
      "  nwc-connector.exe --uninstall-autostart remove the autostart task\n"
      "  nwc-connector.exe --help               this text\n");
}

int main(int argc, char **argv) {
    int want_install = 0, want_uninstall = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--install-autostart")) want_install = 1;
        else if (!strcmp(argv[i], "--uninstall-autostart")) want_uninstall = 1;
    }

    if (!is_elevated())
        return relaunch_elevated(argc, argv);   /* re-enter as admin, then continue below */

    if (want_uninstall) return autostart_uninstall();
    if (want_install)   return autostart_install();

    printf("=== Nintendo Wi-Fi USB Connector (Windows) ===\n");
    printf("[nwc] running with administrator rights.\n");

    char dir[MAX_PATH]; app_dir(dir, sizeof(dir));
    printf("[nwc] runtime dir: %s\n", dir);
    if (!extract_resource(IDR_LIBUSB, dir, "libusb-1.0.dll")) die("extract libusb");
    if (!extract_resource(IDR_WINTUN, dir, "wintun.dll"))     die("extract wintun");
    if (!extract_resource(IDR_PROBE,  dir, "nwcusb_probe.exe")) die("extract probe");
    if (!extract_resource(IDR_WINDIVERT_DLL, dir, "WinDivert.dll"))   die("extract WinDivert.dll");
    if (!extract_resource(IDR_WINDIVERT_SYS, dir, "WinDivert64.sys")) die("extract WinDivert64.sys");

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    /* Proven tuning env for the RT2570 SoftAP + data path (validated end-to-end: a DS reaches
     * Wiimmfi matchmaking). The probe answers DHCP/DNS in-process and NATs via WinDivert. */
    SetEnvironmentVariableA("NWC_DATAPATH", "1");
    SetEnvironmentVariableA("NWC_CSR0", "0x1ec2");
    SetEnvironmentVariableA("NWC_PROBE_MINGAP_MS", "300");
    SetEnvironmentVariableA("NWC_NO_RAWRETURN", "1");
    SetEnvironmentVariableA("NWC_TXPOWER", "31");
    SetEnvironmentVariableA("NWC_WD_DUP", "1");
    SetEnvironmentVariableA("NWC_MSS", "0");
    SetEnvironmentVariableA("NWC_NO_BAREACK", "1");
    SetEnvironmentVariableA("NWC_NO_CCATUNE", "1");
    SetEnvironmentVariableA("NWC_NO_AUTORECOVER", "1");
    SetEnvironmentVariableA("NWC_RX_DRAIN", "24");
    SetEnvironmentVariableA("NWC_DATA_TXPOWER", "0");
    SetEnvironmentVariableA("NWC_WD_DRAIN", "24");

    char probe[MAX_PATH];
    snprintf(probe, sizeof(probe), "\"%s\\nwcusb_probe.exe\" ap-loop 1", dir);

    STARTUPINFOA si = {0}; si.cb = sizeof(si);
    printf("[nwc] starting probe: %s\n", probe);
    char cmdbuf[MAX_PATH + 32]; strncpy(cmdbuf, probe, sizeof(cmdbuf) - 1); cmdbuf[sizeof(cmdbuf)-1]=0;
    if (!CreateProcessA(NULL, cmdbuf, NULL, NULL, TRUE, 0, NULL, dir, &si, &g_probe))
        die("failed to start nwcusb_probe.exe");

    /* Give the probe time to init the dongle + create the Wintun adapter, then
     * configure the adapter IP and NAT. (The probe streams its own log here.) */
    Sleep(9000);
    configure_network(dir);
    printf("[nwc] up. Scan for the connector from the DS. Ctrl+C to stop.\n");

    WaitForSingleObject(g_probe.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(g_probe.hProcess, &code);
    CloseHandle(g_probe.hProcess); CloseHandle(g_probe.hThread);
    printf("[nwc] probe exited (code %lu). Removing NAT.\n", code);
    teardown_nat();
    return (int)code;
}
