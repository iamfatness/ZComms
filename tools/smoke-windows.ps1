<#
.SYNOPSIS
  Windows run-verification for ZComms: joins a real meeting, brings the channel
  bank up, keys a channel, and asserts audio actually reached Zoom.

.DESCRIPTION
  CI proves the Windows tree COMPILES and that the unit tests pass. It cannot
  prove the app still joins a meeting and keys a channel, because none of that
  runs in CI. This script closes that gap, and it is the acceptance test for
  any change that rewires main.cpp's live construction path.

  Every assertion is machine-checkable. The 2026-08-29 no-audio hunt cost a
  full day precisely because "it looked fine" and "fails == 0" were both true
  while every listener heard silence -- so this checks the two things that
  distinguish those cases:

    chsends  Zoom ACCEPTED a channel send. Rules out "nothing was ever sent".
    txpeak   post-envelope level. Rules out "we shipped silence, successfully".

  Neither alone is sufficient. Both together mean audio left this machine.

  It does NOT prove a human heard anything -- Zoom's delivery to a listener is
  the one leg no local assertion can reach. For that, put a real Zoom client
  on the channel and listen, or run tools/tap-detect. This script proves
  everything up to the SDK boundary.

.PARAMETER Meeting
  Meeting number to join. Required.

.PARAMETER Passcode
  Meeting passcode, if the meeting has one.

.PARAMETER Name
  Display name in the meeting. Defaults to "ZComms smoke".

.PARAMETER Channels
  Size of the channel bank to provision. Defaults to 2 -- enough to prove the
  routing rule (audio goes to the keyed channel and NOT the other one) without
  waiting on a 16-channel bring-up.

.PARAMETER Port
  Control-server port. Defaults to 7399, deliberately not the app's usual 7350
  so a smoke run cannot collide with an operator's live session.

.PARAMETER Exe
  Path to zcomms.exe. Defaults to the usual dev build output.

.PARAMETER JoinTimeoutSec
  How long to wait for the meeting. Defaults to 90 -- a waiting room or a
  host-not-started meeting legitimately takes a while.

.PARAMETER BankTimeoutSec
  How long to wait for the channel bank. Defaults to 120. Channel creation
  needs host or co-host, and the app retries while you arrange it.

.EXAMPLE
  .\tools\smoke-windows.ps1 -Meeting 95512345678 -Passcode hunter2

.NOTES
  Requires: the meeting to exist, and ZComms to be made host or co-host once
  it joins (Zoom requires that role to create talkback channels). The script
  says so, loudly, if the bank never comes up.
#>

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Meeting,
  [string]$Passcode = "",
  [string]$Name = "ZComms smoke",
  [int]$Channels = 2,
  [int]$Port = 7399,
  [string]$Exe = "build\Release\zcomms.exe",
  [int]$JoinTimeoutSec = 90,
  [int]$BankTimeoutSec = 120
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Windows PowerShell 5.1 does not load System.Net.Http by default, and the SSE
# reader below needs HttpClient's ResponseHeadersRead -- Invoke-WebRequest
# buffers the whole response and would never return on an endless stream.
try { Add-Type -AssemblyName System.Net.Http -ErrorAction Stop } catch { }

# ---------------------------------------------------------------------------
# Result accounting. Every check lands here; nothing prints a verdict early,
# because a partial pass read as a pass is how a broken build ships.
# ---------------------------------------------------------------------------
$script:Checks = @()
$script:Proc = $null

function Add-Check {
  param([string]$Name, [bool]$Ok, [string]$Detail)
  $script:Checks += [pscustomobject]@{ Name = $Name; Ok = $Ok; Detail = $Detail }
  $mark = if ($Ok) { "PASS" } else { "FAIL" }
  Write-Host ("  [{0}] {1}" -f $mark, $Name) -ForegroundColor $(if ($Ok) { "Green" } else { "Red" })
  if ($Detail) { Write-Host ("         {0}" -f $Detail) -ForegroundColor DarkGray }
}

function Fail-Fast {
  param([string]$Message)
  Write-Host ""
  Write-Host "ABORT: $Message" -ForegroundColor Red
  Show-Verdict
  exit 2
}

