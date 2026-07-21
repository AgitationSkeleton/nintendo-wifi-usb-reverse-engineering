# Build nwcusbap.sys (KMDF 1.11, x64) from the command line with the WDK.
# Compiling does NOT require Secure Boot off; only loading the signed .sys does.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
# Locate MSVC portably: the known-good 14.44.35207 lives under different VS roots
# on different machines (2022 BuildTools on the server, VS 2026 Community on the
# main PC). Prefer that exact version anywhere it exists, else newest via vswhere.
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
if (-not $vc) { throw "MSVC toolchain not found; set `$vc manually in build-kmdf.ps1" }
Write-Host "[kmdf] using MSVC at $vc"
$kit  = "C:\Program Files (x86)\Windows Kits\10"
$ver  = "10.0.19041.0"
$kmdf = "1.11"

# Order matters: kernel CRT first so it shadows the user-mode VC/ucrt CRT; VC
# (compiler intrinsics) and ucrt (corecrt.h fallback) come last.
$inc = @(
  "$kit\Include\$ver\km\crt",
  "$kit\Include\$ver\km",
  "$kit\Include\$ver\shared",
  "$kit\Include\wdf\kmdf\$kmdf",
  "$vc\include",
  "$kit\Include\$ver\ucrt"
) -join ";"
$env:INCLUDE = $inc

$libpaths = @(
  "$kit\Lib\$ver\km\x64",
  "$kit\Lib\wdf\kmdf\x64\$kmdf"
)
$env:PATH = "$vc\bin\Hostx64\x64;$env:PATH"

New-Item -ItemType Directory -Force -Path "$root\build" | Out-Null
Push-Location "$root\build"
try {
  # Kernel-mode C compile
  $cflags = @(
    "/c","/nologo","/W3","/O2","/GF","/Gy","/Gm-","/GS","/Zp8","/fp:precise",
    "/Zc:wchar_t-","/Zc:forScope","/GR-","/kernel",
    "/D_AMD64_","/DAMD64","/D_WIN64","/DWIN64",
    "/DKMDF_VERSION_MAJOR=1","/DKMDF_VERSION_MINOR=11","/DNTDDI_VERSION=0x0A000008",
    "/DDEPRECATE_DDK_FUNCTIONS=1","/DPOOL_NX_OPTIN=1"
  )
  & cl.exe @cflags "/I$root" "$root\nwcusbap.c" /Fonwcusbap.obj
  if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

  $lflags = @(
    "/NOLOGO","/DRIVER","/SUBSYSTEM:NATIVE,10.00","/ENTRY:FxDriverEntry","/NODEFAULTLIB",
    "/OUT:nwcusbap.sys","/MERGE:_TEXT=.text;_PAGE=PAGE","/INTEGRITYCHECK"
  )
  $libs = @("wdfdriverentry.lib","wdfldr.lib","ntoskrnl.lib","hal.lib","usbdex.lib",
            "wdmsec.lib","BufferOverflowFastFailK.lib")
  $lp = ($libpaths | ForEach-Object { "/LIBPATH:$_" })
  & link.exe @lflags @lp nwcusbap.obj @libs
  if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
  Write-Host "[kmdf] built nwcusbap.sys"
} finally { Pop-Location }
