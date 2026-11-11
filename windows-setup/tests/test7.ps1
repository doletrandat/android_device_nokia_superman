param([string]$T, [string]$Mode)
. "$PSScriptRoot/harness.ps1" -Kit "$T/kit"
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Collections.Generic;
public static class SupermanRawDisk {
    public static string Phone; public static List<string> Log = new List<string>();
    sealed class Dummy : IDisposable { public void Dispose() { Log.Add("unlock"); } }
    public static FileStream Open(string path, bool write) {
        if (path != @"\\.\PhysicalDrive3") throw new Exception("unexpected drive " + path);
        Log.Add((write ? "open-rw " : "open-ro ") + path);
        return new FileStream(Phone, FileMode.Open, write ? FileAccess.ReadWrite : FileAccess.Read, FileShare.ReadWrite, 512);
    }
    public static int ReadFull(Stream s, byte[] buf) { int n = 0; while (n < buf.Length) { int r = s.Read(buf, n, buf.Length - n); if (r <= 0) break; n += r; } return n; }
    public static IDisposable LockVolume(string path) { Log.Add("lock " + path); if (path.Contains("b5b5")) throw new Exception("Access is denied"); return new Dummy(); }
    public static void UpdateDisk(string path) { Log.Add("update " + path); }
}
"@
[SupermanRawDisk]::Phone = $(if ($env:PHONE_IMG) { $env:PHONE_IMG } else { "$T/phone.img" })
$dataVol = '\\?\Volume{11111111-2222-3333-4444-555555555555}\'
if ($Mode -eq 'busy') { $dataVol = '\\?\Volume{0b05b05b-0000-0000-0000-00000000b5b5}\'; }
$script:Disks = @(
    [pscustomobject]@{ Number = 0; FriendlyName = 'PC NVMe'; Size = 512GB; BusType = 'NVMe'; IsSystem = $true; IsBoot = $true; IsReadOnly = $false },
    [pscustomobject]@{ Number = 3; FriendlyName = 'Qualcomm MMC Storage USB Device'; Size = 15269888 * 512; BusType = 'USB'; IsSystem = $false; IsBoot = $false; IsReadOnly = $false })
$script:Parts = @(
    [pscustomobject]@{ DiskNumber = 0; PartitionNumber = 3; Size = 300GB; DriveLetter = [char]'C'; Offset = 1GB; AccessPaths = @('C:\') },
    [pscustomobject]@{ DiskNumber = 3; PartitionNumber = 25; Size = 32MB; DriveLetter = [char]'E'; Offset = 184320 * 512; AccessPaths = @('E:\', '\\?\Volume{eeeeeeee-0000-0000-0000-000000000000}\') },
    [pscustomobject]@{ DiskNumber = 3; PartitionNumber = 26; Size = 2451MB; DriveLetter = [char]'F'; Offset = 262144 * 512; AccessPaths = @('F:\', '\\?\Volume{ffffffff-0000-0000-0000-000000000000}\') },
    [pscustomobject]@{ DiskNumber = 3; PartitionNumber = 27; Size = 4872MB; DriveLetter = [char]'G'; Offset = 5292032 * 512; AccessPaths = @('G:\', $dataVol) })
function Get-Disk { param($Number) if ($PSBoundParameters.ContainsKey('Number')) { return $script:Disks | Where-Object { $_.Number -eq $Number } }; return $script:Disks }
function Get-Partition { param($DiskNumber, $PartitionNumber, $DriveLetter)
    $r = $script:Parts
    if ($PSBoundParameters.ContainsKey('DiskNumber')) { $r = $r | Where-Object { $_.DiskNumber -eq $DiskNumber } }
    if ($PSBoundParameters.ContainsKey('PartitionNumber')) { $r = $r | Where-Object { $_.PartitionNumber -eq $PartitionNumber } }
    if ($PSBoundParameters.ContainsKey('DriveLetter')) { $r = $r | Where-Object { "$($_.DriveLetter)" -eq "$DriveLetter" } }
    return $r }
function Start-Sleep { }
Copy-Item function:Get-EfiEspInfo function:Real-EfiEspInfo
function Get-EfiEspInfo([string]$root) { if ($root -eq 'E:\') { $i = Real-EfiEspInfo "$T/efi/"; $i.Root = $root; return $i }; return $null }
Copy-Item function:Install-BootFiles function:Real-InstallBootFiles
function Install-BootFiles([string]$efi) { if ($efi -ne 'E:') { throw "TEST: unexpected EFIESP $efi" }; Real-InstallBootFiles "$T/efi" }
function Invoke-ChildScript([string]$file, [string[]]$scriptArgs) { Write-Host "    (mock) run $(Split-Path -Leaf $file)" -ForegroundColor DarkYellow; if ($file -notlike '*install.ps1') { Copy-Item (Join-Path $BootDir 'payload/emmc_appsboot.mbn') "$T/efi/" }; return 0 }
function Do-CheckPc { Write-Host '  (PC check skipped in this test)' }
switch ($Mode) {
    'install'   { Queue-Answers 'y' 'y' '' 'y' '' 'n' 'INSTALL' '' }
    'withdata'  { Queue-Answers 'y' 'y' '' 'y' '' 'y' 'no' }
    'reinstall' { Queue-Answers 'y' 'y' '' 'y' '' 'INSTALL' '' }
    'busy'      { Queue-Answers 'y' 'y' '' 'y' '' '' 'INSTALL' }
    'installdata' { Queue-Answers 'y' 'y' '' 'y' '' 'y' 'INSTALL' '' }
    'restore'   { Queue-Answers 'B' '1' '' 'y' 'RESTORE' }
    'restorecancel' { Queue-Answers 'B' '1' '' 'y' 'no' }
}
function Get-FreeSpace([string]$folder) { return [long]20GB }
function Set-WindowsDefault([string]$efi) { Write-Host "    (mock) boot menu default -> Windows on $efi" -ForegroundColor DarkYellow; return $true }
try { if ($Mode -like 'restore*') { Do-BackToWindows } else { Do-InstallPhone } } catch { "THREW: $($_.Exception.Message)" }
"--- answers left: $($script:Answers.Count)"
"--- disk calls: " + (([SupermanRawDisk]::Log | Group-Object | ForEach-Object { "$($_.Count)x $($_.Name)" }) -join '; ')
