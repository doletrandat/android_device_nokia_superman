<#
  Android 11 for the Nokia Lumia 730 (superman) - guided setup for Windows.
  Start it with Start-Here.cmd (double-click). It asks for Administrator rights by itself.

  What it offers, step by step and with checks:
    1. Install Android on the phone itself (boot shim, Android bootloader, Android on the internal storage)
    2. Try Android from a microSD card instead (optional; Windows Phone stays on the phone)
    3. Go back to Windows Phone (fresh with the stock FFU, or restore this tool's backup - experimental)
    4. What you need and where to get it (WPinternals, drivers, the FFU from lumiadb.com)
    5. Check that this PC has what is needed (cautions only)
    6. Check the files of this kit
    7. Advanced: move an Android that runs from the SD card onto the phone, over adb

  Every screen starts on a cleared window. This file is kept plain ASCII on purpose (see the UI helpers).
  Nothing is written without a clear question first. A log of every run is kept in the logs\ folder.
#>
#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

# ------------------------------------------------------------------------------------------------------------
# Administrator rights
# ------------------------------------------------------------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    try { Clear-Host } catch { }
    Write-Host ''
    Write-Host '  Android 11 for the Nokia Lumia 730 - setup' -ForegroundColor Cyan
    Write-Host ''
    Write-Host '  One thing first: this tool writes to your phone (and maybe an SD card),'
    Write-Host '  and Windows only allows that for an Administrator.'
    Write-Host ''
    Write-Host '  Press Enter, then click "Yes" when Windows asks. ' -ForegroundColor Yellow -NoNewline
    $Host.UI.ReadLine() | Out-Null
    try {
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $MyInvocation.MyCommand.Path + '"'))
    } catch {
        Write-Host ''
        Write-Host '  No Administrator rights, so nothing was done. Start it again whenever you like.' -ForegroundColor Red
        Write-Host '  Press Enter to close. ' -NoNewline
        $Host.UI.ReadLine() | Out-Null
    }
    exit
}

# ------------------------------------------------------------------------------------------------------------
# What this kit contains (sha256). The tool refuses to use a file that does not match.
# ------------------------------------------------------------------------------------------------------------
$Files      = Join-Path $Here 'files'
$Tools      = Join-Path $Here 'tools'
$Backups    = Join-Path $Here 'backups'
$Logs       = Join-Path $Here 'logs'
$ImageGz    = Join-Path $Files 'superman-alpha-sdcard.img.gz'
$ImageGzSha = 'c68c00307c08d3f82bcda465462bdbfdb13d91727b173ed1c7c462a0e26eafea'
$ImageSha   = 'aabd8c81ac3cae2845023cad465cc8fa47f1558a1121f0051b4d5f061328fc81'   # the unpacked image
$ImageBytes = 2751463424
$BootDir    = Join-Path $Files 'boot-installer'
$BootScript = Join-Path $BootDir 'Install-SupermanBoot.ps1'
$KitHashes  = [ordered]@{
    'files\superman-alpha-sdcard.img.gz'                    = $ImageGzSha
    'files\boot-installer\Install-SupermanBoot.ps1'         = 'd39a9007cb6ba33fd09cdcab10cad6e786f26b1b0f7369b9bc377991daa928fd'
    'files\boot-installer\payload\emmc_appsboot.mbn'        = 'b5f9d17926b40e49fca58eea5428943e7d8c12de999459e9c729d183fb9e1ec3'
    'files\internal-direct\boot.img.gz'                         = '045128c3a03ad97a3b1d1b65dd3002f7acab6104ac6ae377074e36de3a8f35b1'
    'files\internal-direct\system.img.gz'                       = '5c504dbb23cb07fd3741fdd104baf6eb958721f50c532c0e523da7b8f0e58e64'
    'files\internal-direct\data.img.gz'                         = 'd2891b773cad9533b434e0e9bc793a47bb873eb19dbb067ced1c4b65bf8ed42a'
    'files\internal\payload\boot-internal.img'              = '7b134610a18a551e1ad603b0225331cab5ebb656c75e469977ade427117d1d16'
    'files\internal\payload\fstab.superman.internal'        = '35c8679cea5a2a751877180b0ab0a288416f19bd9d391253549980e3338883e4'
    'files\internal\payload\fstab.superman.late.internal'   = '65f7664889cdb5e929fce7a7980f48e5f704c749895feb31ba24f77841fd0971'
    'files\internal\payload\superman-bootmenu'              = '65fa6780023a7258a7dad5f7baa54d9ac6d00f5aa4bb758fce0448f238675d70'
    'files\internal\payload\superman-gpt'                   = '4f2e348356859e805d009080f98b63c3ed4f02af47aeef1293471610b2ad3f97'
    'files\internal\payload\superman-install-internal.sh'   = '2bffe2f12499b5ea66b7607cf9fbeac567eddbdb3461cdb85cf2ccbedd1861f4'
}
# Ready-made partition images for the install on the phone itself (sha256 and size of the unpacked image).
$InternalImages = [ordered]@{
    boot   = @{ Name = 'Android boot';   Gz = 'files\internal-direct\boot.img.gz';   Sha = '35fe712c97c3acd03e360dbc5dc34f421c735de60ff7955d4e5e11443a2bc9c2'; Bytes = 66060288; Sparse = $false }
    system = @{ Name = 'Android system'; Gz = 'files\internal-direct\system.img.gz'; Sha = 'c49b2f31108c07579529f1678f72a99ecc722d777a482097226fcbef80132023'; Bytes = 2147483648; Sparse = $false }
    data   = @{ Name = 'empty data';     Gz = 'files\internal-direct\data.img.gz';   Sha = 'f99b18cf80ef773641aaf0af3a3209277d21e88a3ad6013b7d8511bc79b7ec94'; Bytes = 2684354560; Sparse = $true }
}
$DataMinSectors = 5242880      # the data image must fit into what is left of Windows Phone's Data partition
# Mainline4Lumia LumiaQuickStart (boot shim). Not part of this kit: no licence is stated for it, so the tool
# downloads it from its own GitHub repository (one fixed version) or uses a copy you put in tools\LumiaQuickStart.
$LqsCommit = '4ecbfb8c93ef8789207f470126825e3ac91090c3'
$LqsUrl    = "https://github.com/Mainline4Lumia/LumiaQuickStart/archive/$LqsCommit.zip"
$LqsHashes = @{
    'bootshim.efi' = '108b070195f59aac27ddd0ee1d2a806ec1e267d22c2a763d95dcac9c72912a18'
    'Stage2.efi'   = '6fe6e3a4d1cf52f397d0904372d6cfd412efd829e710658dedf988bc0d153108'
    'install.ps1'  = '306976f1f18b8f742582e1122ce0c37dd92b8206798480b2321ba2eed6977ea2'
}
$AdbUrl = 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip'
$Remote = '/data/local/tmp/superman-internal'

# ------------------------------------------------------------------------------------------------------------
# Small helpers for talking to the user
# ------------------------------------------------------------------------------------------------------------
# Line-drawing characters are built from their codes so this file stays plain ASCII (Windows PowerShell 5.1
# reads a script without a byte order mark in the PC's own code page).
$UiWidth = 84
$Ui = @{
    H = [string][char]0x2500; V = [string][char]0x2502; TL = [string][char]0x250C; TR = [string][char]0x2510
    BL = [string][char]0x2514; BR = [string][char]0x2518; Done = [string][char]0x221A; Now = [string][char]0x25CF
    Todo = [string][char]0x25CB; Arrow = [string][char]0x25BA
}
function Clear-Screen { try { Clear-Host } catch { } }
function Line { Write-Host ('  ' + ($Ui.H * $UiWidth)) -ForegroundColor DarkGray }
function Show-Banner {
    $name = 'Android 11 for the Nokia Lumia 730'; $tag = 'setup ' + [string][char]0x00B7 + ' alpha'
    $gap = $UiWidth - 2 - 2 - $name.Length - $tag.Length - 2
    Write-Host ''
    Write-Host ('  ' + $Ui.TL + ($Ui.H * ($UiWidth - 2)) + $Ui.TR) -ForegroundColor DarkCyan
    Write-Host ('  ' + $Ui.V + '  ') -ForegroundColor DarkCyan -NoNewline
    Write-Host $name -ForegroundColor White -NoNewline
    Write-Host ((' ' * $gap) + $tag + '  ') -ForegroundColor DarkGray -NoNewline
    Write-Host $Ui.V -ForegroundColor DarkCyan
    Write-Host ('  ' + $Ui.BL + ($Ui.H * ($UiWidth - 2)) + $Ui.BR) -ForegroundColor DarkCyan
}
function Show-Screen([string]$title, [string]$sub = '', [string[]]$steps = @(), [int]$current = 0) {
    # Every screen starts on a clean window: banner, title, an optional "where am I" line of steps.
    Clear-Screen
    Show-Banner
    Write-Host ''
    Write-Host "  $title" -ForegroundColor Cyan
    if ($sub) { Write-Host "  $sub" -ForegroundColor DarkGray }
    if ($steps.Count -gt 0) {
        Write-Host ''
        Write-Host '  ' -NoNewline
        for ($i = 1; $i -le $steps.Count; $i++) {
            if ($i -lt $current) { Write-Host ($Ui.Done + ' ' + $steps[$i - 1]) -ForegroundColor DarkGreen -NoNewline }
            elseif ($i -eq $current) { Write-Host ($Ui.Now + ' ' + $steps[$i - 1]) -ForegroundColor White -NoNewline }
            else { Write-Host ($Ui.Todo + ' ' + $steps[$i - 1]) -ForegroundColor DarkGray -NoNewline }
            if ($i -lt $steps.Count) { Write-Host ('  ' + ($Ui.H * 3) + '  ') -ForegroundColor DarkGray -NoNewline }
        }
        Write-Host ''
    }
    Line
    Write-Host ''
}
function Split-Lines([string]$t, [int]$width) {
    # Breaks a long line at spaces so nothing runs past the right edge of the screen.
    $out = @()
    while ($t.Length -gt $width) {
        $cut = $t.LastIndexOf(' ', $width)
        if ($cut -le 0) { break }
        $out += $t.Substring(0, $cut); $t = $t.Substring($cut + 1)
    }
    $out += $t
    return , $out
}
function Write-Marked([string]$mark, [string]$markColor, [string]$text, [string]$textColor = '') {
    # A short mark in the left margin, then the text, wrapped and lined up under itself.
    $lines = Split-Lines $text ($UiWidth + 2 - $mark.Length)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($i -eq 0) { Write-Host $mark -ForegroundColor $markColor -NoNewline } else { Write-Host (' ' * $mark.Length) -NoNewline }
        if ($textColor) { Write-Host $lines[$i] -ForegroundColor $textColor } else { Write-Host $lines[$i] }
    }
}
function Say([string]$t) { Write-Marked '  ' 'Gray' $t }
function Note([string]$t) { Write-Marked '  ' 'DarkGray' $t 'DarkGray' }
function Head([string]$t) { Write-Host ''; Write-Host "  $t" -ForegroundColor White }
function Item([string]$t) { Write-Marked '   -  ' 'DarkGray' $t }
function Step([string]$n, [string]$t) { Write-Marked ("   {0}  " -f $n) 'Cyan' $t }
function More([string]$t) { Write-Marked '      ' 'Gray' $t }
function Good([string]$t) { Write-Marked ('   ' + $Ui.Done + '  ') 'Green' $t 'Green' }
function Warn([string]$t) { Write-Marked '   !  ' 'Yellow' $t 'Yellow' }
function Bad([string]$t) { Write-Marked '   x  ' 'Red' $t 'Red' }
function Info([string]$t) { Write-Marked '   i  ' 'DarkCyan' $t }
function Field([string]$label, [string[]]$lines) {
    # A label in the left column, its text (one or more lines) to the right of it.
    Write-Host ('   ' + $label.PadRight(21)) -ForegroundColor Cyan -NoNewline
    Write-Host $lines[0]
    for ($i = 1; $i -lt $lines.Count; $i++) { Write-Host ((' ' * 24) + $lines[$i]) }
}
function Choice([string]$key, [string]$text, [string]$note = '', [string]$tag = '', [string]$tagColor = 'Green') {
    # One line of a menu: a key cap, what it does, and a quieter second line.
    Write-Host '   ' -NoNewline
    Write-Host " $key " -ForegroundColor Black -BackgroundColor Cyan -NoNewline
    Write-Host "  $text" -ForegroundColor White -NoNewline
    if ($tag) { Write-Host "   $tag" -ForegroundColor $tagColor -NoNewline }
    Write-Host ''
    if ($note) { Write-Host "        $note" -ForegroundColor DarkGray }
}
function Show-Danger([string[]]$lines) {
    # The one box that should make people stop and read.
    Write-Host ''
    Write-Host ('  ' + $Ui.TL + ($Ui.H * ($UiWidth - 2)) + $Ui.TR) -ForegroundColor Red
    $wrapped = @()
    foreach ($l in $lines) { $wrapped += (Split-Lines $l ($UiWidth - 4)) }
    foreach ($l in $wrapped) {
        Write-Host ('  ' + $Ui.V + ' ') -ForegroundColor Red -NoNewline
        Write-Host ($l.PadRight($UiWidth - 4)) -ForegroundColor Yellow -NoNewline
        Write-Host (' ' + $Ui.V) -ForegroundColor Red
    }
    Write-Host ('  ' + $Ui.BL + ($Ui.H * ($UiWidth - 2)) + $Ui.BR) -ForegroundColor Red
}
function Read-Raw {
    # The one place that reads the keyboard.
    $a = $Host.UI.ReadLine()
    if ($null -eq $a) { return '' }
    return [string]$a
}
function Read-Line([string]$prompt) {
    Write-Host "  $prompt " -ForegroundColor White -NoNewline
    Write-Host ($Ui.Arrow + ' ') -ForegroundColor Cyan -NoNewline
    return (Read-Raw)
}
function Pause-Enter([string]$t = 'Press Enter to continue') {
    Write-Host ''
    Write-Host "  $t " -ForegroundColor DarkGray -NoNewline
    Read-Raw | Out-Null
}
function Ask-YesNo([string]$q, [bool]$default = $true) {
    $hint = '[Y/n]'; if (-not $default) { $hint = '[y/N]' }
    while ($true) {
        $a = (Read-Line "$q $hint").Trim().ToLower()
        if ($a -eq '') { return $default }
        if ($a -in 'y', 'yes') { return $true }
        if ($a -in 'n', 'no') { return $false }
        Note 'Just Y or N, please.'
    }
}
function Ask-Choice([string]$q, [string[]]$allowed) {
    while ($true) {
        $a = (Read-Line $q).Trim().ToUpper()
        if ($allowed -contains $a) { return $a }
        Note ("That's not one of the options. Try: " + ($allowed -join ', '))
    }
}
function Ask-Word([string]$word, [string]$otherwise) {
    # For the steps that cannot be undone: the user types a whole word, in capitals.
    return ((Read-Line "Type $word to go ahead, or anything else to $otherwise").Trim() -ceq $word)
}
function Open-Web([string]$url) {
    try { Start-Process $url | Out-Null; Good "Opened $url in your browser." } catch { Say "Couldn't open the browser. The address is: $url" }
}
function Get-Sha([string]$path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }
function Format-Size([double]$bytes) {
    if ($bytes -ge 1GB) { return ('{0:N1} GB' -f ($bytes / 1GB)) }
    return ('{0:N0} MB' -f ($bytes / 1MB))
}
function Get-FreeSpace([string]$folder) {
    # Free bytes on the drive that holds this folder, or -1 when it cannot be read.
    try { return [long](New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($folder))).AvailableFreeSpace } catch { return [long]-1 }
}
function New-BackupFolder([string]$what) {
    $d = Join-Path $Backups ($what + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Path $d -Force | Out-Null
    return $d
}
function Test-KitFile([string]$rel) {
    $p = Join-Path $Here $rel
    if (-not (Test-Path -LiteralPath $p)) { throw "A file of this kit is missing: $rel" }
    $h = Get-Sha $p
    if ($h -ne $KitHashes[$rel]) { throw "A file of this kit is damaged or was changed: $rel`n      expected $($KitHashes[$rel])`n      found    $h" }
}
function Invoke-ChildScript([string]$file, [string[]]$scriptArgs) {
    # Runs another PowerShell script in its own process and shows its output. Returns its exit code.
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $file) + $scriptArgs
    & powershell.exe @a | Out-Host
    return $LASTEXITCODE
}
function Get-WebFile([string]$url, [string]$dest) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'   # the progress bar makes downloads very slow
    try { Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing } finally { $ProgressPreference = $old }
}

