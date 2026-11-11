# Loads the tool's functions and constants without running its admin check or menu.
param([string]$Kit)
$tokens = $null; $errs = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Kit 'Superman-Setup.ps1'), [ref]$tokens, [ref]$errs)
$Here = $Kit
$code = New-Object System.Text.StringBuilder
foreach ($hxSt in $ast.EndBlock.Statements) {
    $hxText = $hxSt.Extent.Text
    if ($hxSt -is [System.Management.Automation.Language.FunctionDefinitionAst]) { [void]$code.AppendLine($hxText); continue }
    if ($hxSt -is [System.Management.Automation.Language.AssignmentStatementAst]) {
        $hxLeft = $hxSt.Left.Extent.Text
        if ($hxLeft -in '$Here', '$principal', '$log') { continue }
        [void]$code.AppendLine($hxText)
    }
}
. ([scriptblock]::Create($code.ToString()))
$script:Answers = New-Object System.Collections.Queue
function Read-Host { param($p) if ($script:Answers.Count -eq 0) { throw "TEST: no answer queued for: $p" }; $a = $script:Answers.Dequeue(); Write-Host "$p > $a" -ForegroundColor Magenta; return $a }
function Queue-Answers { foreach ($a in $args) { $script:Answers.Enqueue($a) } }
