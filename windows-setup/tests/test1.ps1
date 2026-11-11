param([string]$T)
. "$PSScriptRoot/harness.ps1" -Kit "$T/kit"
"--- constants"; "$Files | $ImageGz | $($KitHashes.Count) hashes"
"--- helpers"
Format-Size 2751463424; Format-Size 66060288
Queue-Answers '' 'n' 'maybe' 'YES'
"yn: $(Ask-YesNo 'q1') $(Ask-YesNo 'q2') $(Ask-YesNo 'q3')"
Queue-Answers 'x' 'r'
"choice: $(Ask-Choice 'pick' @('R','Q'))"
"--- kit check"
Do-CheckKit
"--- EFIESP info"
$i = Get-EfiEspInfo "$T/efi/"; $i | Format-List | Out-String
"null for a non-EFIESP dir: $($null -eq (Get-EfiEspInfo "$T/kit/"))"