# ---------------------------------------------------------------------------
# State comes over SSE (GET /events, a snapshot every ~100 ms). Invoke-WebRequest
# buffers the whole response and would never return on an endless stream, so
# read the raw stream and stop at the first complete event.
# ---------------------------------------------------------------------------
function Get-ZcState {
  param([int]$TimeoutSec = 5)

  $client = [System.Net.Http.HttpClient]::new()
  $client.Timeout = [TimeSpan]::FromSeconds($TimeoutSec)
  # Declared before the try so the finally can test it under StrictMode --
  # an unassigned variable there is a terminating error, which would mask
  # whatever actually went wrong.
  $reader = $null
  try {
    $resp = $client.GetAsync("http://127.0.0.1:$Port/events",
              [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
    if (-not $resp.IsSuccessStatusCode) { return $null }
    $stream = $resp.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
    $reader = [System.IO.StreamReader]::new($stream)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
      $line = $reader.ReadLine()
      if ($null -eq $line) { break }
      if ($line.StartsWith("data:")) {
        $json = $line.Substring(5).Trim()
        if ($json) { return ($json | ConvertFrom-Json) }
      }
    }
    return $null
  } catch {
    return $null
  } finally {
    if ($reader) { $reader.Dispose() }
    $client.Dispose()
  }
}

function Wait-ZcState {
  param(
    [scriptblock]$Until,
    [int]$TimeoutSec,
    [string]$What
  )
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  $last = $null
  while ((Get-Date) -lt $deadline) {
    if ($script:Proc -and $script:Proc.HasExited) {
      Fail-Fast "zcomms.exe exited (code $($script:Proc.ExitCode)) while waiting for $What"
    }
    $s = Get-ZcState
    if ($s) {
      $last = $s
      if (& $Until $s) { return $s }
    }
    Start-Sleep -Milliseconds 700
  }
  return $last   # caller decides whether a timeout is fatal
}

function Send-ZcAct {
  param([string]$Verb)
  try {
    Invoke-WebRequest -Uri "http://127.0.0.1:$Port/act" -Method Post `
      -Body $Verb -TimeoutSec 5 -UseBasicParsing | Out-Null
    return $true
  } catch {
    return $false
  }
}

# `channels` is an array in the JSON, but ConvertFrom-Json hands back a bare
# object when there is exactly one element. Normalise so indexing is safe.
function Get-Channels {
  param($State)
  if (-not $State) { return @() }
  if (-not $State.PSObject.Properties.Name.Contains("channels")) { return @() }
  return @($State.channels)
}

function Show-Verdict {
  Write-Host ""
  Write-Host "================ ZComms Windows smoke ================" -ForegroundColor Cyan
  $failed = @($script:Checks | Where-Object { -not $_.Ok })
  foreach ($c in $script:Checks) {
    $mark = if ($c.Ok) { "PASS" } else { "FAIL" }
    Write-Host ("{0,-4}  {1}" -f $mark, $c.Name)
  }
  Write-Host ""
  if ($script:Checks.Count -eq 0) {
    Write-Host "NO CHECKS RAN" -ForegroundColor Red
  } elseif ($failed.Count -eq 0) {
    Write-Host ("SMOKE PASS -- {0}/{0} checks" -f $script:Checks.Count) -ForegroundColor Green
  } else {
    Write-Host ("SMOKE FAIL -- {0} of {1} checks failed" -f $failed.Count, $script:Checks.Count) -ForegroundColor Red
    $logDir = Join-Path $env:APPDATA "ZComms\logs"
    if (Test-Path $logDir) {
      $newest = Get-ChildItem $logDir -Filter *.log | Sort-Object LastWriteTime -Descending | Select-Object -First 1
      if ($newest) {
        Write-Host ""
        Write-Host "Last 40 lines of $($newest.FullName):" -ForegroundColor Yellow
        Get-Content $newest.FullName -Tail 40
      }
    }
  }
  Write-Host "======================================================" -ForegroundColor Cyan
}

function Stop-Zc {
  if ($script:Proc -and -not $script:Proc.HasExited) {
    Write-Host "  (forcing exit -- the app did not quit on its own)" -ForegroundColor Yellow
    try { $script:Proc.Kill() } catch { }
  }
}

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
Write-Host "ZComms Windows smoke test" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path $Exe)) {
  Fail-Fast "zcomms.exe not found at '$Exe'. Build it, or pass -Exe."
}
$exeFull = (Resolve-Path $Exe).Path
Write-Host "exe      : $exeFull"
Write-Host "meeting  : $Meeting"
Write-Host "channels : $Channels"
Write-Host "port     : $Port"
Write-Host ""

$portBusy = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($portBusy) {
  Fail-Fast "port $Port is already listening -- another ZComms is running. Quit it, or pass -Port."
}

# An already-running instance would take the Zoom SDK singleton and this run
# would die on SDKERR_OTHER_SDK_INSTANCE_RUNNING (14) with a confusing message.
$existing = Get-Process -Name zcomms -ErrorAction SilentlyContinue
if ($existing) {
  Fail-Fast "zcomms.exe is already running (pid $($existing.Id -join ', ')). The Zoom SDK refuses a second instance."
}

# ---------------------------------------------------------------------------
# Launch
#
# --no-open      no WebView2 window and no browser: this run is driven by the
#                control API, and a window would steal focus on the operator's
#                desktop for no benefit.
# --test-signal  puts a 700/1000 Hz pattern through the REAL chain, so txpeak
#                is a meaningful number instead of whatever the room's mic
#                happened to pick up. Without it, a silent room would make a
#                healthy run look like a failure.
# --seconds      a hard ceiling. A smoke test that can hang forever is a smoke
#                test nobody runs twice.
# ---------------------------------------------------------------------------
$appArgs = @(
  "--meeting", $Meeting,
  "--name", $Name,
  "--channels", $Channels,
  "--ui-port", $Port,
  "--no-open",
  "--test-signal",
  "--seconds", ($JoinTimeoutSec + $BankTimeoutSec + 120)
)
if ($Passcode) { $appArgs += @("--passcode", $Passcode) }

Write-Host "Launching..." -ForegroundColor Cyan
$script:Proc = Start-Process -FilePath $exeFull -ArgumentList $appArgs -PassThru

try {
  # -- 1. the control surface comes up ---------------------------------------
  $s = Wait-ZcState -Until { param($x) $true } -TimeoutSec 30 -What "the control server"
  Add-Check "control server answers on :$Port" ($null -ne $s) `
    $(if ($s) { "phase=$($s.phase)" } else { "no /events response in 30s" })
  if (-not $s) { Fail-Fast "the control server never answered -- the app may have died at startup" }

  # -- 2. the meeting --------------------------------------------------------
  Write-Host "Waiting for the meeting (up to ${JoinTimeoutSec}s)..." -ForegroundColor Cyan
  $s = Wait-ZcState -Until { param($x) $x.phase -eq "up" } -TimeoutSec $JoinTimeoutSec -What "the meeting"
  $inMeeting = ($s -and $s.phase -eq "up")
  Add-Check "joined the meeting" $inMeeting `
    $(if ($inMeeting) { "status=$($s.status)" } else { "phase=$(if($s){$s.phase}else{'?'}) status=$(if($s){$s.status}else{'?'})" })
  if (-not $inMeeting) { Fail-Fast "never reached the meeting -- see the status above and the log tail below" }

  # -- 3. delivery law 1 -----------------------------------------------------
  # Talkback delivers ONLY while this client's meeting audio is open. Muted,
  # SendAudioDataToChannel is ACCEPTED -- sends count, zero failures -- and
  # every member hears silence (owner-found live, 2026-08-29). If this check
  # fails, every later check can still pass while nobody hears a thing.
  Add-Check "meeting mic is OPEN (delivery law 1)" ([bool]$s.sdkmic) `
    $(if ($s.sdkmic) { "sdkmic=true" } else { "sdkmic=false -- talkback will be ACCEPTED and INAUDIBLE" })

  # -- 4. the channel bank ---------------------------------------------------
  Write-Host "Waiting for the channel bank (up to ${BankTimeoutSec}s)..." -ForegroundColor Cyan
  Write-Host "  NOTE: Zoom requires host or co-host to create channels." -ForegroundColor Yellow
  Write-Host "        Make '$Name' host or co-host now if you have not." -ForegroundColor Yellow
  $s = Wait-ZcState -TimeoutSec $BankTimeoutSec -What "the channel bank" -Until {
    param($x)
    $ch = Get-Channels $x
    ($ch.Count -ge 1) -and ($ch[0].ready -eq $true)
  }
  $ch = Get-Channels $s
  $bankUp = ($ch.Count -ge 1 -and $ch[0].ready -eq $true)
  $readyCount = @($ch | Where-Object { $_.ready -eq $true }).Count
  Add-Check "channel bank provisioned" $bankUp `
    $(if ($bankUp) { "$readyCount of $($ch.Count) channels ready" } else { "no channel reported ready -- was ZComms made host/co-host?" })
  if (-not $bankUp) { Fail-Fast "the channel bank never came up" }

  # -- 5. baseline before keying --------------------------------------------
  # Captured so the deltas below mean something. A raw chsends value proves
  # nothing; a chsends that MOVED while keyed is the evidence.
  $chsends0 = [int64]$s.chsends
  $fails0   = [int64]$s.fails
  Add-Check "not keyed at rest" (-not $ch[0].keyed) "chsends=$chsends0 fails=$fails0"

  # -- 6. key it -------------------------------------------------------------
  Write-Host "Keying channel 1..." -ForegroundColor Cyan
  $sent = Send-ZcAct "talk 0 on"
  Add-Check "POST /act 'talk 0 on' accepted" $sent

  $s = Wait-ZcState -TimeoutSec 10 -What "the key to register" -Until {
    param($x)
    $c = Get-Channels $x
    ($c.Count -ge 1) -and ($c[0].keyed -eq $true)
  }
  $ch = Get-Channels $s
  $keyed = ($ch.Count -ge 1 -and $ch[0].keyed -eq $true)
  Add-Check "channel 1 reads KEYED" $keyed

  # Let a few seconds of audio actually flow. 20 ms frames, so ~150 ticks.
  Start-Sleep -Seconds 3
  $s = Get-ZcState
  if (-not $s) { Fail-Fast "lost the control server while keyed" }

  # -- 7. the two numbers that matter ---------------------------------------
  $chsends1 = [int64]$s.chsends
  $fails1   = [int64]$s.fails
  $txpeak   = [double]$s.txpeak

  $sendsMoved = ($chsends1 -gt $chsends0)
  Add-Check "Zoom ACCEPTED channel sends while keyed" $sendsMoved `
    ("chsends {0} -> {1} (delta {2})" -f $chsends0, $chsends1, ($chsends1 - $chsends0))

  # txpeak is post-envelope, which is the whole point: it separates
  # "transmitting" from "shipping silence, successfully". A run where chsends
  # climbs and txpeak stays at zero is exactly the 2026-08-29 failure.
  $carryingAudio = ($txpeak -gt 0.001)
  Add-Check "the sends CARRIED AUDIO, not silence" $carryingAudio `
    ("txpeak={0:F4} (post-envelope; >0.001 means real signal)" -f $txpeak)

  $noFailures = ($fails1 -eq $fails0)
  Add-Check "no send failures while keyed" $noFailures `
    ("fails {0} -> {1}" -f $fails0, $fails1)

  # -- 8. the routing rule ---------------------------------------------------
  # The product's core claim: a frame goes to a channel if and only if that
  # channel is keyed. Only checkable when a second channel exists.
  if ($ch.Count -ge 2) {
    Add-Check "channel 2 stayed UNKEYED while channel 1 was keyed" (-not $ch[1].keyed) `
      "routing rule: audio goes only where it was keyed"
  } else {
    Write-Host "  (skipping the routing-rule check -- run with -Channels 2 or more)" -ForegroundColor DarkGray
  }

  # -- 9. release ------------------------------------------------------------
  Write-Host "Releasing..." -ForegroundColor Cyan
  Send-ZcAct "talk 0 off" | Out-Null
  $s = Wait-ZcState -TimeoutSec 10 -What "the key to release" -Until {
    param($x)
    $c = Get-Channels $x
    ($c.Count -ge 1) -and ($c[0].keyed -eq $false)
  }
  $ch = Get-Channels $s
  $released = ($ch.Count -ge 1 -and $ch[0].keyed -eq $false)
  Add-Check "channel 1 reads RELEASED" $released

  # Sends must STOP. A key that releases visually but keeps transmitting is
  # an open mic the operator believes is shut -- the worst failure this
  # product has.
  $chsends2 = [int64]$s.chsends
  Start-Sleep -Seconds 2
  $s = Get-ZcState
  $chsends3 = if ($s) { [int64]$s.chsends } else { $chsends2 }
  $stopped = ($chsends3 -eq $chsends2)
  Add-Check "sends STOPPED after release" $stopped `
    ("chsends {0} -> {1} over 2s (must not move)" -f $chsends2, $chsends3)

  # -- 10. clean quit --------------------------------------------------------
  Write-Host "Quitting..." -ForegroundColor Cyan
  Send-ZcAct "quit" | Out-Null
  $exited = $script:Proc.WaitForExit(20000)
  Add-Check "quit cleanly within 20s" $exited `
    $(if ($exited) { "exit code $($script:Proc.ExitCode)" } else { "still running -- possible teardown hang" })

  if ($exited -and $script:Proc.ExitCode -ne 0) {
    Add-Check "exit code is 0" $false "exit code $($script:Proc.ExitCode)"
  }
}
finally {
  Stop-Zc
}

Show-Verdict
if (@($script:Checks | Where-Object { -not $_.Ok }).Count -gt 0) { exit 1 }
exit 0