# ------------------------------------------------------------------------------------------------------------
# "Is this PC ready?" - cautions only. Nothing here ever stops the setup.
# ------------------------------------------------------------------------------------------------------------
function Find-FirstFile([string[]]$folders, [string]$filter, [int]$depth) {
    foreach ($f in $folders) {
        if (-not $f -or -not (Test-Path -LiteralPath $f)) { continue }
        try {
            $hit = Get-ChildItem -LiteralPath $f -Filter $filter -File -Recurse -Depth $depth -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        } catch { }
    }
    return $null
}
function Show-WhereToGet {
    # The short "shopping list". Facts checked in November 2026.
    Head 'Where everything comes from'
    Field 'WPinternals' @('github.com/ReneLergner/WPinternals  (under Releases)')
    Field 'Lumia USB drivers' @('Install Windows Device Recovery Tool (WDRT) once.', 'It can''t download firmware any more, but it still', 'brings the drivers. lumiadb.com links an offline', 'installer ("WDRT" in the top bar).')
    Field 'Stock FFU' @('lumiadb.com -> Lumia 730 Dual SIM (RM-1040) -> the FFU', 'for your product code. The code is on the label under', 'the battery ("CODE: 059W..."). Not listed? LumiaDB', 'says any RM-1040 FFU will do.')
    Field 'Files for unlocking' @('Emergency files: same LumiaDB page. "Donor FFU": top', 'bar. The guide "How to Unlock Your Lumia''s Bootloader"', 'at lumiadb.com/guides walks you through it.')
    Write-Host ''
    Note 'Heads-up: the "Download" tab inside WPinternals no longer works. Microsoft switched those servers off in 2024, so the FFU has to come from LumiaDB.'
}
function Do-WhatYouNeed {
    Show-Screen 'What you need, and where to get it' 'Nothing is installed or changed here.'
    Head 'For any install'
    Item 'A Nokia Lumia 730 (RM-1040). The 735 is a different phone and won''t work.'
    Item 'Its bootloader unlocked with WPinternals.'
    Item 'A Windows 10 or 11 PC, a USB cable, and the battery above 50 %.'
    Item 'The stock FFU of your phone, saved somewhere safe. It''s your ticket back to Windows Phone, so don''t skip it.'
    Head 'Only for the microSD option'
    Item 'A microSD card of 4 GB or more (it gets erased) and a card reader.'
    Show-WhereToGet
    Write-Host ''
    if (Ask-YesNo 'Open lumiadb.com in your browser now?' $false) { Open-Web 'https://lumiadb.com' }
}
function Do-CheckPc {
    $cautions = 0
    $userDirs = @()
    foreach ($n in 'Desktop', 'Downloads', 'Documents') { if ($env:USERPROFILE) { $userDirs += (Join-Path $env:USERPROFILE $n) } }

    # Windows and its built-in tools
    try {
        $v = [Environment]::OSVersion.Version
        if ($v.Major -ge 10) { Good "Windows $($v.Major) (build $($v.Build))" } else { Warn "Windows version $v - this was written for Windows 10 and 11, so some things may not work."; $cautions++ }
    } catch { }
    foreach ($exe in 'bcdedit.exe', 'diskpart.exe') {
        if (Get-Command $exe -ErrorAction SilentlyContinue) { Good "$exe is there" } else { Warn "$exe is missing. It's part of Windows and is needed to prepare the phone / the SD card."; $cautions++ }
    }

    # Room for backups, downloads and logs next to this tool
    $freeHere = Get-FreeSpace $Here
    if ($freeHere -ge 300MB) { Good ("Free space next to this tool: " + (Format-Size $freeHere)) }
    elseif ($freeHere -ge 0) { Warn ("Only " + (Format-Size $freeHere) + " free next to this tool. Backups and downloads want about 300 MB."); $cautions++ }

    # Internet (only for the two small downloads)
    $online = $false
    try {
        $tcp = New-Object Net.Sockets.TcpClient
        $wait = $tcp.BeginConnect('github.com', 443, $null, $null)
        if ($wait.AsyncWaitHandle.WaitOne(4000) -and $tcp.Connected) { $online = $true }
        $tcp.Close()
    } catch { }
    if ($online) { Good 'Internet works (needed once, for a 1 MB download)' }
    else { Warn 'No internet. The boot shim (about 1 MB) gets downloaded the first time, so you''ll need it then.'; $cautions++ }

    # Lumia USB drivers - they come with Windows Device Recovery Tool (WDRT). WPinternals needs them to see the phone.
    $drivers = $null
    try {
        $names = @()
        foreach ($k in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*') {
            $names += @(Get-ItemProperty -Path $k -ErrorAction SilentlyContinue | ForEach-Object { "$($_.DisplayName)" })
        }
        $m = @($names | Where-Object { $_ -match 'Windows Device Recovery Tool|Windows Phone Recovery Tool|Lumia Software Recovery|WinUSB.*(Nokia|Lumia|Windows Phone)|Care Suite' } | Select-Object -First 1)
        if ($m.Count -gt 0) { $drivers = $m[0] }
    } catch { }
    if (-not $drivers) {
        try {
            $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
            try { $pnp = & pnputil.exe /enum-drivers 2>$null | Out-String } finally { $ErrorActionPreference = $old }
            if ($pnp -match '(?i)Nokia|Microsoft Mobile') { $drivers = 'a Nokia / Microsoft Mobile driver in the Windows driver store' }
        } catch { }
    }
    $missing = $false
    if ($drivers) { Good "Lumia USB drivers ($drivers)" }
    else {
        Warn 'Lumia USB drivers: couldn''t find them. Without them WPinternals can''t see the phone. They come with Windows Device Recovery Tool (WDRT). If WPinternals already sees your phone, you''re fine - ignore this.'
        $cautions++; $missing = $true
    }

    # WPinternals (a portable program, so this is only a look in the usual places)
    $wpi = $null
    try { $proc = Get-Process -Name 'WPinternals' -ErrorAction SilentlyContinue | Select-Object -First 1; if ($proc) { $wpi = 'it''s running' } } catch { }
    if (-not $wpi) {
        $wpiDirs = @() + $userDirs + @((Split-Path -Parent $Here))
        foreach ($d in $env:ProgramFiles, ${env:ProgramFiles(x86)}) { if ($d) { $wpiDirs += $d } }
        $f = Find-FirstFile $wpiDirs 'WPinternals.exe' 3
        if ($f) { $wpi = $f }
    }
    if ($wpi) { Good "WPinternals ($wpi)" }
    else {
        Warn 'WPinternals: didn''t see it in the usual folders. You need it to unlock the phone and to switch it to Mass Storage Mode.'
        $cautions++; $missing = $true
    }

    # Stock FFU - the way back to Windows Phone
    $ffuDirs = @() + $userDirs
    if ($env:ProgramData) { $ffuDirs += (Join-Path $env:ProgramData 'WPinternals\Repository'); $ffuDirs += (Join-Path $env:ProgramData 'Microsoft\Packages\Products') }
    $ffu = Find-FirstFile $ffuDirs '*.ffu' 4
    if ($ffu) { Good "A stock FFU ($ffu)" }
    else {
        Warn 'Stock FFU: no .ffu file in the usual folders. It''s your way back to Windows Phone, so make sure you have the one for your phone before you install.'
        $cautions++; $missing = $true
    }

    # adb - only for the advanced "move from SD card" part
    $adb = $null
    $local = Join-Path $Tools 'platform-tools\adb.exe'
    if (Test-Path -LiteralPath $local) { $adb = $local } else { $c = Get-Command adb.exe -ErrorAction SilentlyContinue; if ($c) { $adb = $c.Source } }
    if ($adb) { Good "adb ($adb)" } else { Info 'adb: not here, and that''s fine. Only the advanced option uses it, and offers to fetch it.' }

    Write-Host ''
    if ($cautions -eq 0) { Good 'This PC looks ready.' }
    else {
        Warn "$cautions thing(s) worth a look. They're cautions, not errors: this check only looks in"
        More 'the usual places, so it can miss things. You can carry on anyway.'
        if ($missing) { Show-WhereToGet }
    }
}

