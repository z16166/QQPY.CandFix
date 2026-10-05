param([int]$Seconds = 300, [string]$Log = 'H:\github\QQPY.CandFix\_win.log')

Add-Type -Namespace QW -Name C -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EProc f, IntPtr l);
public delegate bool EProc(IntPtr h, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
'@

$sw=[Diagnostics.Stopwatch]::StartNew()
$prev=''
"=== watch start $(Get-Date -Format HH:mm:ss) ===" | Out-File -Encoding utf8 $Log
while($sw.Elapsed.TotalSeconds -lt $Seconds){
    $wt=Get-Process WindowsTerminal -ErrorAction SilentlyContinue
    $pids=@(); if($wt){ $pids=@($wt | Select-Object -ExpandProperty Id) }
    $rows=New-Object System.Collections.ArrayList
    $fg=[QW.C]::GetForegroundWindow(); $fgp=0; [void][QW.C]::GetWindowThreadProcessId($fg,[ref]$fgp)

    if($pids.Count -and $wt){
      foreach($p in $wt){
        $h=$p.MainWindowHandle
        if($h -ne [IntPtr]::Zero){
          $r=New-Object QW.C+RECT; [void][QW.C]::GetWindowRect($h,[ref]$r)
          [void]$rows.Add(("WINDOW    [{0}] vis={1} rect={2},{3}-{4},{5} ({6}x{7})" -f $p.Id,[QW.C]::IsWindowVisible($h),$r.Left,$r.Top,$r.Right,$r.Bottom,$r.Right-$r.Left,$r.Bottom-$r.Top))
        }
      }
      $cb=[QW.C+EProc]{ param($h,$l)
        $pp=0; [void][QW.C]::GetWindowThreadProcessId($h,[ref]$pp)
        if($pids -contains [int]$pp){
          $c=New-Object Text.StringBuilder 256; [void][QW.C]::GetClassNameW($h,$c,256); $cn=$c.ToString()
          if($cn -like 'QQPinyin*' -and [QW.C]::IsWindowVisible($h)){
            $r=New-Object QW.C+RECT; [void][QW.C]::GetWindowRect($h,[ref]$r)
            [void]$rows.Add(("  CANDIDATE {0} rect={1},{2}-{3},{4} ({5}x{6})" -f $cn,$r.Left,$r.Top,$r.Right,$r.Bottom,$r.Right-$r.Left,$r.Bottom-$r.Top))
          } }
        return $true }
      [void][QW.C]::EnumWindows($cb,[IntPtr]::Zero)
    }
    $sig=($rows | Sort-Object) -join "`n"
    if($sig -ne $prev){
      $prev=$sig
      "$(Get-Date -Format 'HH:mm:ss.fff') fgPid=$fgp" | Out-File -Append -Encoding utf8 $Log
      foreach($r in ($rows | Sort-Object)){ "    $r" | Out-File -Append -Encoding utf8 $Log }
    }
    Start-Sleep -Milliseconds 120
}
"=== end ===" | Out-File -Append -Encoding utf8 $Log
