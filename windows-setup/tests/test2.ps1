param([string]$T)
. "$PSScriptRoot/harness.ps1" -Kit "$T/kit"
$ErrorActionPreference = 'Stop'
# ---- stand-in for the raw disk class: PhysicalDrive7 is a file ----
Add-Type -TypeDefinition @"
using System; using System.IO;
public static class SupermanRawDisk {
    public static string Map;
    public static FileStream Open(string path, bool write) {
        if (path != @"\\.\PhysicalDrive7") throw new Exception("unexpected drive " + path);
        return new FileStream(Map, FileMode.Open, write ? FileAccess.ReadWrite : FileAccess.Read, FileShare.ReadWrite, 1048576);
    }
    public static int ReadFull(Stream s, byte[] buf) { int n = 0; while (n < buf.Length) { int r = s.Read(buf, n, buf.Length - n); if (r <= 0) break; n += r; } return n; }
}
"@
[SupermanRawDisk]::Map = "$T/card.img"
$fs = [IO.File]::Create("$T/card.img"); $fs.SetLength(4GB); $junk = New-Object byte[] (4MB); (New-Object Random 1).NextBytes($junk); $fs.Write($junk, 0, $junk.Length); $fs.Dispose()
# ---- stand-ins for the Windows storage cmdlets ----
$script:Disks = @(
    [pscustomobject]@{ Number = 0; FriendlyName = 'PC NVMe'; Size = 512GB; BusType = 'NVMe'; IsSystem = $true; IsBoot = $true; IsReadOnly = $false },
    [pscustomobject]@{ Number = 3; FriendlyName = 'Qualcomm MMC Storage USB Device'; Size = 7818182656; BusType = 'USB'; IsSystem = $false; IsBoot = $false; IsReadOnly = $false },
    [pscustomobject]@{ Number = 5; FriendlyName = 'Tiny stick'; Size = 2GB; BusType = 'USB'; IsSystem = $false; IsBoot = $false; IsReadOnly = $false },
    [pscustomobject]@{ Number = 6; FriendlyName = 'External SATA'; Size = 1000GB; BusType = 'SATA'; IsSystem = $false; IsBoot = $false; IsReadOnly = $false },
    [pscustomobject]@{ Number = 7; FriendlyName = 'Generic SD Reader'; Size = 4GB; BusType = 'USB'; IsSystem = $false; IsBoot = $false; IsReadOnly = $false })
$script:Parts = @(
    [pscustomobject]@{ DiskNumber = 0; PartitionNumber = 1; Size = 100MB; DriveLetter = [char]0 },
    [pscustomobject]@{ DiskNumber = 0; PartitionNumber = 3; Size = 300GB; DriveLetter = [char]'C' },
    [pscustomobject]@{ DiskNumber = 3; PartitionNumber = 24; Size = 8MB; DriveLetter = [char]0 },
    [pscustomobject]@{ DiskNumber = 3; PartitionNumber = 25; Size = 32MB; DriveLetter = [char]0 },
    [pscustomobject]@{ DiskNumber = 3; PartitionNumber = 26; Size = 2500MB; DriveLetter = [char]'F' },
    [pscustomobject]@{ DiskNumber = 7; PartitionNumber = 1; Size = 3900MB; DriveLetter = [char]'G' })
function Get-Disk { param($Number) if ($PSBoundParameters.ContainsKey('Number')) { return $script:Disks | Where-Object { $_.Number -eq $Number } }; return $script:Disks }
function Get-Partition { param($DiskNumber, $PartitionNumber, $DriveLetter, $ErrorAction)
    $r = $script:Parts
    if ($PSBoundParameters.ContainsKey('DiskNumber')) { $r = $r | Where-Object { $_.DiskNumber -eq $DiskNumber } }
    if ($PSBoundParameters.ContainsKey('PartitionNumber')) { $r = $r | Where-Object { $_.PartitionNumber -eq $PartitionNumber } }
    if ($PSBoundParameters.ContainsKey('DriveLetter')) { $r = $r | Where-Object { "$($_.DriveLetter)" -eq "$DriveLetter" } }
    return $r }
