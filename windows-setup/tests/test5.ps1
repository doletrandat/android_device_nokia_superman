param([string]$T)
. "$PSScriptRoot/harness.ps1" -Kit "$T/kit"
$ErrorActionPreference = 'Stop'
"=== with the Data copy"
Queue-Answers 'y' 'y' 'INSTALL'
try { Do-InstallInternal } catch { "THREW: $($_.Exception.Message)" }
"answers left: $($script:Answers.Count)"
$d = Get-ChildItem "$T/kit/backups" -Directory -Filter 'internal-*' | Sort-Object Name | Select-Object -Last 1
"Data.img: $((Get-Item (Join-Path $d.FullName 'Data.img')).Length) bytes, sha $((Get-FileHash (Join-Path $d.FullName 'Data.img')).Hash.ToLower().Substring(0,16))"
"=== copy cut short, then stop"
New-Item "$T/phone/short-data" -ItemType File -Force | Out-Null; Start-Sleep 1
Queue-Answers 'y' 'y' 'n'
try { Do-InstallInternal } catch { "THREW: $($_.Exception.Message)" }
Remove-Item "$T/phone/short-data"
"answers left: $($script:Answers.Count)"
"=== declined (default)"; Start-Sleep 1
Queue-Answers 'y' '' 'no'
try { Do-InstallInternal } catch { "THREW: $($_.Exception.Message)" }
"answers left: $($script:Answers.Count)"