# ------------------------------------------------------------------------------------------------------------
# Step: find the phone's EFIESP volume (phone in Mass Storage Mode)
# ------------------------------------------------------------------------------------------------------------
function Get-EfiEspInfo([string]$root) {
    foreach ($p in 'EFI\Microsoft\Boot\BCD', 'efi\boot\bootarm.efi', 'Windows\System32\Boot\mobilestartup.efi') {
        if (-not (Test-Path -LiteralPath (Join-Path $root $p))) { return $null }
    }
    $dsm = @(Get-ChildItem -LiteralPath (Join-Path $root 'Windows\Packages\DsmFiles') -Filter 'Nokia.DEVICE_*' -ErrorAction SilentlyContinue)
    $device = '(no device package found)'
    $isSuperman = $false
    foreach ($d in $dsm) {
        if ($d.Name -like 'Nokia.DEVICE_SUPERMAN*') { $isSuperman = $true; $device = $d.Name }
    }
    if (-not $isSuperman -and $dsm.Count -gt 0) { $device = $dsm[0].Name }
    return [pscustomobject]@{ Root = $root; Device = $device; IsSuperman = $isSuperman }
}
function Find-EfiEsp {
    $hits = @()
    foreach ($part in @(Get-Partition -ErrorAction SilentlyContinue)) {
        if ($part.Size -gt 512MB -or $part.Size -lt 8MB) { continue }          # EFIESP is small (32-64 MB)
        $disk = Get-Disk -Number $part.DiskNumber -ErrorAction SilentlyContinue
        if (-not $disk -or $disk.IsSystem -or $disk.IsBoot) { continue }       # never this PC's own disks
        $letter = ("$($part.DriveLetter)").Trim([char]0).Trim()
        $root = $null
        if ($letter -match '^[A-Za-z]$') { $root = $letter + ':\' }
        else {
            $letter = ''
            $vol = $null
            try { $vol = $part | Get-Volume -ErrorAction Stop } catch { }
            if ($vol -and $vol.Path) { $root = $vol.Path }
        }
        if (-not $root) { continue }
        $info = $null
        try { $info = Get-EfiEspInfo $root } catch { }
        if ($info) {
            $hits += [pscustomobject]@{ Letter = $letter; Root = $root; Device = $info.Device; IsSuperman = $info.IsSuperman
                Size = $part.Size; DiskNumber = $part.DiskNumber; PartitionNumber = $part.PartitionNumber
                DiskName = $disk.FriendlyName; DiskSize = $disk.Size; Bus = "$($disk.BusType)" }
        }
    }
    return $hits
}
function Select-EfiEsp {
    # Returns a drive letter such as 'E:' or $null when the user gives up.
    while ($true) {
        Write-Host ''
        Say 'Looking for your phone...'
        $hits = @(Find-EfiEsp)
        if ($hits.Count -eq 0) {
            Warn 'Couldn''t find the phone yet. A few things to check:'
            Item 'WPinternals says the phone is in Mass Storage Mode'
            Item 'the USB cable is in, and Windows shows new drives (MainOS, Data, ...)'
            Item 'you clicked Cancel on every "You need to format the disk" window'
            $a = Ask-Choice 'R = look again   M = I''ll type the drive letter   Q = back to the menu' @('R', 'M', 'Q')
            if ($a -eq 'Q') { return $null }
            if ($a -eq 'M') {
                $l = (Read-Line 'Drive letter of the phone''s EFIESP drive (for example E)').Trim().TrimEnd(':').ToUpper()
                if ($l -notmatch '^[A-Z]$') { Warn 'That isn''t a drive letter.'; continue }
                $info = $null
                try { $info = Get-EfiEspInfo ($l + ':\') } catch { }
                if (-not $info) { Bad "$l`: doesn't look like a Lumia's EFIESP drive (its boot files aren't there)."; continue }
                $hits = @([pscustomobject]@{ Letter = $l; Root = $l + ':\'; Device = $info.Device; IsSuperman = $info.IsSuperman
                        Size = 0; DiskNumber = -1; PartitionNumber = -1; DiskName = '(typed by you)'; DiskSize = 0; Bus = '?' })
            } else { continue }
        }
        $pick = $hits[0]
        if ($hits.Count -gt 1) {
            Warn 'Found more than one phone. Is a second Lumia plugged in?'
            for ($i = 0; $i -lt $hits.Count; $i++) { Step ($i + 1) ("{0}  {1}  on {2}" -f $hits[$i].Root, $hits[$i].Device, $hits[$i].DiskName) }
            $n = Ask-Choice 'Which one is it? (number)' @(1..$hits.Count | ForEach-Object { "$_" })
            $pick = $hits[[int]$n - 1]
        }
        if (-not $pick.Letter) {
            Note 'Windows gave that drive no letter, and the boot menu editor needs one. Assigning one...'
            Add-PartitionAccessPath -DiskNumber $pick.DiskNumber -PartitionNumber $pick.PartitionNumber -AssignDriveLetter | Out-Null
            Start-Sleep -Seconds 2
            $p2 = Get-Partition -DiskNumber $pick.DiskNumber -PartitionNumber $pick.PartitionNumber
            $pick.Letter = ("$($p2.DriveLetter)").Trim([char]0).Trim()
            if ($pick.Letter -notmatch '^[A-Za-z]$') { throw 'Windows did not assign a drive letter to the EFIESP volume.' }
            $pick.Root = $pick.Letter + ':\'
        }
        Head 'Found it'
        Field 'Drive' @(("{0}:   (the phone's small boot drive, EFIESP)" -f $pick.Letter.ToUpper()))
        Field 'Phone' @($pick.Device)
        if ($pick.DiskNumber -ge 0) {
            Field 'Drive size' @((Format-Size $pick.Size))
            Field 'On disk' @(("{0} ({1}, {2})" -f $pick.DiskName, (Format-Size $pick.DiskSize), $pick.Bus))
        }
        Write-Host ''
        if (-not $pick.IsSuperman) {
            Bad 'That''s a Lumia, but not a Lumia 730 (no Nokia.DEVICE_SUPERMAN package on it). This Android only runs on the 730, so nothing was touched.'
            if (-not (Ask-YesNo 'Look again?')) { return $null }
            continue
        }
        if (Ask-YesNo ("Is {0}: your Lumia 730? The boot files get written there." -f $pick.Letter.ToUpper())) {
            return ($pick.Letter.ToUpper() + ':')
        }
        if (-not (Ask-YesNo 'Look again?')) { return $null }
    }
}

# ------------------------------------------------------------------------------------------------------------
# Step: boot shim (LumiaQuickStart) and Android bootloader
# ------------------------------------------------------------------------------------------------------------
function Get-QuickStart {
    # Returns the folder that holds LumiaQuickStart's install.ps1, downloading it if needed.
    $candidates = @(Get-ChildItem -LiteralPath $Tools -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'LumiaQuickStart*' })
    foreach ($c in $candidates) {
        if ((Test-Path -LiteralPath (Join-Path $c.FullName 'install.ps1')) -and (Test-Path -LiteralPath (Join-Path $c.FullName 'bootshim.efi'))) { return $c.FullName }
    }
    Write-Host ''
    Say 'One small download is needed: the "boot shim" from the Mainline4Lumia project (LumiaQuickStart). It lets the phone start something other than Windows Phone. It isn''t ours to ship, so it comes straight from their GitHub page (about 1 MB):'
    Note "  $LqsUrl"
    if (-not (Ask-YesNo 'Download it now?')) {
        Say 'No problem. You can also download it yourself and unpack it into:'
        Note ("  " + (Join-Path $Tools 'LumiaQuickStart'))
        return $null
    }
    New-Item -ItemType Directory -Path $Tools -Force | Out-Null
    $zip = Join-Path $Tools 'LumiaQuickStart.zip'
    Get-WebFile $LqsUrl $zip
    Expand-Archive -LiteralPath $zip -DestinationPath $Tools -Force
    Remove-Item -LiteralPath $zip -Force
    $dir = Join-Path $Tools ("LumiaQuickStart-" + $LqsCommit)
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'install.ps1'))) { throw 'The download did not contain what was expected.' }
    Good 'Downloaded.'
    return $dir
}
function Install-BootFiles([string]$efi) {
    # $efi is like 'E:'
    $root = $efi + '\'
    $shim = Join-Path $root 'Windows\System32\Boot\bootshim.efi'
    $stage2 = Join-Path $root 'Stage2.efi'
    $backup = New-BackupFolder 'efiesp'
    Copy-Item -LiteralPath (Join-Path $root 'EFI\Microsoft\Boot\BCD') -Destination (Join-Path $backup 'BCD.before') -Force
    Good "Saved a copy of the phone's boot menu first: $backup"

    # --- boot shim ---
    if ((Test-Path -LiteralPath $shim) -and (Test-Path -LiteralPath $stage2)) {
        Good 'The boot shim is already on the phone, so that part is done.'
        if ((Get-Sha $shim) -ne $LqsHashes['bootshim.efi'] -or (Get-Sha $stage2) -ne $LqsHashes['Stage2.efi']) {
            Warn 'It''s a different version from the one this kit was tested with. Leaving it alone.'
        }
    } else {
        $lqs = Get-QuickStart
        if (-not $lqs) { throw 'The install can''t go on without the boot shim.' }
        foreach ($n in 'bootshim.efi', 'Stage2.efi') {
            if ((Get-Sha (Join-Path $lqs $n)) -ne $LqsHashes[$n]) { throw "LumiaQuickStart's $n is not the version this kit was tested with. Stopping to be safe." }
        }
        if ((Get-Sha (Join-Path $lqs 'install.ps1')) -ne $LqsHashes['install.ps1']) {
            Warn "LumiaQuickStart's install.ps1 is a different version than the tested one."
            if (-not (Ask-YesNo 'Run it anyway?' $false)) { throw 'Stopped before the boot shim was installed.' }
        }
        Say 'Putting the boot shim on the phone...'
        $rc = Invoke-ChildScript (Join-Path $lqs 'install.ps1') @('-efiesp_location', $efi)
        if ($rc -ne 0) { throw "LumiaQuickStart stopped with an error (code $rc). See the messages above." }
        if (-not ((Test-Path -LiteralPath $shim) -and (Test-Path -LiteralPath $stage2))) { throw 'LumiaQuickStart ran, but the boot shim files are not on the phone.' }
        Good 'Boot shim is on.'
    }

    # --- Android bootloader + boot menu default ---
    Say 'Adding the Android bootloader and making it the default in the boot menu...'
    $rc = Invoke-ChildScript $BootScript @('-EfiEsp', $efi)
    if ($rc -ne 0) { throw "The boot installer stopped with an error (code $rc). See the messages above. The phone's boot menu backup is in $backup" }
    $lk = Join-Path $root 'emmc_appsboot.mbn'
    if ((Get-Sha $lk) -ne $KitHashes['files\boot-installer\payload\emmc_appsboot.mbn']) { throw 'The Android bootloader on the phone does not match after copying.' }
    try { Write-VolumeCache -DriveLetter $efi.Substring(0, 1) | Out-Null } catch { }
    Good 'Android bootloader is on and checked. The phone now starts it by default.'
}

