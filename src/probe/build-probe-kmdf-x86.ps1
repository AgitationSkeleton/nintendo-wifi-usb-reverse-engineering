# Build nwcusb_probe_kmdf_x86.exe — the SoftAP probe with the KMDF driver backend,
# 32-bit (x86) for the Win10 x86 test VM. All hardware access goes through
# nwcusbap.sys IOCTLs on \\.\NWCUSBAP; libusb is NOT linked (the libusb-referencing
# helpers are now gated out under NWC_BACKEND_KMDF), only its header is used for types.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

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
Write-Host "[build-kmdf-x86] using MSVC at $vc"

$sdkI = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.19041.0"
$sdkL = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0"
$env:INCLUDE = "$vc\include;$sdkI\ucrt;$sdkI\shared;$sdkI\um"
$env:LIB     = "$vc\lib\x86;$sdkL\ucrt\x86;$sdkL\um\x86"
$env:PATH    = "$vc\bin\Hostx64\x86;$env:PATH"
$inc = "$root\..\native-windows-assets\libusb\libusb"   # header types only, no lib
New-Item -ItemType Directory -Force -Path "$root\build-x86" | Out-Null
Push-Location "$root\build-x86"
try {
    & cl.exe /nologo /W3 /O2 /EHsc /DNWC_BACKEND_KMDF "/I$inc" "$root\nwcusb_probe.c" `
        /Fenwcusb_probe_kmdf_x86.exe
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed ($LASTEXITCODE)" }
    Write-Host "[build] wrote $root\build-x86\nwcusb_probe_kmdf_x86.exe"
} finally { Pop-Location }
