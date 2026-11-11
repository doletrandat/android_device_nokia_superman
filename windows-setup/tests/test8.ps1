# Walks the main menu itself: the menu, the "what you need" screen, the back-to-Windows submenu and its
# three screens, then Quit. Nothing here touches a disk.
param([string]$T)
. "$PSScriptRoot/harness.ps1" -Kit "$T/kit"
$ErrorActionPreference = 'Stop'
$tokens = $null; $errs = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $T 'kit/Superman-Setup.ps1'), [ref]$tokens, [ref]$errs)
$loop = @($ast.EndBlock.Statements | Where-Object { $_ -is [System.Management.Automation.Language.WhileStatementAst] })[-1]
$log = "$T/kit/logs/test.log"
function Start-Process { param($FilePath) Write-Host "    (mock) open $FilePath" -ForegroundColor DarkYellow }
# 4 (what you need), open site y, Enter | 3 then Q (straight back, no pause) | 3 A, open site n, Enter |
# 3 B, Enter | 3 C, Enter | a wrong key, then Q
Queue-Answers '4' 'y' '' '3' 'Q' '3' 'A' 'n' '' '3' 'B' '' '3' 'C' '' 'zz' 'Q'
. ([scriptblock]::Create($loop.Extent.Text))
"--- answers left: $($script:Answers.Count)"
