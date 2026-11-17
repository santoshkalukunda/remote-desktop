# Builds build\RemoteDesk.msix from build\RemoteDesk.exe (run build.bat first).
#
#   .\build-msix.ps1                       # package with test identity
#   .\build-msix.ps1 -Register             # also install it on this PC (Developer Mode)
#   .\build-msix.ps1 -Name ... -Publisher ... -PublisherName ... -Version 1.0.1.0
#                                          # Store build: values from Partner Center (STORE.md)
#   .\build-msix.ps1 -Sign                 # self-signed, to sideload on test PCs
param(
    [string]$Name = 'Kalukunda.RemoteDesktopClient',                      # Partner Center: Package/Identity/Name
    [string]$Publisher = 'CN=7D1AD138-83CF-4BF6-9231-7D8B7ED89594',      # Partner Center: Package/Identity/Publisher
    [string]$PublisherName = 'Kalukunda',                                 # Partner Center: Package/Properties/PublisherDisplayName
    [string]$DisplayName = 'Remote Desktop (Client)',                     # the app name reserved in Partner Center
    [string]$Version = '',                          # default: src\core\version.h + ".0" (the Store needs the last number to be 0)
    [string]$Relay = '',                            # optional host:port override; default: src\core\server_config.h
    [switch]$Register,
    [switch]$Sign
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = Join-Path $root 'build\RemoteDesk.exe'
if (-not (Test-Path $exe)) { throw 'build\RemoteDesk.exe not found. Run build.bat first.' }
if (-not $Version) {   # same version as the app: one place to bump it
    $vh = Get-Content -Raw (Join-Path $root 'src\core\version.h')
    $n = foreach ($k in 'MAJOR', 'MINOR', 'PATCH') { [regex]::Match($vh, "RD_VERSION_$k\s+(\d+)").Groups[1].Value }
    $Version = ($n -join '.') + '.0'
}
if ($Version -notmatch '^\d+\.\d+\.\d+\.0$') { throw "Version must look like 1.2.3.0 (got $Version)." }
if ($Relay -match '^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.|127\.)') {
    Write-Warning "Relay $Relay is a private address: only computers on that network can use it. Store users and reviewers need a public relay."
}

$kit = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\makeappx.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $kit) { throw 'makeappx.exe not found. Install the Windows 10/11 SDK.' }
$bin = $kit.DirectoryName

# Stage the package contents.
$stage = Join-Path $root 'build\msix'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\Assets" | Out-Null
Copy-Item $exe, (Join-Path $root 'PRIVACY.txt') $stage
Copy-Item (Join-Path $root 'packaging\Assets\*.png') "$stage\Assets"
# The app connects to the relay compiled in from src\core\server_config.h; -Relay overrides it for this package.
if ($Relay) { Set-Content -Encoding ascii "$stage\config.ini" "[RemoteDesk]`r`nRelay=$Relay`r`n" }
$manifest = (Get-Content -Raw (Join-Path $root 'packaging\AppxManifest.xml')).
    Replace('$NAME$', $Name).Replace('$PUBLISHER$', [Security.SecurityElement]::Escape($Publisher)).
    Replace('$PUBLISHER_NAME$', [Security.SecurityElement]::Escape($PublisherName)).Replace('$VERSION$', $Version).
    Replace('$DISPLAY_NAME$', [Security.SecurityElement]::Escape($DisplayName))
Set-Content -Encoding utf8 "$stage\AppxManifest.xml" $manifest

$msix = Join-Path $root 'build\RemoteDesk.msix'
& "$bin\makeappx.exe" pack /o /d $stage /p $msix
if ($LASTEXITCODE) { throw 'makeappx failed.' }

if ($Sign) {
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Publisher -and $_.HasPrivateKey } |
        Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature `
            -FriendlyName 'RemoteDesk MSIX test signing' -CertStoreLocation Cert:\CurrentUser\My `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    }
    Export-Certificate -Cert $cert -FilePath (Join-Path $root 'build\RemoteDesk-test.cer') | Out-Null
    & "$bin\signtool.exe" sign /fd SHA256 /sha1 $cert.Thumbprint $msix
    if ($LASTEXITCODE) { throw 'signtool failed.' }
    Write-Host 'Signed. On a test PC, import build\RemoteDesk-test.cer into Local Machine > Trusted People first.'
}

if ($Register) {
    # Installs straight from build\msix (needs Developer Mode); keep that folder while testing.
    Add-AppxPackage -Register "$stage\AppxManifest.xml" -ForceApplicationShutdown
    Write-Host 'Installed for this user: find RemoteDesk in the Start menu.'
}
Write-Host "Built $msix"
