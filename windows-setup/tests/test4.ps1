param([string]$T)
. "$PSScriptRoot/harness.ps1" -Kit "$T/kit"
"=== bare PC (nothing found)"
$env:USERPROFILE = "$T/nouser"; Do-CheckPc
"=== PC with drivers, WPinternals and an FFU"
$env:USERPROFILE = "$T/user"; $env:PATH = "$T/fakebin:" + $env:PATH; Do-CheckPc
