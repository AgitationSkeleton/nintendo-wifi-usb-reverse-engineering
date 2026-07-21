# Generate a self-signed test cert, trust it, and sign nwcusbap.sys + .cat.
# Safe to run NOW (does not touch Secure Boot / boot config). The driver only
# actually LOADS after Secure Boot is disabled + testsigning is on + reboot.
# Must run elevated (writes to LocalMachine cert stores).
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = "$root\build"
$kit = "C:\Program Files (x86)\Windows Kits\10"
$signtool = "$kit\bin\10.0.19041.0\x64\signtool.exe"
$inf2cat  = "$kit\bin\10.0.19041.0\x86\Inf2Cat.exe"
$certName = "NWCUSBAP Test Cert"

if (-not (Test-Path "$build\nwcusbap.sys")) { throw "build nwcusbap.sys first (build-kmdf.ps1)" }
Copy-Item "$root\nwcusbap.inf" "$build\nwcusbap.inf" -Force

# 1. self-signed code-signing cert (reuse if present)
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq "CN=$certName" } | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$certName" `
            -CertStoreLocation Cert:\LocalMachine\My -KeyUsage DigitalSignature `
            -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(5)
  Write-Host "[sign] created cert $($cert.Thumbprint)"
} else { Write-Host "[sign] reusing cert $($cert.Thumbprint)" }

# 2. trust it (Root + TrustedPublisher) so Windows accepts the driver signature
$store = Get-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)"
foreach ($s in @("Root","TrustedPublisher")) {
  $o = New-Object System.Security.Cryptography.X509Certificates.X509Store($s,"LocalMachine")
  $o.Open("ReadWrite"); $o.Add($store); $o.Close()
}

# 3. sign the .sys
& $signtool sign /v /sm /fd SHA256 /sha1 $cert.Thumbprint /tr http://timestamp.digicert.com /td SHA256 "$build\nwcusbap.sys"
if ($LASTEXITCODE -ne 0) { throw "signtool (sys) failed" }

# 4. build catalog from the INF, then sign it
& $inf2cat /driver:"$build" /os:10_X64 /verbose
if ($LASTEXITCODE -ne 0) { throw "inf2cat failed" }
& $signtool sign /v /sm /fd SHA256 /sha1 $cert.Thumbprint /tr http://timestamp.digicert.com /td SHA256 "$build\nwcusbap.cat"
if ($LASTEXITCODE -ne 0) { throw "signtool (cat) failed" }

Write-Host "[sign] done. nwcusbap.sys + nwcusbap.cat signed and cert trusted."
Write-Host "[sign] AFTER Secure Boot off + 'bcdedit /set testsigning on' + reboot, install with:"
Write-Host "        pnputil /add-driver `"$build\nwcusbap.inf`" /install"
