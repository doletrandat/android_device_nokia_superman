param([string]$T)
. "$PSScriptRoot/harness.ps1" -Kit "$T/kit"
$ErrorActionPreference = 'Stop'
function Start-Sleep { }
Queue-Answers 'y' '' 'INSTALL'
try { Do-InstallInternal } catch { "THREW: $($_.Exception.Message)" }
"--- answers left: $($script:Answers.Count)"
"--- cancel path"
Queue-Answers 'y' '' 'no'
try { Do-InstallInternal } catch { "THREW: $($_.Exception.Message)" }
"--- failing install path"
New-Item "$T/phone/fail-install" -ItemType File -Force | Out-Null
Queue-Answers 'y' '' 'INSTALL'
try { Do-InstallInternal } catch { "THREW: $($_.Exception.Message)" }
Remove-Item "$T/phone/fail-install"