# ------------------------------------------------------------------------------------------------------------
# Step: write the SD card
# ------------------------------------------------------------------------------------------------------------
$RawSource = @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class SupermanRawDisk {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize, IntPtr outBuf, uint outSize, out uint returned, IntPtr overlapped);
    // Raw access to \\.\PhysicalDriveN. The stream's own buffer is one sector, so every read and write of a
    // whole number of sectors goes to the device exactly as asked.
    public static FileStream Open(string path, bool write) {
        uint access = write ? 0xC0000000u : 0x80000000u;
        SafeFileHandle h = CreateFile(path, access, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return new FileStream(h, write ? FileAccess.ReadWrite : FileAccess.Read, 512, false);
    }
    public static int ReadFull(Stream s, byte[] buf) {
        int n = 0;
        while (n < buf.Length) { int r = s.Read(buf, n, buf.Length - n); if (r <= 0) break; n += r; }
        return n;
    }
    // Locks and dismounts a volume (\\.\Volume{guid}); raw writes into its sectors are allowed while the returned
    // handle stays open. Throws if another program keeps the volume busy.
    public static IDisposable LockVolume(string path) {
        SafeFileHandle h = CreateFile(path, 0xC0000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        uint r; bool locked = false; int err = 0;
        for (int i = 0; i < 20 && !locked; i++) {
            locked = DeviceIoControl(h, 0x00090018, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);   // FSCTL_LOCK_VOLUME
            if (!locked) { err = Marshal.GetLastWin32Error(); System.Threading.Thread.Sleep(500); }
        }
        if (!locked) { h.Dispose(); throw new System.ComponentModel.Win32Exception(err); }
        DeviceIoControl(h, 0x00090020, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);                 // FSCTL_DISMOUNT_VOLUME
        return h;
    }
    // Asks Windows to read the partition table again (IOCTL_DISK_UPDATE_PROPERTIES).
    public static void UpdateDisk(string path) {
        using (SafeFileHandle h = CreateFile(path, 0xC0000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) return;
            uint r; DeviceIoControl(h, 0x00070140, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);
        }
    }
}
'@
$GptSource = @'
// Partition-table planner for the Lumia 730 internal Android install. A port of superman-gpt.c ("plan"):
// only Windows Phone's Data partition is re-used; every other entry stays byte for byte.
//   entry 27 "Data" -> "cache" (64 MiB, boot), entry 30 -> "android_system" (2 GiB), entry 31 -> "android_data" (rest)
// primary = LBA 0..33 (protective MBR, header, 32 entry sectors); backup = the last 33 LBAs. 512-byte sectors.
using System;
using System.Text;
public sealed class SupermanGptPlan {
    public string Layout;                  // "windows" (to be converted) or "android" (already converted)
    public byte[] NewPrimary, NewBackup;   // null when the layout is already "android"
    public long Total, BootFirst, BootLast, SystemFirst, SystemLast, DataFirst, DataLast, OldDataFirst, OldDataLast;
}
public static class SupermanGpt {
    const int S = 512; const int Pri = 34 * S; const int Bak = 33 * S; const long MiB = 2048;
    static uint G32(byte[] b, int o) { return (uint)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24); }
    static ulong G64(byte[] b, int o) { return G32(b, o) | (ulong)G32(b, o + 4) << 32; }
    static void P32(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }
    static void P64(byte[] b, int o, ulong v) { P32(b, o, (uint)v); P32(b, o + 4, (uint)(v >> 32)); }
    static uint Crc32(byte[] b, int o, int n) {
        uint c = 0xffffffffu;
        for (int i = 0; i < n; i++) { c ^= b[o + i]; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? (c >> 1) ^ 0xedb88320u : c >> 1; }
        return ~c;
    }
    static bool Same(byte[] a, int ao, byte[] b, int bo, int n) { for (int i = 0; i < n; i++) if (a[ao + i] != b[bo + i]) return false; return true; }
    // GUID bytes in a GPT are little-endian in the first three fields; RFC 4122 is big-endian. The swap is its own inverse.
    static void LeBe(byte[] i, int io, byte[] o, int oo) {
        o[oo] = i[io + 3]; o[oo + 1] = i[io + 2]; o[oo + 2] = i[io + 1]; o[oo + 3] = i[io]; o[oo + 4] = i[io + 5]; o[oo + 5] = i[io + 4];
        o[oo + 6] = i[io + 7]; o[oo + 7] = i[io + 6]; Array.Copy(i, io + 8, o, oo + 8, 8);
    }
    static void Uuid5(byte[] nsLe, int nsOff, string name, byte[] dst, int dOff) {
        byte[] nm = Encoding.ASCII.GetBytes(name); byte[] buf = new byte[16 + nm.Length];
        LeBe(nsLe, nsOff, buf, 0); Array.Copy(nm, 0, buf, 16, nm.Length);
        byte[] dig; using (var sha = System.Security.Cryptography.SHA1.Create()) dig = sha.ComputeHash(buf);
        byte[] be = new byte[16]; Array.Copy(dig, be, 16);
        be[6] = (byte)((be[6] & 0x0f) | 0x50); be[8] = (byte)((be[8] & 0x3f) | 0x80);
        LeBe(be, 0, dst, dOff);
    }
    sealed class Hdr { public ulong My, Alt, First, Last, Elba; public uint Count, Rev; public byte[] Disk = new byte[16]; }
    static Hdr Parse(byte[] b, int h, int ents) {
        if (Encoding.ASCII.GetString(b, h, 8) != "EFI PART" || G32(b, h + 12) != 92 || G32(b, h + 84) != 128) return null;
        byte[] tmp = new byte[92]; Array.Copy(b, h, tmp, 0, 92); P32(tmp, 16, 0);
        if (Crc32(tmp, 0, 92) != G32(b, h + 16)) return null;
        Hdr o = new Hdr(); o.Rev = G32(b, h + 8); o.My = G64(b, h + 24); o.Alt = G64(b, h + 32); o.First = G64(b, h + 40); o.Last = G64(b, h + 48);
        Array.Copy(b, h + 56, o.Disk, 0, 16); o.Elba = G64(b, h + 72); o.Count = G32(b, h + 80);
        if (o.Count == 0 || o.Count > 128) return null;
        if (Crc32(b, ents, (int)o.Count * 128) != G32(b, h + 88)) return null;
        return o;
    }
    static int Ent(int idx) { return 2 * S + (idx - 1) * 128; }               // offset of entry idx (1-based) in a primary buffer
    static bool IsEmpty(byte[] b, int e) { for (int i = 0; i < 16; i++) if (b[e + i] != 0) return false; return true; }
    static string Name(byte[] b, int e) {
        var sb = new StringBuilder();
        for (int i = 0; i < 36; i++) { int c = b[e + 56 + 2 * i] | b[e + 57 + 2 * i] << 8; if (c == 0) break; sb.Append((char)c); }
        return sb.ToString();
    }
    static void PutName(byte[] b, int e, string s) { Array.Clear(b, e + 56, 72); for (int i = 0; i < s.Length; i++) b[e + 56 + 2 * i] = (byte)s[i]; }
    static Hdr Check(byte[] p, byte[] k, long total) {
        if (p == null || k == null || p.Length != Pri || k.Length != Bak) throw new Exception("partition table buffers have the wrong size");
        Hdr hp = Parse(p, S, 2 * S); if (hp == null) throw new Exception("primary GPT header or entries invalid");
        Hdr hb = Parse(k, 32 * S, 0); if (hb == null) throw new Exception("backup GPT header or entries invalid");
        if (hp.My != 1 || hp.Alt != (ulong)total - 1 || hp.Elba != 2) throw new Exception("primary GPT not where expected");
        if (hb.My != (ulong)total - 1 || hb.Alt != 1 || hb.Elba != (ulong)total - 33) throw new Exception("backup GPT not where expected");
        if (!Same(p, 2 * S, k, 0, 32 * S) || !Same(hp.Disk, 0, hb.Disk, 0, 16) || hp.Count != hb.Count) throw new Exception("primary and backup GPT differ");
        if (p[510] != 0x55 || p[511] != 0xaa || p[450] != 0xee) throw new Exception("no protective MBR");
        if (hp.Count < 31) throw new Exception("partition table has fewer than 31 entries");
        return hp;
    }
    static string LayoutOf(byte[] p) {
        if (Name(p, Ent(26)) == "MainOS" && Name(p, Ent(27)) == "Data" && IsEmpty(p, Ent(30)) && IsEmpty(p, Ent(31))) return "windows";
        if (Name(p, Ent(26)) == "MainOS" && Name(p, Ent(27)) == "cache" && Name(p, Ent(30)) == "android_system" && Name(p, Ent(31)) == "android_data") return "android";
        return "other";
    }
    static void BuildHdr(byte[] dst, int o, Hdr h, byte[] ents, int eo, ulong my, ulong alt, ulong elba) {
        Array.Clear(dst, o, S);
        Array.Copy(Encoding.ASCII.GetBytes("EFI PART"), 0, dst, o, 8); P32(dst, o + 8, h.Rev); P32(dst, o + 12, 92);
        P64(dst, o + 24, my); P64(dst, o + 32, alt); P64(dst, o + 40, h.First); P64(dst, o + 48, h.Last);
        Array.Copy(h.Disk, 0, dst, o + 56, 16); P64(dst, o + 72, elba); P32(dst, o + 80, h.Count); P32(dst, o + 84, 128);
        P32(dst, o + 88, Crc32(ents, eo, (int)h.Count * 128));
        P32(dst, o + 16, Crc32(dst, o, 92));
    }
    // First and last sector of the partition with this name, or null.
    public static long[] Find(byte[] primary, string name) {
        Hdr h = Parse(primary, S, 2 * S); if (h == null) throw new Exception("primary GPT header or entries invalid");
        for (int i = 1; i <= (int)h.Count; i++) { int e = Ent(i); if (!IsEmpty(primary, e) && Name(primary, e) == name) return new long[] { (long)G64(primary, e + 32), (long)G64(primary, e + 40) }; }
        return null;
    }
    public static SupermanGptPlan Plan(byte[] pri, byte[] bak, long total, long minDataSectors) {
        Hdr h = Check(pri, bak, total);
        var r = new SupermanGptPlan(); r.Total = total; r.Layout = LayoutOf(pri);
        if (r.Layout == "android") {
            r.BootFirst = (long)G64(pri, Ent(27) + 32); r.BootLast = (long)G64(pri, Ent(27) + 40);
            r.SystemFirst = (long)G64(pri, Ent(30) + 32); r.SystemLast = (long)G64(pri, Ent(30) + 40);
            r.DataFirst = (long)G64(pri, Ent(31) + 32); r.DataLast = (long)G64(pri, Ent(31) + 40);
            r.OldDataFirst = r.BootFirst; r.OldDataLast = r.DataLast;
            if (r.BootLast - r.BootFirst + 1 != 64 * MiB || r.SystemFirst != r.BootLast + 1 || r.SystemLast - r.SystemFirst + 1 != 2048 * MiB
                || r.DataFirst != r.SystemLast + 1 || r.DataLast - r.DataFirst + 1 < minDataSectors)
                throw new Exception("the Android partitions on this phone are not the ones this tool makes");
            return r;
        }
        if (r.Layout != "windows") throw new Exception("not the Windows Phone layout (MainOS 26, Data 27, 30/31 free)");
        int data = Ent(27);
        ulong dfirst = G64(pri, data + 32), dlast = G64(pri, data + 40);
        if (dlast != h.Last || dfirst % (ulong)MiB != 0) throw new Exception("Data is not the last, 1 MiB-aligned partition");
        for (int i = 1; i <= (int)h.Count; i++) {
            int e = Ent(i); if (i == 27 || IsEmpty(pri, e)) continue;
            if (G64(pri, e + 40) >= dfirst) throw new Exception("another partition lies inside or after Data");
        }
        ulong bfirst = dfirst, blast = bfirst + 64 * (ulong)MiB - 1, sfirst = blast + 1, slast = sfirst + 2048 * (ulong)MiB - 1, afirst = slast + 1, alast = dlast;
        if (alast < afirst + 1024 * (ulong)MiB || alast - afirst + 1 < (ulong)minDataSectors) throw new Exception("Data is too small for the Android layout");
        byte[] linuxFs = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
        byte[] np = (byte[])pri.Clone();
        int[] idx = { 27, 30, 31 }; string[] names = { "cache", "android_system", "android_data" };
        ulong[] f = { bfirst, sfirst, afirst }, l = { blast, slast, alast };
        for (int i = 0; i < 3; i++) {
            int e = Ent(idx[i]); Array.Clear(np, e, 128); Array.Copy(linuxFs, 0, np, e, 16);
            Uuid5(h.Disk, 0, "superman-android/" + names[i], np, e + 16);
            P64(np, e + 32, f[i]); P64(np, e + 40, l[i]); PutName(np, e, names[i]);
        }
        // Protective MBR: Windows Phone writes start 1 + length = disk size, one sector past the end; the Android
        // bootloader then ignores the whole GPT. Use the UEFI value, min(disk size - 1, 0xffffffff).
        ulong want = (ulong)total - 1 > 0xffffffffUL ? 0xffffffffUL : (ulong)total - 1;
        if (G32(np, 446 + 8) == 1 && G32(np, 446 + 12) > want) P32(np, 446 + 12, (uint)want);
        BuildHdr(np, S, h, np, 2 * S, 1, (ulong)total - 1, 2);
        byte[] nb = new byte[Bak]; Array.Copy(np, 2 * S, nb, 0, 32 * S);
        BuildHdr(nb, 32 * S, h, nb, 0, (ulong)total - 1, 1, (ulong)total - 33);
        // prove the result: valid, Android layout, and only entries 27/30/31 differ
        Hdr nh = Check(np, nb, total);
        if (LayoutOf(np) != "android") throw new Exception("internal error: new table invalid");
        for (int i = 1; i <= (int)h.Count; i++) if (i != 27 && i != 30 && i != 31 && !Same(pri, Ent(i), np, Ent(i), 128)) throw new Exception("internal error: other entry changed");
        if (nh.First != h.First || nh.Last != h.Last || !Same(nh.Disk, 0, h.Disk, 0, 16)) throw new Exception("internal error: header changed");
        r.NewPrimary = np; r.NewBackup = nb;
        r.BootFirst = (long)bfirst; r.BootLast = (long)blast; r.SystemFirst = (long)sfirst; r.SystemLast = (long)slast;
        r.DataFirst = (long)afirst; r.DataLast = (long)alast; r.OldDataFirst = (long)dfirst; r.OldDataLast = (long)dlast;
        return r;
    }
    public static bool IsZero(byte[] b, int n) { for (int i = 0; i < n; i++) if (b[i] != 0) return false; return true; }
    public static bool Equal(byte[] a, byte[] b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return false; return true; }
}
'@
function Initialize-RawDisk { if (-not ('SupermanRawDisk' -as [type])) { Add-Type -TypeDefinition $RawSource } }
function Get-CardCandidates([int[]]$excludeDisks) {
    $list = @()
    foreach ($d in @(Get-Disk)) {
        if ($d.IsSystem -or $d.IsBoot) { continue }
        if (@('USB', 'SD', 'MMC') -notcontains "$($d.BusType)") { continue }
        if ($excludeDisks -contains [int]$d.Number) { continue }
        if ("$($d.FriendlyName)" -match 'Qualcomm') { continue }      # the phone itself in Mass Storage Mode
        if ($d.Size -lt $ImageBytes) { continue }
        $list += $d
    }
    return $list
}
function Show-Disk($d) {
    Say ("  Disk {0}: {1}   {2}   ({3})" -f $d.Number, $d.FriendlyName, (Format-Size $d.Size), $d.BusType)
    foreach ($p in @(Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue)) {
        $l = ("$($p.DriveLetter)").Trim([char]0).Trim()
        $label = ''
        if ($l -match '^[A-Za-z]$') {
            try { $v = Get-Volume -DriveLetter $l -ErrorAction Stop; $label = "  $($l): `"$($v.FileSystemLabel)`" $($v.FileSystem), " + (Format-Size ($v.Size - $v.SizeRemaining)) + ' used' } catch { }
        }
        Say ("        partition {0}  {1}{2}" -f $p.PartitionNumber, (Format-Size $p.Size), $label)
    }
}
function Clear-Card([int]$number) {
    try {
        Clear-Disk -Number $number -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
        return
    } catch { Say 'Windows could not clear the card the normal way; trying diskpart...' }
    $script = Join-Path $env:TEMP 'superman-diskpart.txt'
    Set-Content -LiteralPath $script -Value @("select disk $number", 'clean') -Encoding ASCII
    $out = & diskpart.exe /s $script 2>&1 | Out-String
    Remove-Item -LiteralPath $script -Force -ErrorAction SilentlyContinue
    if ($LASTEXITCODE -ne 0) { throw "diskpart could not clear disk ${number}:`n$out" }
}
function Write-Card([int]$number) {
    Initialize-RawDisk
    $drive = "\\.\PhysicalDrive$number"
    $buf = New-Object byte[] (1MB)
    $first = New-Object byte[] (1MB)
    $sha = [Security.Cryptography.SHA256]::Create()
    $gz = [IO.File]::OpenRead($ImageGz)
    $in = New-Object IO.Compression.GZipStream($gz, [IO.Compression.CompressionMode]::Decompress)
    $out = $null
    $total = [long]0
    try {
        $out = [SupermanRawDisk]::Open($drive, $true)
        # The first megabyte (the partition table) is written last. Windows then leaves the card alone meanwhile.
        $n = [SupermanRawDisk]::ReadFull($in, $first)
        if ($n -ne $first.Length) { throw 'The image file is too short.' }
        $sha.TransformBlock($first, 0, $n, $null, 0) | Out-Null
        $total = $n
        $out.Position = $total
        while ($true) {
            $n = [SupermanRawDisk]::ReadFull($in, $buf)
            if ($n -le 0) { break }
            if (($n % 512) -ne 0) { throw 'The image size is not a whole number of sectors.' }
            $sha.TransformBlock($buf, 0, $n, $null, 0) | Out-Null
            $out.Write($buf, 0, $n)
            $total += $n
            if (($total % 32MB) -eq 0) {
                Write-Progress -Activity 'Writing the SD card' -Status ("{0} of {1}" -f (Format-Size $total), (Format-Size $ImageBytes)) -PercentComplete ([int](100 * $total / $ImageBytes))
            }
        }
        $sha.TransformFinalBlock($buf, 0, 0) | Out-Null
        $h = ([BitConverter]::ToString($sha.Hash)).Replace('-', '').ToLower()
        if ($total -ne $ImageBytes -or $h -ne $ImageSha) { throw "The unpacked image is not the expected one ($total bytes, $h). The card is not usable; run this step again." }
        $out.Position = 0
        $out.Write($first, 0, $first.Length)
        $out.Flush($true)
    } finally {
        Write-Progress -Activity 'Writing the SD card' -Completed
        if ($out) { $out.Dispose() }
        $in.Dispose(); $gz.Dispose(); $sha.Dispose()
    }
}
function Test-Card([int]$number) {
    Initialize-RawDisk
    $buf = New-Object byte[] (1MB)
    $sha = [Security.Cryptography.SHA256]::Create()
    $s = [SupermanRawDisk]::Open("\\.\PhysicalDrive$number", $false)
    $total = [long]0
    try {
        while ($total -lt $ImageBytes) {
            $n = [SupermanRawDisk]::ReadFull($s, $buf)
            if ($n -ne $buf.Length) { throw "The card could not be read back at $(Format-Size $total)." }
            $sha.TransformBlock($buf, 0, $n, $null, 0) | Out-Null
            $total += $n
            if (($total % 32MB) -eq 0) {
                Write-Progress -Activity 'Checking the SD card' -Status ("{0} of {1}" -f (Format-Size $total), (Format-Size $ImageBytes)) -PercentComplete ([int](100 * $total / $ImageBytes))
            }
        }
        $sha.TransformFinalBlock($buf, 0, 0) | Out-Null
        return ([BitConverter]::ToString($sha.Hash)).Replace('-', '').ToLower()
    } finally {
        Write-Progress -Activity 'Checking the SD card' -Completed
        $s.Dispose(); $sha.Dispose()
    }
}
function Step-SdCard([int[]]$excludeDisks) {
    Say 'Pop the microSD card into this PC (card reader or adapter). Everything on it gets erased, so check there''s nothing you want on there.'
    Pause-Enter 'Press Enter when the card is in'
    while ($true) {
        $cards = @(Get-CardCandidates $excludeDisks)
        if ($cards.Count -eq 0) {
            Warn 'Can''t see an SD card or USB drive of 4 GB or more.'
            if (-not (Ask-YesNo 'Look again?')) { return $false }
            continue
        }
        Head 'Removable drives on this PC'
        Note 'Your PC''s own disks and the phone never show up in this list.'
        foreach ($c in $cards) { Show-Disk $c }
        $allowed = @($cards | ForEach-Object { "$($_.Number)" }) + @('R', 'Q')
        $a = Ask-Choice 'Which disk number is your SD card?   (R = look again, Q = back to the menu)' $allowed
        if ($a -eq 'Q') { return $false }
        if ($a -eq 'R') { continue }
        $disk = Get-Disk -Number ([int]$a)
        if ($disk.IsReadOnly) { Bad 'That card is write-protected. Check the little lock switch on the adapter.'; continue }
        $box = @(("Disk {0} - {1}, {2}" -f $disk.Number, $disk.FriendlyName, (Format-Size $disk.Size)), 'Everything on it will be ERASED.')
        if ($disk.Size -gt 130GB) { $box += 'That''s big for an SD card. Make sure it isn''t an external hard drive!' }
        Show-Danger $box
        if (-not (Ask-Word 'ERASE' 'pick again')) { continue }

        Write-Host ''
        Say 'Checking this kit''s image file...'
        Test-KitFile 'files\superman-alpha-sdcard.img.gz'
        Good 'Image file is fine.'
        Say 'Clearing the card...'
        Clear-Card $disk.Number
        Say 'Writing 2.6 GB. A few minutes, depending on the card...'
        Write-Card $disk.Number
        Say 'Reading it all back to make sure every byte landed...'
        $h = Test-Card $disk.Number
        if ($h -ne $ImageSha) { throw "The card does not hold what was written (read back $h). Try again, or use another card or reader." }
        Good 'SD card written and double-checked.'
        Say 'If Windows now offers to format a disk, click Cancel. Then take the card out.'
        return $true
    }
}

# ------------------------------------------------------------------------------------------------------------
# Writing Android straight onto the phone (phone in Mass Storage Mode; no SD card, no adb)
# ------------------------------------------------------------------------------------------------------------
function Initialize-Gpt { if (-not ('SupermanGpt' -as [type])) { Add-Type -TypeDefinition $GptSource } }
function Read-DiskSectors([int]$number, [long]$lba, [int]$count) {
    Initialize-RawDisk
    $s = [SupermanRawDisk]::Open("\\.\PhysicalDrive$number", $false)
    try {
        $s.Position = $lba * 512
        $buf = New-Object byte[] ($count * 512)
        $n = [SupermanRawDisk]::ReadFull($s, $buf)
        if ($n -ne $buf.Length) { throw "The phone's storage could not be read at sector $lba." }
        return , $buf
    } finally { $s.Dispose() }
}
function Lock-PhoneVolumes([int]$number, [long]$first, [long]$last) {
    # Windows refuses raw writes into a mounted volume. Lock and dismount every volume in the range and keep the
    # locks until the install is done. Returns the lock handles.
    $locks = @()
    try {
        foreach ($p in @(Get-Partition -DiskNumber $number -ErrorAction SilentlyContinue)) {
            if ([long]$p.Offset -lt ($first * 512) -or [long]$p.Offset -gt ($last * 512)) { continue }
            foreach ($ap in @($p.AccessPaths)) {
                if ("$ap" -match '^\\\\\?\\(Volume\{[0-9a-fA-F-]+\})\\?$') { $locks += [SupermanRawDisk]::LockVolume('\\.\' + $Matches[1]) }
            }
        }
    } catch {
        foreach ($l in $locks) { try { $l.Dispose() } catch { } }
        throw ("Windows is still using the phone's Data drive, so it cannot be replaced yet. Close every Explorer window and program that shows the phone's drives, then try again. Nothing was written.`n(" + $_.Exception.Message + ")")
    }
    return $locks
}
function Write-ImageToPhone($out, $img, [long]$startLba) {
    # Unpacks one image of the kit onto the phone, 1 MiB at a time. Returns the number of bytes written.
    $buf = New-Object byte[] (1MB)
    $sha = [Security.Cryptography.SHA256]::Create()
    $gz = [IO.File]::OpenRead((Join-Path $Here $img.Gz))
    $in = New-Object IO.Compression.GZipStream($gz, [IO.Compression.CompressionMode]::Decompress)
    $total = [long]0; $written = [long]0
    try {
        while ($true) {
            $n = [SupermanRawDisk]::ReadFull($in, $buf)
            if ($n -le 0) { break }
            if (($n % 512) -ne 0) { throw 'The image size is not a whole number of sectors.' }
            $sha.TransformBlock($buf, 0, $n, $null, 0) | Out-Null
            # "Sparse" images (the empty data filesystem) are safe to write without their all-zero parts.
            if (-not $img.Sparse -or -not [SupermanGpt]::IsZero($buf, $n)) {
                $out.Position = $startLba * 512 + $total
                $out.Write($buf, 0, $n)
                $written += $n
            }
            $total += $n
            if (($total % 32MB) -eq 0) {
                Write-Progress -Activity ("Writing " + $img.Name) -Status ("{0} of {1}" -f (Format-Size $total), (Format-Size $img.Bytes)) -PercentComplete ([int](100 * $total / $img.Bytes))
            }
        }
        $sha.TransformFinalBlock($buf, 0, 0) | Out-Null
        $h = ([BitConverter]::ToString($sha.Hash)).Replace('-', '').ToLower()
        if ($total -ne $img.Bytes -or $h -ne $img.Sha) { throw "The unpacked $($img.Name) image is not the expected one ($total bytes, $h)." }
        $out.Flush($true)
    } finally {
        Write-Progress -Activity ("Writing " + $img.Name) -Completed
        $in.Dispose(); $gz.Dispose(); $sha.Dispose()
    }
    return $written
}
function Test-ImageOnPhone($disk, $img, [long]$startLba) {
    # Reads back what Write-ImageToPhone wrote and compares it byte for byte with the image.
    $buf = New-Object byte[] (1MB); $got = New-Object byte[] (1MB)
    $gz = [IO.File]::OpenRead((Join-Path $Here $img.Gz))
    $in = New-Object IO.Compression.GZipStream($gz, [IO.Compression.CompressionMode]::Decompress)
    $total = [long]0
    try {
        while ($true) {
            $n = [SupermanRawDisk]::ReadFull($in, $buf)
            if ($n -le 0) { break }
            if (-not $img.Sparse -or -not [SupermanGpt]::IsZero($buf, $n)) {
                $disk.Position = $startLba * 512 + $total
                $r = $disk.Read($got, 0, $n)
                if ($r -ne $n -or -not [SupermanGpt]::Equal($buf, $got, $n)) { throw "The $($img.Name) on the phone does not match what was written (at $(Format-Size $total))." }
            }
            $total += $n
            if (($total % 32MB) -eq 0) {
                Write-Progress -Activity ("Checking " + $img.Name) -Status ("{0} of {1}" -f (Format-Size $total), (Format-Size $img.Bytes)) -PercentComplete ([int](100 * $total / $img.Bytes))
            }
        }
    } finally {
        Write-Progress -Activity ("Checking " + $img.Name) -Completed
        $in.Dispose(); $gz.Dispose()
    }
}
function Copy-DiskRange([int]$number, [long]$first, [long]$last, [string]$file, [string]$what) {
    # Copies sectors first..last of the phone to a file and returns the sha256 of what was read.
    Initialize-RawDisk
    $buf = New-Object byte[] (1MB)
    $sha = [Security.Cryptography.SHA256]::Create()
    $src = [SupermanRawDisk]::Open("\\.\PhysicalDrive$number", $false)
    $dst = [IO.File]::Create($file)
    $bytes = ($last - $first + 1) * 512
    $done = [long]0
    try {
        $src.Position = $first * 512
        while ($done -lt $bytes) {
            $want = [int][Math]::Min([long]$buf.Length, $bytes - $done)
            $n = $src.Read($buf, 0, $want)
            if ($n -ne $want) { throw "The phone's storage could not be read ($what, at $(Format-Size $done))." }
            $sha.TransformBlock($buf, 0, $n, $null, 0) | Out-Null
            $dst.Write($buf, 0, $n)
            $done += $n
            if (($done % 32MB) -eq 0) {
                Write-Progress -Activity "Copying $what to this PC" -Status ("{0} of {1}" -f (Format-Size $done), (Format-Size $bytes)) -PercentComplete ([int](100 * $done / $bytes))
            }
        }
        $sha.TransformFinalBlock($buf, 0, 0) | Out-Null
        return ([BitConverter]::ToString($sha.Hash)).Replace('-', '').ToLower()
    } finally {
        Write-Progress -Activity "Copying $what to this PC" -Completed
        $dst.Dispose(); $src.Dispose(); $sha.Dispose()
    }
}
function Step-PreparePhone {
    # Mass Storage Mode -> find and confirm the EFIESP -> boot shim + Android bootloader.
    # Returns @{ Efi = 'E:'; Disk = <disk number> } or $null when the user stops.
    Say 'First the phone has to show up on this PC as a set of drives. WPinternals does that:'
    Write-Host ''
    Step 1 'Open WPinternals and plug the phone in.'
    Step 2 'On the left, click "Mode", then pick "Mass Storage Mode".'
    Step 3 'Wait until WPinternals says the phone is in Mass Storage Mode. Windows will pop up a few new drives (MainOS, Data, ...).'
    Write-Host ''
    Warn 'If Windows says "You need to format the disk": click Cancel. Never format.'
    Pause-Enter 'Press Enter once the phone is in Mass Storage Mode'
    $efi = Select-EfiEsp
    if (-not $efi) { Say 'OK, stopping here. Nothing was changed.'; return $null }
    $diskNumber = -1
    try { $diskNumber = [int](Get-Partition -DriveLetter $efi.Substring(0, 1)).DiskNumber } catch { }
    Write-Host ''
    Test-KitFile 'files\boot-installer\Install-SupermanBoot.ps1'
    Test-KitFile 'files\boot-installer\payload\emmc_appsboot.mbn'
    Install-BootFiles $efi
    return @{ Efi = $efi; Disk = $diskNumber }
}
function Step-WritePhone([int]$number) {
    # Returns $true when Android was written and verified.
    Initialize-RawDisk; Initialize-Gpt
    $disk = Get-Disk -Number $number
    $total = [long]($disk.Size / 512)
    $pri = [byte[]](Read-DiskSectors $number 0 34)
    $bak = [byte[]](Read-DiskSectors $number ($total - 33) 33)
    $plan = $null
    try { $plan = [SupermanGpt]::Plan($pri, $bak, $total, $DataMinSectors) }
    catch {
        $m = $_.Exception.Message; if ($_.Exception.InnerException) { $m = $_.Exception.InnerException.Message }
        throw "This phone's partition table is not one this tool can convert: $m.`nNothing was written. (An unlocked Lumia 730 with Windows Phone on it is expected.)"
    }
    $dataBytes = ($plan.DataLast - $plan.DataFirst + 1) * 512
    if ($plan.Layout -eq 'windows') {
        Say ("Here's the plan. Windows Phone's Data partition (" + (Format-Size (($plan.OldDataLast - $plan.OldDataFirst + 1) * 512)) + ") gets split into three:")
        Write-Host ''
        Field 'Android boot' @('64 MB')
        Field 'Android system' @('2.0 GB')
        Field 'Your apps and files' @((Format-Size $dataBytes))
        Write-Host ''
        Note 'Everything else on the phone (firmware, calibration, boot files, MainOS) stays as it is.'
    } else {
        Warn 'This phone already has Android on its internal storage. Going on installs it fresh and ERASES the apps and files of that Android.'
    }
    Write-Host ''

    # ---- backups ----
    $b = New-BackupFolder 'phone'
    [IO.File]::WriteAllBytes((Join-Path $b 'old-primary.bin'), $pri)
    [IO.File]::WriteAllBytes((Join-Path $b 'old-backup.bin'), $bak)
    if ($plan.NewPrimary) {
        [IO.File]::WriteAllBytes((Join-Path $b 'new-primary.bin'), $plan.NewPrimary)
        [IO.File]::WriteAllBytes((Join-Path $b 'new-backup.bin'), $plan.NewBackup)
    }
    $planEnv = "DISK_SECTORS=$total`nBOOT_FIRST=$($plan.BootFirst)`nBOOT_LAST=$($plan.BootLast)`nSYSTEM_FIRST=$($plan.SystemFirst)`nSYSTEM_LAST=$($plan.SystemLast)`nDATA_FIRST=$($plan.DataFirst)`nDATA_LAST=$($plan.DataLast)`nOLD_DATA_FIRST=$($plan.OldDataFirst)`nOLD_DATA_LAST=$($plan.OldDataLast)`n"
    [IO.File]::WriteAllText((Join-Path $b 'plan.env'), $planEnv, (New-Object Text.ASCIIEncoding))
    $efiRange = [SupermanGpt]::Find($pri, 'EFIESP')
    if ($efiRange) {
        $h = Copy-DiskRange $number $efiRange[0] $efiRange[1] (Join-Path $b 'EFIESP.img') 'the boot partition'
        if ((Get-Sha (Join-Path $b 'EFIESP.img')) -ne $h) { throw 'The backup of the boot partition (EFIESP) did not save correctly. Nothing was written.' }
    }
    Good "Backed up the partition table and the boot partition: $b"

    if ($plan.Layout -eq 'windows') {
        Head 'Optional: keep a copy of your Windows Phone'
        Say ("This saves Windows Phone's whole Data partition (your apps, photos, settings) to this PC. It takes " + (Format-Size (($plan.OldDataLast - $plan.OldDataFirst + 1) * 512)) + " of disk space and several minutes. With it, option 3 can later put Windows Phone back exactly as it is now. Without it you can still go back, but to a factory-fresh Windows Phone (stock FFU).")
        Warn 'EXPERIMENTAL: restoring from this copy passed its tests on the PC, but hasn''t been tried on a real phone yet. Don''t count on it as your only way back; keep the stock FFU.'
        if (Ask-YesNo 'Make that copy?' $false) {
            $need = ($plan.OldDataLast - $plan.OldDataFirst + 1) * 512
            $free = Get-FreeSpace $b
            $ok = $false
            if ($free -ge 0 -and $free -lt ($need + 200MB)) {
                Warn ("Not enough room for the copy: it needs " + (Format-Size $need) + ", and only " + (Format-Size $free) + " is free next to this tool.")
            } else {
                $file = Join-Path $b 'Data.img'
                try {
                    $h = Copy-DiskRange $number $plan.OldDataFirst $plan.OldDataLast $file 'the Data partition'
                    Say 'Checking the copy...'
                    if ((Get-Sha $file) -eq $h) { $ok = $true; Good "Copy made and checked: $file" }
                    else { Warn 'The copy didn''t save correctly.' }
                } catch { Warn ("The copy failed: " + $_.Exception.Message) }
                if (-not $ok) { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
            }
            if (-not $ok) {
                if (-not (Ask-YesNo 'Carry on with the install without that copy?' $false)) { Say 'OK, stopping here. Nothing was written to the phone.'; return $false }
            }
        } else { Note 'No copy, then. The stock FFU is your way back.' }
    }
    $sums = ''
    foreach ($f in @(Get-ChildItem -LiteralPath $b -File | Sort-Object Name)) { $sums += (Get-Sha $f.FullName) + '  ./' + $f.Name + "`n" }
    [IO.File]::WriteAllText((Join-Path $b 'SHA256SUMS'), $sums, (New-Object Text.ASCIIEncoding))

    # ---- the point of no return ----
    if ($plan.Layout -eq 'windows') {
        Show-Danger @('LAST STOP. After this there is no undo button.', '',
            'Windows Phone''s apps and data on the phone are ERASED and Android takes', 'their place. Windows Phone will not start afterwards.',
            'The way back is flashing the stock FFU with WPinternals.')
    } else {
        Show-Danger @('LAST STOP. After this there is no undo button.', '',
            'The Android that is on the phone now is ERASED and installed fresh.')
    }
    Say 'It takes 10 to 20 minutes. Keep the phone plugged in and this window open.'
    if (-not (Ask-Word 'INSTALL' 'cancel')) { Say 'Cancelled. Nothing was written to the phone.'; return $false }

    Write-Host ''
    Say 'Checking this kit''s images...'
    foreach ($k in $InternalImages.Keys) { Test-KitFile $InternalImages[$k].Gz }
    Good 'Images are fine.'
    $locks = @(Lock-PhoneVolumes $number $plan.OldDataFirst $plan.OldDataLast)
    $out = $null
    try {
        $out = [SupermanRawDisk]::Open("\\.\PhysicalDrive$number", $true)
        $starts = @{ boot = $plan.BootFirst; system = $plan.SystemFirst; data = $plan.DataFirst }
        $i = 0
        foreach ($k in $InternalImages.Keys) {
            $i++
            $img = $InternalImages[$k]
            Step "$i/4" ("Writing {0} ({1})..." -f $img.Name, (Format-Size $img.Bytes))
            Write-ImageToPhone $out $img $starts[$k] | Out-Null
            Note '        reading it back to check...'
            Test-ImageOnPhone $out $img $starts[$k]
            Good ("{0} written and checked." -f $img.Name)
        }
        if ($plan.NewPrimary) {
            Step '4/4' 'Writing the new partition table...'
            # backup copy first, then the primary: an interruption in between leaves a table the phone still reads
            $out.Position = ($total - 33) * 512; $out.Write($plan.NewBackup, 0, $plan.NewBackup.Length)
            $out.Position = 0; $out.Write($plan.NewPrimary, 0, $plan.NewPrimary.Length)
            $out.Flush($true)
            $chkP = New-Object byte[] ($plan.NewPrimary.Length); $chkB = New-Object byte[] ($plan.NewBackup.Length)
            $out.Position = 0; $r1 = $out.Read($chkP, 0, $chkP.Length)
            $out.Position = ($total - 33) * 512; $r2 = $out.Read($chkB, 0, $chkB.Length)
            if ($r1 -ne $chkP.Length -or $r2 -ne $chkB.Length -or -not [SupermanGpt]::Equal($chkP, $plan.NewPrimary, $chkP.Length) -or -not [SupermanGpt]::Equal($chkB, $plan.NewBackup, $chkB.Length)) {
                throw "The new partition table did not read back correctly. Do NOT restart the phone; run this step again. The old table is saved in $b"
            }
            Good 'Partition table written and checked.'
        } else { Step '4/4' 'The partition table is already the Android one. Leaving it.' }
    } finally {
        if ($out) { $out.Dispose() }
        try { [SupermanRawDisk]::UpdateDisk("\\.\PhysicalDrive$number") } catch { }
        foreach ($l in $locks) { try { $l.Dispose() } catch { } }
    }
    return $true
}

# ------------------------------------------------------------------------------------------------------------
# Menu 1: Android on the phone itself
# ------------------------------------------------------------------------------------------------------------
function Confirm-Unlocked {
    # Shared opening of both installs: a quick look at the PC, then the one thing that must be true.
    Show-Screen 'Before we start' 'A quick look at this PC. These are cautions only - nothing here stops you.'
    try { Do-CheckPc } catch { Warn ('The PC check couldn''t finish (' + $_.Exception.Message + '). That doesn''t stop the install.') }
    Write-Host ''
    if (Ask-YesNo 'Is the phone''s bootloader already unlocked with WPinternals?') { return $true }
    Write-Host ''
    Say 'Then that comes first. In WPinternals, choose "Unlock bootloader". It asks for three files: your phone''s FFU, its emergency files and a donor FFU. All three are on lumiadb.com, and so is a step-by-step guide (lumiadb.com/guides). Keep the FFU afterwards - it''s also your way back to Windows Phone. Come back here when the phone is unlocked.'
    return $false
}
function Do-InstallPhone {
    $steps = @('Get the phone ready', 'Write Android', 'First start')
    Show-Screen 'Install Android on the phone' 'About 30 minutes, most of it waiting.'
    Say 'Android goes onto the phone''s own storage. You don''t need a microSD card.'
    Write-Host ''
    Say 'Windows Phone gets replaced: it won''t start afterwards, and its apps and data are erased. If you change your mind later, flashing the stock FFU with WPinternals brings back a factory-fresh Windows Phone. That route has been tested.'
    Head 'You need'
    Item 'a Nokia Lumia 730 (not the 735), bootloader unlocked with WPinternals'
    Item 'WPinternals on this PC'
    Item 'the stock FFU of your phone, saved somewhere safe (from lumiadb.com)'
    Item 'the phone charged above 50 %'
    Head 'How it goes'
    Step 1 'Get the phone ready     boot shim and Android bootloader'
    Step 2 'Write Android           onto the phone, every byte checked'
    Step 3 'First start             what you''ll see'
    Write-Host ''
    if (-not (Ask-YesNo 'Ready to start?')) { return }
    if (-not (Confirm-Unlocked)) { return }

    Show-Screen 'Get the phone ready' '' $steps 1
    $phone = Step-PreparePhone
    if (-not $phone) { return }
    if ($phone.Disk -lt 0) { throw 'The phone''s disk number could not be worked out from the EFIESP drive letter.' }
    Write-Host ''
    Good 'The phone is ready. Leave it plugged in and in Mass Storage Mode.'
    Pause-Enter 'Press Enter for the next part'

    Show-Screen 'Write Android onto the phone' '' $steps 2
    if (-not (Step-WritePhone $phone.Disk)) { return }
    Write-Host ''
    Good 'Android is on the phone.'
    Pause-Enter 'Press Enter for the last part'

    Show-Screen 'First start' '' $steps 3
    Step 1 'Take out any microSD card that has Android on it (it would start instead).'
    Step 2 'Restart the phone: hold Power for about 10 seconds, until it buzzes. If Windows offers to format a disk at any point, click Cancel.'
    Step 3 'A small text menu shows for 5 seconds with "Boot Shim" selected. Just wait.'
    Step 4 'Android starts. The first start takes about 3 minutes: the phone gathers its own firmware (lines like "SUPERMAN_FIRMWARE 1/4 ... 4/4") and sets itself up.'
    Head 'Good to know'
    Item 'Later starts are much quicker.'
    Item 'Screen says "firmware version not supported yet"? Android still starts, but sound, Wi-Fi and the modem stay off. Please report the hashes it shows.'
    Item 'Phone stops on a "fastboot" screen? Android didn''t start. Run this install again.'
    Item 'Want Windows Phone back one day? That''s option 3 in the menu.'
    Write-Host ''
    Good 'That''s it. Enjoy Android on your Lumia 730!'
}

# ------------------------------------------------------------------------------------------------------------
# Menu 2: Android on a microSD card instead
# ------------------------------------------------------------------------------------------------------------
function Do-InstallSd {
    $steps = @('Get the phone ready', 'Write the SD card', 'First start')
    Show-Screen 'Try Android from a microSD card' 'Optional. About 20 minutes.'
    Say 'Android runs from a microSD card and Windows Phone stays on the phone, untouched. It''s the way to have a look at Android without giving anything up. The price: storage is slower and smaller than with the install on the phone itself.'
    Head 'You need'
    Item 'a Nokia Lumia 730 (not the 735), bootloader unlocked with WPinternals'
    Item 'WPinternals on this PC'
    Item 'the stock FFU of your phone, saved somewhere safe (from lumiadb.com)'
    Item 'a microSD card of 4 GB or more, and a way to plug it into this PC'
    Head 'How it goes'
    Step 1 'Get the phone ready     boot shim and Android bootloader'
    Step 2 'Write the SD card       every byte checked'
    Step 3 'First start             what you''ll see'
    Write-Host ''
    if (-not (Ask-YesNo 'Ready to start?')) { return }
    if (-not (Confirm-Unlocked)) { return }

    # ---- Part 1 ----
    Show-Screen 'Get the phone ready' '' $steps 1
    $phoneDisk = @()
    Note 'Already done this once? If the phone shows the "Boot Shim" menu when it starts, answer N.'
    if (Ask-YesNo 'Get the phone ready now?') {
        Write-Host ''
        $phone = Step-PreparePhone
        if (-not $phone) { return }
        if ($phone.Disk -ge 0) { $phoneDisk = @([int]$phone.Disk) }
        Write-Host ''
        Good 'The phone is ready. Leave it plugged in for now; you restart it in the last part.'
    } else { Say 'Skipping this part.' }
    Pause-Enter 'Press Enter for the next part'

    # ---- Part 2 ----
    Show-Screen 'Write the SD card' '' $steps 2
    $written = $false
    Note 'Already have a card written by this tool? Answer N.'
    if (Ask-YesNo 'Write the SD card now?') {
        Write-Host ''
        $written = Step-SdCard $phoneDisk
        if (-not $written) { Say 'The SD card wasn''t written. You can come back to this from the menu.'; return }
    } else { Say 'Skipping this part.' }
    Pause-Enter 'Press Enter for the last part'

    # ---- Part 3 ----
    Show-Screen 'First start' '' $steps 3
    Step 1 'Put the microSD card into the phone.'
    Step 2 'Restart the phone: hold Power for about 10 seconds, until it buzzes.'
    Step 3 'A small text menu shows for 5 seconds with "Boot Shim" selected. Just wait.'
    Step 4 'Android starts. The first start takes about 3 minutes: the phone copies its own firmware from Windows Phone ("SUPERMAN_FIRMWARE 1/4 ... 4/4") and sets itself up.'
    Head 'Good to know'
    Item 'Later starts are much quicker.'
    Item 'Holding Volume Up while the phone starts always starts Android.'
    Item 'Screen says "firmware version not supported yet"? Android still starts, but sound, Wi-Fi and the modem stay off. Please report the hashes it shows.'
    Item 'Want Windows Phone to start again? That''s option 3 in the menu.'
    Write-Host ''
    Good 'That''s it. Enjoy Android on your Lumia 730!'
}

# ------------------------------------------------------------------------------------------------------------
# Menu 6: move a running SD-card Android onto the phone (over adb)
# ------------------------------------------------------------------------------------------------------------
$script:AdbExe = $null
function Find-Adb {
    $local = Join-Path $Tools 'platform-tools\adb.exe'
    if (Test-Path -LiteralPath $local) { return $local }
    $cmd = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    Write-Host ''
    Say 'This part talks to the phone through "adb", Google''s Android debugging tool, and it isn''t on this PC yet. It can be fetched straight from Google (about 7 MB):'
    Note "  $AdbUrl"
    if (-not (Ask-YesNo 'Download it now?')) { return $null }
    New-Item -ItemType Directory -Path $Tools -Force | Out-Null
    $zip = Join-Path $Tools 'platform-tools.zip'
    Get-WebFile $AdbUrl $zip
    Expand-Archive -LiteralPath $zip -DestinationPath $Tools -Force
    Remove-Item -LiteralPath $zip -Force
    if (-not (Test-Path -LiteralPath $local)) { throw 'The download did not contain adb.exe.' }
    Good 'Got adb.'
    return $local
}
function Adb-Run([string[]]$a) {
    # Runs adb and returns its text output; the exit code is in $script:AdbCode.
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $o = & $script:AdbExe @a 2>$null | Out-String } finally { $ErrorActionPreference = $old }
    $script:AdbCode = $LASTEXITCODE
    return $o.Replace("`r", '').Trim()
}
function Adb-Sh([string]$cmd) { return (Adb-Run @('shell', $cmd)) }
function Adb-ShLive([string]$cmd) {
    # Runs a command on the phone and shows its output as it comes. Returns the exit code.
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & $script:AdbExe shell $cmd 2>&1 | ForEach-Object { Write-Host "    $_" } } finally { $ErrorActionPreference = $old }
    return $LASTEXITCODE
}
function Wait-Phone {
    while ($true) {
        $o = Adb-Run @('devices')
        if ($o -match '(?m)^\S+\s+device\s*$') { return $true }
        if ($o -match 'unauthorized') { Warn 'The phone is asking whether to allow USB debugging. Tap Allow on its screen.' }
        else {
            Warn 'adb can''t see the phone.'
            Item 'Is Android running (from the SD card), and is the USB cable in?'
            Item 'Does Device Manager show "Lumia 730 Android" with a yellow mark? Then it needs a driver: right-click it -> Update driver -> Browse my computer -> Let me pick -> "Android Device" -> "Android ADB Interface". That entry comes with Google''s USB driver; Windows Update often offers it too.'
        }
        if (-not (Ask-YesNo 'Try again?')) { return $false }
    }
}
function Backup-DataPartition([string]$folder) {
    # Optional: a full copy of Windows Phone's Data partition. Returns $true when a verified copy was made.
    $dev = Adb-Sh 'for u in /sys/block/mmcblk0/mmcblk0p*/uevent; do grep -qx PARTNAME=Data $u && grep ^DEVNAME= $u; done; true'
    if ($dev -notmatch '^DEVNAME=(mmcblk0p\d+)$') { Warn 'Couldn''t find the Data partition on the phone, so no copy was made.'; return $false }
    $dev = $Matches[1]
    $sectors = [long]0
    if (-not [long]::TryParse((Adb-Sh "blockdev --getsz /dev/block/$dev"), [ref]$sectors) -or $sectors -le 0) { Warn 'Couldn''t read the size of the Data partition, so no copy was made.'; return $false }
    $bytes = $sectors * 512
    $free = Get-FreeSpace $folder
    if ($free -ge 0 -and $free -lt ($bytes + 200MB)) {
        Warn ("Not enough room for the copy: it needs " + (Format-Size $bytes) + ", and only " + (Format-Size $free) + " is free next to this tool.")
        Say 'No copy was made. Free up some space and start this part again, or carry on without it.'
        return $false
    }
    $file = Join-Path $folder 'Data.img'
    Say ("Copying the Data partition (" + (Format-Size $bytes) + ") to this PC. Several minutes...")
    # adb's output is read as raw bytes and written straight to the file (text handling would damage it).
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $script:AdbExe
    $psi.Arguments = "exec-out cat /dev/block/$dev"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $outFile = [IO.File]::Create($file)
    $buf = New-Object byte[] (1MB)
    $done = [long]0
    $shown = [long]0
    try {
        $src = $p.StandardOutput.BaseStream
        while ($true) {
            $n = $src.Read($buf, 0, $buf.Length)
            if ($n -le 0) { break }
            $outFile.Write($buf, 0, $n)
            $done += $n
            if (($done - $shown) -ge 32MB) {
                $shown = $done
                Write-Progress -Activity 'Copying the Data partition' -Status ("{0} of {1}" -f (Format-Size $done), (Format-Size $bytes)) -PercentComplete ([int][Math]::Min(100, 100 * $done / $bytes))
            }
        }
    } finally {
        $outFile.Dispose()
        Write-Progress -Activity 'Copying the Data partition' -Completed
    }
    $p.WaitForExit()
    $got = (Get-Item -LiteralPath $file).Length
    if ($p.ExitCode -ne 0 -or $got -ne $bytes) {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        Warn ("The copy didn't finish ($got of $bytes bytes), so it was thrown away. No copy was made.")
        return $false
    }
    Say 'Comparing the copy with the phone. A few more minutes...'
    $phoneHash = (Adb-Sh "sha256sum /dev/block/$dev").Split(' ')[0]
    if ((Get-Sha $file) -ne $phoneHash) {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        Warn 'The copy doesn''t match the phone, so it was thrown away. No copy was made.'
        return $false
    }
    Good "Copy made and checked: $file"
    return $true
}
function Do-InstallInternal {
    Show-Screen 'Advanced: move Android from the SD card onto the phone' 'Over adb. Tested on one phone only.'
    Say 'Most people don''t need this: option 1 puts Android on the phone directly.'
    Write-Host ''
    Say 'This takes the Android you''re running from the SD card and copies it onto the phone itself, into the space of Windows Phone''s Data partition. Afterwards the phone runs Android without the card, with more room for apps (about 2.7 GB) and faster storage.'
    Show-Danger @('Windows Phone will no longer start afterwards, and its apps and data on the',
        'phone are erased. Firmware, calibration and the boot files are not touched.',
        'The way back is the stock FFU with WPinternals - make sure you have it.')
    Head 'Before you start'
    Item 'Android is running from the SD card written by this tool (started at least once)'
    Item 'the phone is plugged in, on the charger, battery at 40 % or more'
    Write-Host ''
    if (-not (Ask-YesNo 'Continue?' $false)) { return }

    foreach ($k in @($KitHashes.Keys | Where-Object { $_ -like 'files\internal\*' })) { Test-KitFile $k }
    $script:AdbExe = Find-Adb
    if (-not $script:AdbExe) { Say 'This part can''t run without adb. Nothing was changed.'; return }
    Adb-Run @('start-server') | Out-Null
    if (-not (Wait-Phone)) { return }
    Adb-Run @('root') | Out-Null
    Start-Sleep -Seconds 3
    Adb-Run @('wait-for-device') | Out-Null
    if ((Adb-Sh 'getprop ro.product.device') -ne 'superman' -and (Adb-Sh 'getprop ro.hardware') -ne 'superman') { throw 'The connected phone is not a Lumia 730 running this Android.' }
    if ((Adb-Sh 'id -u') -ne '0') { throw 'adb could not get root rights on the phone.' }
    Good 'Found your Lumia 730, running Android.'

    Say 'Copying the installer to the phone...'
    Adb-Sh "rm -rf $Remote; mkdir -p $Remote" | Out-Null
    foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $Files 'internal\payload') -File)) {
        Adb-Run @('push', $f.FullName, "$Remote/$($f.Name)") | Out-Null
        if ($script:AdbCode -ne 0) { throw "Could not copy $($f.Name) to the phone." }
    }
    Adb-Sh "chmod 755 $Remote/superman-gpt" | Out-Null

    Say 'Checking the phone. Nothing is written yet...'
    if ((Adb-ShLive "sh $Remote/superman-install-internal.sh check") -ne 0) { throw 'The phone did not pass the checks. See the message above.' }
    if ((Adb-ShLive "sh $Remote/superman-install-internal.sh prepare") -ne 0) { throw 'The new partition layout could not be planned. Nothing was written.' }

    $b = New-BackupFolder 'internal'
    foreach ($f in 'old-primary.bin', 'old-backup.bin', 'new-primary.bin', 'new-backup.bin', 'plan.env', 'PLAN.sha256') {
        Adb-Run @('pull', "$Remote/plan/$f", (Join-Path $b $f)) | Out-Null
        if ($script:AdbCode -ne 0) { throw "Could not copy $f from the phone." }
    }
    foreach ($line in @(Get-Content -LiteralPath (Join-Path $b 'PLAN.sha256'))) {
        if ($line -match '^([0-9a-f]{64})\s+\*?(\S+)$') {
            if ((Get-Sha (Join-Path $b $Matches[2])) -ne $Matches[1]) { throw "The backup copy of $($Matches[2]) is damaged." }
        }
    }
    Good 'Backed up the partition table.'
    $efiDev = Adb-Sh 'for u in /sys/block/mmcblk0/mmcblk0p*/uevent; do grep -qx PARTNAME=EFIESP $u && grep ^DEVNAME= $u; done; true'
    if ($efiDev -notmatch '^DEVNAME=(mmcblk0p\d+)$') { throw 'The EFIESP partition was not found on the phone.' }
    $efiDev = $Matches[1]
    Adb-Sh "dd if=/dev/block/$efiDev of=$Remote/EFIESP.img bs=1M 2>/dev/null" | Out-Null
    $phoneHash = (Adb-Sh "sha256sum /dev/block/$efiDev").Split(' ')[0]
    Adb-Run @('pull', "$Remote/EFIESP.img", (Join-Path $b 'EFIESP.img')) | Out-Null
    Adb-Sh "rm -f $Remote/EFIESP.img" | Out-Null
    if ((Get-Sha (Join-Path $b 'EFIESP.img')) -ne $phoneHash) { throw 'The backup of the boot partition (EFIESP) does not match the phone.' }
    Good "Backed up the boot partition too. Backups are in $b"

    Head 'Optional: keep a copy of your Windows Phone'
    Say 'This saves Windows Phone''s whole Data partition (your apps, photos, settings) to this PC. It takes about 5 GB and several minutes. With it, option 3 can later put Windows Phone back as it is now. Without it you can still go back, but to a factory-fresh Windows Phone (stock FFU).'
    Warn 'EXPERIMENTAL: restoring from this copy passed its tests on the PC, but hasn''t been tried on a real phone yet. Don''t count on it as your only way back; keep the stock FFU.'
    if (Ask-YesNo 'Make that copy?' $false) {
        if (-not (Backup-DataPartition $b)) {
            if (-not (Ask-YesNo 'Carry on with the install without that copy?' $false)) { Say 'OK, stopping here. Nothing was written to the phone.'; return }
        }
    } else { Note 'No copy, then. The stock FFU is your way back.' }
    # Checksums of the backup folder, in the format the Linux script (install-internal.sh restore-windows) reads.
    $sums = ''
    foreach ($f in @(Get-ChildItem -LiteralPath $b -File | Sort-Object Name)) { $sums += (Get-Sha $f.FullName) + '  ./' + $f.Name + "`n" }
    [IO.File]::WriteAllText((Join-Path $b 'SHA256SUMS'), $sums, (New-Object Text.ASCIIEncoding))

    Show-Danger @('LAST STOP. After this there is no undo button.', '',
        'Windows Phone''s Data partition is ERASED and Android takes its place.')
    Say 'It takes about 10 minutes. Keep the phone plugged in while it runs.'
    if (-not (Ask-Word 'INSTALL' 'cancel')) { Say 'Cancelled. Nothing was written.'; return }
    Write-Host ''
    $oldHash = Get-Sha (Join-Path $b 'old-primary.bin')
    $rc = Adb-ShLive "sh $Remote/superman-install-internal.sh install $oldHash"
    Adb-Run @('pull', "$Remote/install.log", (Join-Path $b 'install.log')) | Out-Null
    if ($rc -ne 0) { throw "The install stopped with an error. The phone still starts from the SD card. The log is in $b" }
    Write-Host ''
    Good 'Android now lives on the phone itself.'
    Say 'Switch the phone off, take the SD card OUT, and switch it back on. The first start takes about 3 minutes. Hang on to that card: with it in, the phone starts from the card again, which makes it a handy rescue system.'
}

# ------------------------------------------------------------------------------------------------------------
# Menu 3: back to Windows Phone
# ------------------------------------------------------------------------------------------------------------
function Get-RestorableBackups {
    # Backup folders of this tool that hold everything needed to put Windows Phone back as it was.
    $list = @()
    foreach ($d in @(Get-ChildItem -LiteralPath $Backups -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {
        $ok = $true
        foreach ($f in 'Data.img', 'old-primary.bin', 'old-backup.bin', 'new-primary.bin', 'new-backup.bin', 'SHA256SUMS') {
            if (-not (Test-Path -LiteralPath (Join-Path $d.FullName $f))) { $ok = $false }
        }
        if ($ok) { $list += $d }
    }
    return $list
}
function Test-BackupFolder([string]$folder) {
    foreach ($line in @(Get-Content -LiteralPath (Join-Path $folder 'SHA256SUMS'))) {
        if ($line -match '^([0-9a-f]{64})\s+\*?(?:\./)?(.+)$') {
            $p = Join-Path $folder $Matches[2]; $want = $Matches[1]
            if (-not (Test-Path -LiteralPath $p)) { throw "The backup is incomplete: $($Matches[2]) is missing." }
            if ((Get-Sha $p) -ne $want) { throw "The backup is damaged: $($Matches[2]) does not match its checksum." }
        }
    }
}
function Set-WindowsDefault([string]$efi) {
    # Makes Windows Phone the boot menu's default entry again. Returns $true when done.
    $store = Join-Path ($efi + '\') 'EFI\Microsoft\Boot\BCD'
    $wl = '{7619dcc9-fafe-11d9-b411-000476eba25f}'      # the Windows Loader entry of every Windows Phone
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        $enum = & bcdedit.exe /store $store /enum all /v 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0 -or $enum -notmatch [regex]::Escape($wl)) { return $false }
        & bcdedit.exe /store $store /default $wl 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { return $false }
    } finally { $ErrorActionPreference = $old }
    try { Write-VolumeCache -DriveLetter $efi.Substring(0, 1) | Out-Null } catch { }
    return $true
}
function Do-RestoreFromBackup {
    Show-Screen 'Put Windows Phone back as it was   [EXPERIMENTAL]' 'From the copy this tool made before it installed Android.'
    Say 'This writes your saved Windows Phone data and the old partition table back onto the phone. Windows Phone then starts the way you left it, apps and files included. The Android on the phone is erased. The bootloader stays unlocked.'
    Write-Host ''
    Warn 'EXPERIMENTAL: this route passed its tests on the PC but hasn''t been tried on a real phone yet. If it lets you down, the fresh install (stock FFU) still works afterwards.'
    $found = @(Get-RestorableBackups)
    if ($found.Count -eq 0) {
        Write-Host ''
        Warn 'There''s no backup with a copy of your Windows Phone data in this tool''s backups folder. That copy is the optional step during the install. Without it, the way back is the fresh install with the stock FFU.'
        return
    }
    Head 'Backups you can restore'
    for ($i = 0; $i -lt $found.Count; $i++) {
        $size = (Get-Item -LiteralPath (Join-Path $found[$i].FullName 'Data.img')).Length
        Step ($i + 1) ("{0}   ({1} of Windows Phone data)" -f $found[$i].Name, (Format-Size $size))
    }
    $n = Ask-Choice 'Which one? (number, or Q to go back)' (@(1..$found.Count | ForEach-Object { "$_" }) + @('Q'))
    if ($n -eq 'Q') { return }
    $b = $found[[int]$n - 1].FullName
    Write-Host ''
    Say 'Checking the backup. It reads the whole copy, so give it a minute or two...'
    Test-BackupFolder $b
    Good 'The backup is complete and undamaged.'

    Head 'Now put the phone into Mass Storage Mode'
    Step 1 'Open WPinternals and plug the phone in.'
    Step 2 'On the left, click "Mode", then pick "Mass Storage Mode".'
    Step 3 'If WPinternals keeps waiting for the phone, restart the phone: hold Power for about 10 seconds. WPinternals catches it as it starts and switches it over.'
    Write-Host ''
    Warn 'If Windows says "You need to format the disk": click Cancel. Never format.'
    Pause-Enter 'Press Enter once the phone is in Mass Storage Mode'
    $efi = Select-EfiEsp
    if (-not $efi) { Say 'OK, stopping here. Nothing was changed.'; return }
    $number = [int](Get-Partition -DriveLetter $efi.Substring(0, 1)).DiskNumber

    Initialize-RawDisk; Initialize-Gpt
    $total = [long]((Get-Disk -Number $number).Size / 512)
    $pri = [byte[]](Read-DiskSectors $number 0 34)
    $bak = [byte[]](Read-DiskSectors $number ($total - 33) 33)
    $oldPri = [IO.File]::ReadAllBytes((Join-Path $b 'old-primary.bin')); $oldBak = [IO.File]::ReadAllBytes((Join-Path $b 'old-backup.bin'))
    $newPri = [IO.File]::ReadAllBytes((Join-Path $b 'new-primary.bin'))
    $isAndroid = ($pri.Length -eq $newPri.Length) -and [SupermanGpt]::Equal($pri, $newPri, $pri.Length)
    $isOld = ($pri.Length -eq $oldPri.Length) -and [SupermanGpt]::Equal($pri, $oldPri, $pri.Length)
    if (-not $isAndroid -and -not $isOld) {
        throw "This backup does not belong to the phone that is connected, or the phone's partition table was changed by something else. Nothing was written. Use the fresh install (stock FFU) instead."
    }
    $data = [SupermanGpt]::Find($oldPri, 'Data')
    $file = Join-Path $b 'Data.img'
    $bytes = (Get-Item -LiteralPath $file).Length
    if (-not $data -or $bytes -ne (($data[1] - $data[0] + 1) * 512)) { throw 'The Data copy in this backup does not have the size of the phone''s Data partition. Nothing was written.' }

    Show-Danger @('LAST STOP. After this there is no undo button.', '',
        'The Android on the phone is ERASED and Windows Phone is put back.')
    Say 'It takes 10 to 20 minutes. Keep the phone plugged in and this window open.'
    if (-not (Ask-Word 'RESTORE' 'cancel')) { Say 'Cancelled. Nothing was written to the phone.'; return }
    Write-Host ''

    $locks = @(Lock-PhoneVolumes $number $data[0] $data[1])
    $out = $null
    $buf = New-Object byte[] (1MB); $got = New-Object byte[] (1MB)
    try {
        $out = [SupermanRawDisk]::Open("\\.\PhysicalDrive$number", $true)
        Step '1/3' ("Writing your Windows Phone data back (" + (Format-Size $bytes) + ")...")
        foreach ($pass in 'write', 'check') {
            $src = [IO.File]::OpenRead($file)
            $done = [long]0
            try {
                while ($done -lt $bytes) {
                    $n = [SupermanRawDisk]::ReadFull($src, $buf)
                    if ($n -le 0 -or ($n % 512) -ne 0) { throw 'The Data copy could not be read from this PC.' }
                    $out.Position = $data[0] * 512 + $done
                    if ($pass -eq 'write') { $out.Write($buf, 0, $n) }
                    else {
                        $r = $out.Read($got, 0, $n)
                        if ($r -ne $n -or -not [SupermanGpt]::Equal($buf, $got, $n)) { throw "The Data partition on the phone does not match the copy (at $(Format-Size $done)). Run this again." }
                    }
                    $done += $n
                    if (($done % 32MB) -eq 0) {
                        Write-Progress -Activity "Data partition: $pass" -Status ("{0} of {1}" -f (Format-Size $done), (Format-Size $bytes)) -PercentComplete ([int](100 * $done / $bytes))
                    }
                }
            } finally { $src.Dispose(); Write-Progress -Activity "Data partition: $pass" -Completed }
            if ($pass -eq 'write') { $out.Flush($true); Note '        reading it back to check...' }
        }
        Good 'Windows Phone data written and checked.'
        Step '2/3' 'Writing the Windows Phone partition table...'
        $out.Position = ($total - 33) * 512; $out.Write($oldBak, 0, $oldBak.Length)
        $out.Position = 0; $out.Write($oldPri, 0, $oldPri.Length)
        $out.Flush($true)
        $chkP = New-Object byte[] ($oldPri.Length); $chkB = New-Object byte[] ($oldBak.Length)
        $out.Position = 0; $r1 = $out.Read($chkP, 0, $chkP.Length)
        $out.Position = ($total - 33) * 512; $r2 = $out.Read($chkB, 0, $chkB.Length)
        if ($r1 -ne $chkP.Length -or $r2 -ne $chkB.Length -or -not [SupermanGpt]::Equal($chkP, $oldPri, $chkP.Length) -or -not [SupermanGpt]::Equal($chkB, $oldBak, $chkB.Length)) {
            throw 'The partition table did not read back correctly. Do NOT restart the phone; run this again.'
        }
        Good 'Partition table written and checked.'
    } finally {
        if ($out) { $out.Dispose() }
        try { [SupermanRawDisk]::UpdateDisk("\\.\PhysicalDrive$number") } catch { }
        foreach ($l in $locks) { try { $l.Dispose() } catch { } }
    }
    Step '3/3' 'Making Windows Phone the default in the boot menu...'
    $bcdOk = $false
    try { $bcdOk = Set-WindowsDefault $efi } catch { }
    if ($bcdOk) { Good 'Windows Phone starts by default again.' }
    else {
        Warn 'Couldn''t switch the boot menu. Windows Phone is restored, but the phone still starts the Android bootloader first, which now finds nothing and stops on a "fastboot" screen. If you see that, use the fresh install (stock FFU): it always works.'
    }
    Write-Host ''
    Good 'Windows Phone is back on the phone.'
    Say 'Restart it: hold Power for about 10 seconds. If Windows offers to format a disk, Cancel.'
    Write-Host ''
    Note 'The boot shim and the Android bootloader are still on the phone. They''re harmless; holding Volume Up at start would run them. A fresh install with the stock FFU removes them.'
}
function Do-FreshWindows {
    Show-Screen 'Fresh Windows Phone' 'With the stock FFU and WPinternals. Tested with Android on the phone itself.'
    Say 'This wipes Android completely and leaves you with a factory-fresh Windows Phone. It works whatever state the phone is in.'
    Head 'First, the FFU'
    Say 'You need the stock FFU of your phone. If you no longer have it, get it from lumiadb.com: Lumia 730 Dual SIM (RM-1040), then the FFU for your product code (on the label under the battery, "CODE: 059W..."). Not listed? Any RM-1040 FFU will do.'
    Note 'The "Download" tab in WPinternals no longer works; Microsoft closed those servers.'
    Head 'Then'
    Step 1 'Take the SD card out of the phone, if there is one.'
    Step 2 'Open WPinternals. On the left choose "Flash" and scroll to "Flash original FFU". Pick your FFU file there and let WPinternals check it.'
    Step 3 'Plug the phone into the PC.'
    Step 4 'Restart the phone: hold Power for about 10 seconds, until it buzzes. WPinternals catches it as it starts, and the flash button lights up.'
    Step 5 'Click "Flash FFU image". You''ll see "Restoring bootloader..." first, then the flash.'
    Step 6 'Wait for it to finish. The phone restarts into the Windows Phone setup.'
    Write-Host ''
    Note 'This also removes the boot shim and the bootloader unlock. Your phone''s calibration and addresses are kept. If a red "!" screen shows a key combination, don''t use it.'
    Write-Host ''
    if (Ask-YesNo 'Open lumiadb.com in your browser now?' $false) { Open-Web 'https://lumiadb.com' }
}
function Do-SdOnlyWindows {
    Show-Screen 'Start Windows Phone again' 'For when Android is only on a microSD card.'
    Say 'Windows Phone never left the phone. Only the boot menu''s default points to Android.'
    Head 'Switch the default back'
    Step 1 'In Android, open a root shell: adb shell, then su.'
    Step 2 'Run:  superman-bootmenu windows'
    Step 3 'Restart. Windows Phone starts by default from now on.'
    Head 'And back to Android later'
    Step 1 'Restart while holding Volume Up.'
    Step 2 'Run:  superman-bootmenu android'
    Write-Host ''
    Note 'Why the detour? The phone''s keys can''t move through the boot menu, so whichever entry is the default is the one that starts.'
}
function Do-BackToWindows {
    Show-Screen 'Go back to Windows Phone' 'Three ways, depending on where you are.'
    Choice 'A' 'Fresh Windows Phone' 'Stock FFU + WPinternals. Always works. Erases everything on the phone.' 'tested'
    Write-Host ''
    Choice 'B' 'Windows Phone as you left it' 'From this tool''s backup. Needs the optional copy made during the install.' 'experimental' 'Yellow'
    Write-Host ''
    Choice 'C' 'Android is only on a microSD card' 'Windows Phone is still there; this shows how to start it again.'
    Write-Host ''
    Choice 'Q' 'Back to the menu'
    Write-Host ''
    $c = Ask-Choice 'Which one?' @('A', 'B', 'C', 'Q')
    switch ($c) {
        'A' { Do-FreshWindows }
        'B' { Do-RestoreFromBackup }
        'C' { Do-SdOnlyWindows }
        'Q' { return $false }
    }
}

# ------------------------------------------------------------------------------------------------------------
# Menu 4: check the kit
# ------------------------------------------------------------------------------------------------------------
function Do-CheckKit {
    Show-Screen 'Check this kit''s files' 'Makes sure nothing is missing or got damaged on the way to you.'
    $bad = 0
    foreach ($k in $KitHashes.Keys) {
        try { Test-KitFile $k; Good $k } catch { Bad $k; More $_.Exception.Message.Split("`n")[0]; $bad++ }
    }
    Write-Host ''
    if ($bad -eq 0) { Good 'All there, all intact.' } else { Bad "$bad file(s) are missing or damaged. Please download the kit again." }
}

# ------------------------------------------------------------------------------------------------------------
# Main menu
# ------------------------------------------------------------------------------------------------------------
New-Item -ItemType Directory -Path $Logs -Force | Out-Null
$log = Join-Path $Logs ('setup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
try { Start-Transcript -Path $log | Out-Null } catch { }
try { $Host.UI.RawUI.WindowTitle = 'Lumia 730 Android setup' } catch { }
try {
    # The screens are 86 columns wide; make sure the window is not narrower than that.
    $raw = $Host.UI.RawUI
    if ($raw.WindowSize.Width -lt 90) {
        $buf = $raw.BufferSize; if ($buf.Width -lt 100) { $buf.Width = 100; $raw.BufferSize = $buf }
        $win = $raw.WindowSize; $win.Width = 100; $raw.WindowSize = $win
    }
} catch { }

while ($true) {
    Show-Screen 'What would you like to do?'
    Choice '1' 'Install Android on the phone' 'Replaces Windows Phone. No microSD card needed.' 'start here'
    Choice '2' 'Try Android from a microSD card' 'Optional. Windows Phone stays on the phone.'
    Choice '3' 'Go back to Windows Phone' 'Fresh from the stock FFU, or as you left it from a backup (experimental).'
    Write-Host ''
    Note '  Help and checks'
    Choice '4' 'What do I need, and where do I get it?' 'WPinternals, drivers, the FFU.'
    Choice '5' 'Is this PC ready?' 'Looks for drivers, WPinternals and your FFU. Cautions only.'
    Choice '6' 'Check this kit''s files'
    Write-Host ''
    Note '  For tinkerers'
    Choice '7' 'Move Android from the SD card onto the phone' 'Over adb. Option 1 is the easier way.'
    Write-Host ''
    Choice 'Q' 'Quit'
    Write-Host ''
    $c = Ask-Choice 'Your choice' @('1', '2', '3', '4', '5', '6', '7', 'Q')
    if ($c -eq 'Q') { break }
    $stay = $true
    try {
        switch ($c) {
            '1' { Do-InstallPhone }
            '2' { Do-InstallSd }
            '3' { if ((Do-BackToWindows) -eq $false) { $stay = $false } }
            '4' { Do-WhatYouNeed }
            '5' { Show-Screen 'Is this PC ready?' 'Cautions only - nothing here stops you.'; Do-CheckPc }
            '6' { Do-CheckKit }
            '7' { Do-InstallInternal }
        }
    } catch {
        Write-Host ''
        Bad 'That didn''t work. Here''s what happened:'
        foreach ($l in ("$($_.Exception.Message)" -split "`n")) { More $l }
        Write-Host ''
        Note "The full log of this run is in: $log"
    }
    if ($stay) { Pause-Enter 'Press Enter to go back to the menu' }
}
Clear-Screen
Write-Host ''
Write-Host '  Bye! The log of this run is in:' -ForegroundColor Cyan
Write-Host "  $log" -ForegroundColor DarkGray
Write-Host ''
try { Stop-Transcript | Out-Null } catch { }
