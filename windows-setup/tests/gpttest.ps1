param([string]$Cs, [string]$B)
Add-Type -TypeDefinition (Get-Content -Raw $Cs)
$rb = { param($n) [IO.File]::ReadAllBytes((Join-Path $B $n)) }
$h = { param($b) ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($b))).Replace('-','').ToLower().Substring(0,16) }
$p = [SupermanGpt]::Plan((& $rb 'old-primary.bin'), (& $rb 'old-backup.bin'), 15269888, 5242880)
"layout $($p.Layout)  boot $($p.BootFirst)-$($p.BootLast)  system $($p.SystemFirst)-$($p.SystemLast)  data $($p.DataFirst)-$($p.DataLast)"
"new primary $(& $h $p.NewPrimary)  equal to superman-gpt's: $([Linq.Enumerable]::SequenceEqual($p.NewPrimary, (& $rb 'new-primary.bin')))"
"new backup  $(& $h $p.NewBackup)  equal to superman-gpt's: $([Linq.Enumerable]::SequenceEqual($p.NewBackup, (& $rb 'new-backup.bin')))"
$q = [SupermanGpt]::Plan((& $rb 'new-primary.bin'), (& $rb 'new-backup.bin'), 15269888, 5242880)
"replan of the android table: layout $($q.Layout), NewPrimary null: $($null -eq $q.NewPrimary), data $($q.DataFirst)-$($q.DataLast)"
"EFIESP: $([SupermanGpt]::Find((& $rb 'old-primary.bin'), 'EFIESP') -join '-')   Data: $([SupermanGpt]::Find((& $rb 'old-primary.bin'), 'Data') -join '-')"
foreach ($case in @{n='wrong disk size'; t=15269889}, @{n='data too small'; t=15269888; m=6000000}) {
  try { [void][SupermanGpt]::Plan((& $rb 'old-primary.bin'), (& $rb 'old-backup.bin'), $case.t, $(if ($case.m) { $case.m } else { 5242880 })); "$($case.n): NOT rejected" } catch { "$($case.n): rejected - $($_.Exception.InnerException.Message)" }
}
$bad = & $rb 'old-primary.bin'; $bad[600] = $bad[600] -bxor 1
try { [void][SupermanGpt]::Plan($bad, (& $rb 'old-backup.bin'), 15269888, 5242880); "corrupt header: NOT rejected" } catch { "corrupt header: rejected - $($_.Exception.InnerException.Message)" }
$bad = & $rb 'old-primary.bin'; $bad[2000] = $bad[2000] -bxor 1
try { [void][SupermanGpt]::Plan($bad, (& $rb 'old-backup.bin'), 15269888, 5242880); "corrupt entry: NOT rejected" } catch { "corrupt entry: rejected - $($_.Exception.InnerException.Message)" }
