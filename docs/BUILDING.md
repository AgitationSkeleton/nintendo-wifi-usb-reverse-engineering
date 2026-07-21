# Building

Everything targets **Windows** and MSVC. There are two independent components — you can build and
use the user-mode probe on its own; the kernel driver is only needed for the in-kernel responder.

---

## Prerequisites

| Requirement | Notes |
|---|---|
| **Visual Studio Build Tools 2022** (MSVC v143) | The build scripts probe common install paths and fall back to `vswhere`. If your MSVC toolset version differs, edit `$vcVer` at the top of the script. |
| **Windows SDK 10.0.19041.0** | Other 10.x SDKs work — update the `10.0.19041.0` paths in the scripts. |
| **Windows Driver Kit (WDK)**, KMDF 1.11 | Kernel driver only. |
| **libusb** headers (`libusb.h`) | User-mode probe only — see below. |
| **Python 3** | Only for `tools/decode_usb_capture.py`. |

### libusb header

The probe includes `libusb.h` purely for its opaque handle and enum **types**. In the KMDF-backend
build it never *calls* libusb — all hardware access goes through the driver's IOCTLs.

Provide the header either way:

```powershell
# Option A: point at an existing libusb checkout/release
$env:LIBUSB_INCLUDE = "C:\path\to\libusb\libusb"

# Option B: drop libusb.h into the repo (git-ignored)
#   third_party/libusb/libusb.h
```

If you build a libusb-backed variant (rather than the KMDF backend), also set:

```powershell
$env:LIBUSB_LIB = "C:\path\to\libusb-1.0.lib"
$env:LIBUSB_DLL = "C:\path\to\libusb-1.0.dll"
```

libusb is not vendored here — get it from <https://libusb.info/> (LGPL-2.1).

---

## Build: user-mode probe

```powershell
cd src\probe
.\build-probe-kmdf.ps1        # x64  -> build\nwcusb_probe_kmdf.exe
.\build-probe-kmdf-x86.ps1    # x86  -> build-x86\nwcusb_probe_kmdf_x86.exe
```

Both compile with `-DNWC_BACKEND_KMDF`, meaning the probe talks to the kernel driver over
`\\.\NWCUSBAP` instead of libusb/WinUSB. The x86 build links **no** libusb at all (the
libusb-referencing helpers are compiled out under that define) — useful for a 32-bit test VM.

To build a libusb/WinUSB-backed variant instead, drop `/DNWC_BACKEND_KMDF` and link
`libusb-1.0.lib`; the dongle must then be bound to WinUSB/libusbK (e.g. via Zadig) rather than to
this project's driver.

---

## Build: kernel driver

```powershell
cd src\driver
.\build-kmdf.ps1        # x64 -> build\nwcusbap.sys
.\build-kmdf-x86.ps1    # x86 -> build-x86\nwcusbap.sys
.\sign-driver.ps1       # test-sign x64 (creates a test cert, runs inf2cat + signtool)
.\sign-driver-x86.ps1   # test-sign x86
```

The signing scripts generate a self-signed **test certificate**, build the catalog, and sign both
the `.sys` and `.cat`. They also export the certificate so you can trust it on the target machine.

Notes:

- The x86 build needs `/Gz` (stdcall) plus the `km\x86` and `wdf\kmdf\x86` libraries — already
  handled in `build-kmdf-x86.ps1`.
- `nwc_kmtest.c` builds a small user-mode harness that drives the driver's IOCTLs with synthetic
  frames — handy for exercising the in-kernel responder **without** a DS or radio.

---

## Test-signing

A test-signed driver only loads with test-signing enabled:

```powershell
bcdedit /set testsigning on
# reboot
```

Import the exported test certificate into **both** stores on the target machine:

```powershell
certutil -addstore Root          NWCUSBAP_TestCert.cer
certutil -addstore TrustedPublisher NWCUSBAP_TestCert.cer
```

> **Strongly recommended:** do driver work in a throwaway VM, not on a machine you care about. A
> kernel driver fault is a bugcheck. This project's driver was developed that way.
>
> To undo afterwards: remove the driver packages (`pnputil /delete-driver oemNN.inf /uninstall`),
> delete the certificate from both stores (`certutil -delstore …`), then `bcdedit /set testsigning
> off` and reboot.

---

## Virtualising the dongle

If you pass the dongle through to a VM, give the VM a **USB 2.0 (EHCI) or 3.0 (xHCI)** controller.
The device enumerates at high speed with 512-byte bulk endpoints; a USB 1.1 (OHCI) controller
forces full speed, where those endpoints are illegal and USB configuration selection fails with
`STATUS_INVALID_PARAMETER`.
