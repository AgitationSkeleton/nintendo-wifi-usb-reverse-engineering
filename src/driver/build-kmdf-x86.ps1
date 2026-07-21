# Build nwcusbap.sys as 32-bit (x86) KMDF 1.11 — for the Win10 x86 test VM.
# Same source; only arch (defines/libs/toolchain target) differs from build-kmdf.ps1.
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
      elseif (Test-Path $msvcRoot) { $vc = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName }
    }
  }
}
if (-not $vc) { throw "MSVC toolchain not found" }
Write-Host "[kmdf-x86] using MSVC at $vc"
$kit  = "C:\Program Files (x86)\Windows Kits\10"
$ver  = "10.0.19041.0"
$kmdf = "1.11"

$inc = @(
  "$kit\Include\$ver\km\crt","$kit\Include\$ver\km","$kit\Include\$ver\shared",
  "$kit\Include\wdf\kmdf\$kmdf","$vc\include","$kit\Include\$ver\ucrt"
) -join ";"
$env:INCLUDE = $inc
$libpaths = @("$kit\Lib\$ver\km\x86","$kit\Lib\wdf\kmdf\x86\$kmdf")
# x86 cross toolchain (compiler + linker that emit 32-bit)
$env:PATH = "$vc\bin\Hostx64\x86;$env:PATH"

New-Item -ItemType Directory -Force -Path "$root\build-x86" | Out-Null
Push-Location "$root\build-x86"
try {
  $cflags = @(
    "/c","/nologo","/W3","/O2","/GF","/Gy","/Gm-","/GS","/Zp8","/fp:precise",
    "/Zc:wchar_t-","/Zc:forScope","/GR-","/kernel","/Gz",   # /Gz = __stdcall default (x86 kernel ABI)
    "/D_X86_=1","/Di386=1",
    "/DKMDF_VERSION_MAJOR=1","/DKMDF_VERSION_MINOR=11","/DNTDDI_VERSION=0x0A000008",
    "/DDEPRECATE_DDK_FUNCTIONS=1","/DPOOL_NX_OPTIN=1"
  )
  & cl.exe @cflags "/I$root" "$root\nwcusbap.c" /Fonwcusbap.obj
  if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

  # x86 uses stdcall entry decoration: _FxDriverEntry@8
  $lflags = @(
    "/NOLOGO","/DRIVER","/SUBSYSTEM:NATIVE,10.00","/ENTRY:`"FxDriverEntry@8`"","/NODEFAULTLIB",
    "/OUT:nwcusbap.sys","/MERGE:_TEXT=.text;_PAGE=PAGE","/INTEGRITYCHECK","/MACHINE:X86","/SAFESEH"
  )
  $libs = @("wdfdriverentry.lib","wdfldr.lib","ntoskrnl.lib","hal.lib","usbdex.lib",
            "wdmsec.lib","BufferOverflowFastFailK.lib")
  $lp = ($libpaths | ForEach-Object { "/LIBPATH:$_" })
  & link.exe @lflags @lp nwcusbap.obj @libs
  if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
  Write-Host "[kmdf-x86] built nwcusbap.sys (x86)"
} finally { Pop-Location }
