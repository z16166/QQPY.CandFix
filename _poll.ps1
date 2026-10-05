param([int]$Seconds = 300, [string]$Log = 'H:\github\QQPY.CandFix\_poll.log')

$sw = [Diagnostics.Stopwatch]::StartNew()
"=== poll start $(Get-Date -Format HH:mm:ss) ===" | Out-File -Encoding utf8 $Log
$prev = @{}
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $procs = Get-Process WindowsTerminal -ErrorAction SilentlyContinue
    if ($procs) {
        foreach ($p in $procs) {
            $m = ''
            try { $m = (($p.Modules | Where-Object { $_.ModuleName -match 'qqpyproxy|QQPinyin' } | Select-Object -ExpandProperty ModuleName) -join ',') } catch { $m = '<no access>' }
            $key = "$($p.Id)"
            $val = "$($p.StartTime.ToString('HH:mm:ss'))|$m"
            if (-not $prev.ContainsKey($key) -or $prev[$key] -ne $val) {
                $prev[$key] = $val
                "$(Get-Date -Format 'HH:mm:ss.fff')  WT pid=$($p.Id) start=$($p.StartTime.ToString('HH:mm:ss'))  modules: $m" | Out-File -Append -Encoding utf8 $Log
            }
        }
    } else {
        if (-not $prev.ContainsKey('none')) { $prev['none'] = '1'; "$(Get-Date -Format 'HH:mm:ss.fff')  (没有 WindowsTerminal 进程)" | Out-File -Append -Encoding utf8 $Log }
    }
    if ($procs) { $prev.Remove('none') | Out-Null }
    Start-Sleep -Milliseconds 500
}
"=== poll end $(Get-Date -Format HH:mm:ss) ===" | Out-File -Append -Encoding utf8 $Log
