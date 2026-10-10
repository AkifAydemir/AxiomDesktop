param(
    [Parameter(Mandatory=$true)][string]$IconPath,
    [Parameter(Mandatory=$true)][string]$ExecutablePath
)
$ErrorActionPreference = 'Stop'
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $IconPath).Path)
$sizes = @(16,24,32,48,64,128,256)
if ($bytes.Length -lt 118 -or [BitConverter]::ToUInt16($bytes,0) -ne 0 -or
    [BitConverter]::ToUInt16($bytes,2) -ne 1 -or
    [BitConverter]::ToUInt16($bytes,4) -ne $sizes.Count) { throw 'Invalid multi-resolution ICO header' }
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $entry = 6 + 16 * $i
    $size = if ($bytes[$entry] -eq 0) { 256 } else { [int]$bytes[$entry] }
    $height = if ($bytes[$entry+1] -eq 0) { 256 } else { [int]$bytes[$entry+1] }
    $length = [BitConverter]::ToUInt32($bytes,$entry+8)
    $offset = [BitConverter]::ToUInt32($bytes,$entry+12)
    if ($size -ne $sizes[$i] -or $height -ne $size -or $length -lt 24 -or
        $offset -lt 118 -or [long]$offset + $length -gt $bytes.Length) { throw 'Invalid ICO frame' }
    $signature = [BitConverter]::ToString($bytes,$offset,8)
    if ($signature -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Expected PNG-backed ICO frame' }
    $widthBytes = [byte[]]$bytes[($offset+16)..($offset+19)]
    $heightBytes = [byte[]]$bytes[($offset+20)..($offset+23)]
    [Array]::Reverse($widthBytes)
    [Array]::Reverse($heightBytes)
    if ([BitConverter]::ToUInt32($widthBytes,0) -ne $size -or
        [BitConverter]::ToUInt32($heightBytes,0) -ne $size) { throw 'PNG size does not match ICO directory' }
}
if (-not ('PortfolioIconResources' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PortfolioIconResources {
    [DllImport("shell32.dll", CharSet=CharSet.Unicode)]
    public static extern uint ExtractIconEx(string file, int index, IntPtr large, IntPtr small, uint count);
}
'@
}
$exe = (Resolve-Path -LiteralPath $ExecutablePath).Path
if ([PortfolioIconResources]::ExtractIconEx($exe,-1,[IntPtr]::Zero,[IntPtr]::Zero,0) -lt 1) {
    throw 'Executable has no embedded icon group'
}
Write-Host 'Seven ICO resolutions and executable icon resource verified (not a visual UI test).'