function Get-Volume { param([Parameter(ValueFromPipeline = $true)]$InputObject, $DriveLetter)
    process {
        if ($DriveLetter) { return [pscustomobject]@{ FileSystemLabel = 'OLDCARD'; FileSystem = 'FAT32'; Size = 3900MB; SizeRemaining = 1000MB } }
        if ($InputObject.DiskNumber -eq 3 -and $InputObject.PartitionNumber -eq 25) { return [pscustomobject]@{ Path = '\\?\Volume{efiesp}\' } }
        if ($InputObject.DiskNumber -eq 3) { return [pscustomobject]@{ Path = '\\?\Volume{other}\' } }
        throw 'no volume' } }
function Add-PartitionAccessPath { param($DiskNumber, $PartitionNumber, [switch]$AssignDriveLetter) Write-Host "    (mock) drive letter E assigned to disk $DiskNumber partition $PartitionNumber" -ForegroundColor DarkYellow; ($script:Parts | Where-Object { $_.DiskNumber -eq $DiskNumber -and $_.PartitionNumber -eq $PartitionNumber }).DriveLetter = [char]'E' }
function Clear-Disk { param($Number, [switch]$RemoveData, [switch]$RemoveOEM, $Confirm, $ErrorAction) Write-Host "    (mock) Clear-Disk $Number" -ForegroundColor DarkYellow; if ($Number -ne 7) { throw 'TEST: wrong disk cleared' } }
function Start-Sleep { }
Copy-Item function:Get-EfiEspInfo function:Real-EfiEspInfo
function Get-EfiEspInfo([string]$root) { if ($root -in '\\?\Volume{efiesp}\', 'E:\') { $i = Real-EfiEspInfo "$T/efi/"; $i.Root = $root; return $i }; return $null }
Copy-Item function:Install-BootFiles function:Real-InstallBootFiles
function Install-BootFiles([string]$efi) { if ($efi -ne 'E:') { throw "TEST: unexpected EFIESP $efi" }; Real-InstallBootFiles "$T/efi" }
function Invoke-ChildScript([string]$file, [string[]]$scriptArgs) {
    Write-Host "    (mock) run $(Split-Path -Leaf $file) $($scriptArgs -join ' ')" -ForegroundColor DarkYellow
    if ($file -like '*install.ps1') { $d = Split-Path -Parent $file; Copy-Item "$d/bootshim.efi" "$T/efi/Windows/System32/Boot/"; Copy-Item "$d/Stage2.efi" "$T/efi/" }
    else { Copy-Item (Join-Path $BootDir 'payload/emmc_appsboot.mbn') "$T/efi/" }
    return 0 }

# ready y | unlocked y | part1 y | (Enter: mass storage) | confirm phone y | download LQS y | (Enter: next part) |
# part2 y | (Enter: card is in) | disk 7 | ERASE | (Enter: last part)
Queue-Answers 'y' 'y' 'y' '' 'y' 'y' '' 'y' '' '7' 'ERASE' ''
Do-InstallSd
"--- answers left: $($script:Answers.Count)"
"--- card first $ImageBytes bytes sha256:"
$s = [IO.File]::OpenRead("$T/card.img"); $sha = [Security.Cryptography.SHA256]::Create(); $buf = New-Object byte[] (1MB); $left = $ImageBytes
while ($left -gt 0) { $n = $s.Read($buf, 0, [int][Math]::Min([long]$buf.Length, [long]$left)); [void]$sha.TransformBlock($buf, 0, $n, $null, 0); $left -= $n }
[void]$sha.TransformFinalBlock($buf, 0, 0); $s.Dispose(); ([BitConverter]::ToString($sha.Hash)).Replace('-', '').ToLower()
"--- efi + tools + backups"; Get-ChildItem "$T/efi" -Name; Get-ChildItem "$T/kit/tools" -Name; Get-ChildItem "$T/kit/backups" -Recurse -Name
