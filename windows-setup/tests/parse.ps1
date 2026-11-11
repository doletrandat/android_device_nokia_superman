param([string[]]$Paths)
foreach ($p in $Paths) {
    $tokens = $null; $errs = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($p, [ref]$tokens, [ref]$errs)
    "{0}: {1} parse errors" -f (Split-Path -Leaf $p), $errs.Count
    foreach ($e in $errs) { "  line {0}: {1}" -f $e.Extent.StartLineNumber, $e.Message }
    # PS7-only syntax that 5.1 would reject
    $bad = $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.TernaryExpressionAst] -or $n -is [System.Management.Automation.Language.PipelineChainAst] }, $true)
    "  PS7-only constructs: {0}" -f @($bad).Count
    $bad7 = @($tokens | Where-Object { $_.Kind -in 'QuestionQuestion','QuestionQuestionEquals','QuestionDot','QuestionLBracket' })
    "  PS7-only operators: {0}" -f $bad7.Count
    # commands used, to spot typos in our own function names
    $defined = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) | ForEach-Object Name)
    $used = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true) | ForEach-Object { $_.GetCommandName() } | Where-Object { $_ } | Sort-Object -Unique)
    "  functions defined: {0}" -f $defined.Count
    "  commands not defined here: " + (($used | Where-Object { $defined -notcontains $_ }) -join ', ')
    # variables read but never assigned (rough)
    $assigned = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true) | ForEach-Object { $_.Left.Extent.Text -replace '^\[.*?\]','' -replace '^\$(script:)?','' -replace '[\.\[].*$','' })
    $params = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.ParameterAst] }, $true) | ForEach-Object { $_.Name.VariablePath.UserPath })
    $loops = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.ForEachStatementAst] }, $true) | ForEach-Object { $_.Variable.VariablePath.UserPath })
    $auto = 'true','false','null','_','Matches','LASTEXITCODE','ErrorActionPreference','ProgressPreference','MyInvocation','Host','env:TEMP','PSScriptRoot','args','input','PSCommandPath'
    $vars = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] }, $true) | ForEach-Object { $_.VariablePath.UserPath -replace '^script:','' } | Sort-Object -Unique)
    "  variables never assigned: " + (($vars | Where-Object { $assigned -notcontains $_ -and $params -notcontains $_ -and $loops -notcontains $_ -and $auto -notcontains $_ }) -join ', ')
}
