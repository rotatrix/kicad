# Inspect the actual NSIS payload without running the installer or changing registry state.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [string]$SevenZip = "$env:ProgramFiles/7-Zip/7z.exe"
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $SevenZip)) { throw 'Set -SevenZip to the full 7-Zip executable (7za does not read NSIS)' }
$packageRoot = Join-Path (Split-Path $PSScriptRoot -Parent) 'build/package-check'
$extracted = Join-Path $packageRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $extracted | Out-Null
& $SevenZip x $Installer "-o$extracted" -y -bso0
if ($LASTEXITCODE) { throw 'NSIS payload extraction failed' }
foreach ($relative in @('bin/kicad.exe', 'bin/eeschema.exe', 'bin/pcbnew.exe',
        'bin/gerbview.exe', 'bin/kicad-cli.exe', 'bin/python.exe',
        'bin/licenses/LICENSE.GPLv3', 'bin/licenses/openaxis/LICENSE',
        'bin/licenses/openaxis/IXWebSocket-LICENSE.txt', 'bin/licenses/openssl/copyright',
        'bin/licenses/wxpython-33/copyright', 'bin/licenses/vcpkg-configuration.json')) {
    if (!(Test-Path -LiteralPath (Join-Path $extracted $relative) -PathType Leaf)) {
        throw "Installer is missing $relative"
    }
}
& "$extracted/bin/kicad-cli.exe" version
if ($LASTEXITCODE) { throw 'Extracted CLI failed' }
& "$extracted/bin/python.exe" -I -c 'import wx, pcbnew, ssl, sqlite3, ctypes; print(wx.version()); print(pcbnew.GetBuildVersion())'
if ($LASTEXITCODE) { throw 'Extracted Python runtime failed' }
Get-FileHash -LiteralPath $Installer -Algorithm SHA256
Write-Host "Verified installer payload: $extracted"
