# Build nwcusb_probe_kmdf.exe — the SoftAP probe with the KMDF driver backend
# (-DNWC_BACKEND_KMDF): all hardware access goes through nwcusbap.sys IOCTLs on
# \\.\NWCUSBAP instead of libusb/WinUSB. Requires nwcusbap.sys installed + started
# and the dongle bound to it. See kmdf\REBOOT_READINESS.md.
#
# libusb.h is still included for its types/constants (the opaque handle type),
# and libusb-1.0 is still linked/copied because a couple of unused helpers
# (dump_config, rx_transfer_cb) reference it; the KMDF exe never calls libusb.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# Locate MSVC portably (same logic as kmdf\build-kmdf.ps1).
$vcVer = "14.44.35207"
$vc = @(
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\$vcVer",
  "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\$vcVer"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vc) {
  $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -latest -products * -property installationPath
    if ($vsRoot) {
      $msvcRoot = Join-Path $vsRoot "VC\Tools\MSVC"
      $exact = Join-Path $msvcRoot $vcVer
      if (Test-Path $exact) { $vc = $exact }
      elseif (Test-Path $msvcRoot) {
        $vc = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
      }
    }
  }
}
if (-not $vc) { throw "MSVC toolchain not found; set `$vc manually" }
Write-Host "[build-kmdf] using MSVC at $vc"

$sdkI = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.19041.0"
$sdkL = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0"
$env:INCLUDE = "$vc\include;$sdkI\ucrt;$sdkI\shared;$sdkI\um"
$env:LIB     = "$vc\lib\x64;$sdkL\ucrt\x64;$sdkL\um\x64"
$env:PATH    = "$vc\bin\Hostx64\x64;$env:PATH"
$inc = "$root\..\native-windows-assets\libusb\libusb"
$lib = "$root\..\native-windows-assets\libusb\build\v143\x64\Release\dll\libusb-1.0.lib"
$dll = "$root\..\native-windows-assets\libusb\build\v143\x64\Release\dll\libusb-1.0.dll"
New-Item -ItemType Directory -Force -Path "$root\build" | Out-Null
Push-Location "$root\build"
try {
    & cl.exe /nologo /W3 /O2 /EHsc /DNWC_BACKEND_KMDF "/I$inc" "$root\nwcusb_probe.c" `
        /Fenwcusb_probe_kmdf.exe /link "$lib"
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed ($LASTEXITCODE)" }
    Copy-Item -LiteralPath $dll -Destination "$root\build\" -Force
    Write-Host "[build] wrote $root\build\nwcusb_probe_kmdf.exe"
} finally { Pop-Location }
