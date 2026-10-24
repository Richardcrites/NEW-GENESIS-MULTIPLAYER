<#
.SYNOPSIS
  Authenticode-sign PULSAR release binaries (tool exes + the mod DLL).

.DESCRIPTION
  Run AFTER publishing/building, BEFORE zipping a release. Signing is what removes the
  "Unknown publisher" SmartScreen text and lets Windows build reputation for the file.
  You need a code-signing certificate (OV or EV). NEVER commit the .pfx or its password.

  - OV cert: works immediately, but SmartScreen still warns until reputation accrues
    (re-sign every release with the SAME cert so reputation accumulates).
  - EV cert (hardware token / cloud HSM): near-instant SmartScreen reputation.

.EXAMPLE
  # PFX file on disk:
  .\sign.ps1 -Pfx C:\secret\pulsar.pfx -Password 's3cret' `
             -Files ..\lobby\_dist\Pulsar-v0.2.0\Pulsar.exe, ..\lobby\_dist\Pulsar-v0.2.0\PulsarCompanion.exe

.EXAMPLE
  # Cert already imported into the user's cert store (e.g. EV token):
  .\sign.ps1 -Thumbprint A1B2C3... -Files .\PULSAR.dll
#>
param(
    [string]$Pfx,
    [string]$Password,
    [string]$Thumbprint,
    [Parameter(Mandatory = $true)][string[]]$Files,
    # DigiCert's RFC3161 timestamp server; any trusted /tr endpoint works. Timestamping
    # keeps signatures valid AFTER the cert expires - always do it.
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)
$ErrorActionPreference = "Stop"

function Find-SignTool {
    $found = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" `
                -ErrorAction SilentlyContinue | Sort-Object FullName -Descending
    if ($found) { return $found[0].FullName }
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "signtool.exe not found. Install the Windows 10/11 SDK ('Windows SDK Signing Tools')."
}

if (-not $Thumbprint -and -not $Pfx) {
    throw "Provide either -Thumbprint (cert in the store) or -Pfx (+ optional -Password)."
}

$signtool = Find-SignTool
Write-Host "signtool: $signtool"

foreach ($f in $Files) {
    if (-not (Test-Path $f)) { throw "File not found: $f" }
    $sargs = @("sign", "/fd", "sha256", "/td", "sha256", "/tr", $TimestampUrl)
    if ($Thumbprint) {
        $sargs += @("/sha1", $Thumbprint)
    } else {
        $sargs += @("/f", $Pfx)
        if ($Password) { $sargs += @("/p", $Password) }
    }
    $sargs += $f
    Write-Host "Signing $f ..."
    & $signtool @sargs
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f (exit $LASTEXITCODE)" }
    & $signtool verify /pa /v $f | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signature verify failed on $f" }
}

Write-Host "All files signed + timestamped + verified."
