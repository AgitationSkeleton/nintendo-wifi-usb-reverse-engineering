# Sign the x86 (build-x86) driver + catalog with the same test cert. Elevated.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = "$root\build-x86"
$kit = "C:\Program Files (x86)\Windows Kits\10"
$signtool = "$kit\bin\10.0.19041.0\x64\signtool.exe"
$inf2cat  = "$kit\bin\10.0.19041.0\x86\Inf2Cat.exe"
$certName = "NWCUSBAP Test Cert"

if (-not (Test-Path "$build\nwcusbap.sys")) { throw "build x86 first (build-kmdf-x86.ps1)" }
Copy-Item "$root\nwcusbap.inf" "$build\nwcusbap.inf" -Force

$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq "CN=$certName" } | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$certName" `
            -CertStoreLocation Cert:\LocalMachine\My -KeyUsage DigitalSignature `
            -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(5)
  Write-Host "[sign-x86] created cert $($cert.Thumbprint)"
} else { Write-Host "[sign-x86] reusing cert $($cert.Thumbprint)" }
$store = Get-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)"
foreach ($s in @("Root","TrustedPublisher")) {
  $o = New-Object System.Security.Cryptography.X509Certificates.X509Store($s,"LocalMachine")
  $o.Open("ReadWrite"); $o.Add($store); $o.Close()
}
# Export the cert so it can be imported into the VM's trust stores.
Export-Certificate -Cert $store -FilePath "$build\NWCUSBAP_TestCert.cer" -Force | Out-Null

& $signtool sign /v /sm /fd SHA256 /sha1 $cert.Thumbprint "$build\nwcusbap.sys"
if ($LASTEXITCODE -ne 0) { throw "signtool (sys) failed" }
& $inf2cat /driver:"$build" /os:10_X86 /verbose
if ($LASTEXITCODE -ne 0) { throw "inf2cat failed" }
& $signtool sign /v /sm /fd SHA256 /sha1 $cert.Thumbprint "$build\nwcusbap.cat"
if ($LASTEXITCODE -ne 0) { throw "signtool (cat) failed" }
Write-Host "[sign-x86] signed nwcusbap.sys + .cat (x86); cert exported to build-x86\NWCUSBAP_TestCert.cer"
