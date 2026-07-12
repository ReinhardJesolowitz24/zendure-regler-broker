# l3_alert.ps1 -- L3: externe Alarmierung (Beobachtungs-Domaene, unabhaengig von den ESP32-Boards).
#   Pollt read-only /status von Regler + Broker und meldet Alarm-Bedingungen per Telegram/E-Mail.
#   Nutzung:  .\l3_alert.ps1            (Dauerlauf)
#             .\l3_alert.ps1 -Test      (eine Test-Meldung senden + beenden -> Kanal pruefen)
param([switch]$Test)

# --- Config laden ---
$cfgPath = Join-Path $PSScriptRoot "l3_config.ps1"
if (-not (Test-Path $cfgPath)) { Write-Error "l3_config.ps1 fehlt (l3_config.example.ps1 kopieren + ausfuellen)."; exit 1 }
. $cfgPath

function Get-J($u){ try { Invoke-RestMethod -Uri $u -TimeoutSec 5 } catch { $null } }

# --- Notification (Telegram ODER E-Mail) ---
function Send-Alert($subject, $body) {
  $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
  $text  = "[Zendure-L3] $subject`n$body`n$stamp"
  try {
    if ($L3.Channel -eq "telegram") {
      Invoke-RestMethod -Method Post -TimeoutSec 10 `
        -Uri "https://api.telegram.org/bot$($L3.TgToken)/sendMessage" `
        -Body @{ chat_id = $L3.TgChat; text = $text } | Out-Null
    } else {
      $sec  = ConvertTo-SecureString $L3.MailPass -AsPlainText -Force
      $cred = New-Object System.Management.Automation.PSCredential($L3.MailUser, $sec)
      Send-MailMessage -SmtpServer $L3.SmtpServer -Port $L3.SmtpPort -UseSsl -Credential $cred `
        -From $L3.MailFrom -To $L3.MailTo -Subject "[Zendure-L3] $subject" -Body $text
    }
    Write-Host "$stamp  SENT: $subject"
  } catch { Write-Host "$stamp  SEND-FEHLER ($($_.Exception.Message)): $subject" -ForegroundColor Red }
}

if ($Test) { Send-Alert "Testmeldung" "L3 laeuft und der Kanal funktioniert."; exit 0 }

# --- Zustand ---
$prev   = @{}                    # Zaehler-Baselines (Trip/Boot): -1 = noch nicht gesetzt
"monTrip","bypass","rMonTrip","bootB","bootR" | ForEach-Object { $prev[$_] = -1 }
$active = @{}                    # Level-Bedingungen (an/aus) -> Enter/Recovery
$lastSent = @{}                 # key -> letzte Sendezeit (Anti-Spam)
$fail   = @{ b = 0; r = 0 }     # aufeinanderfolgende Poll-Fehler

function Fire($key, $subject, $body) {   # edge/level-Alarm mit Renotify-Sperre
  if ($lastSent.ContainsKey($key) -and ((Get-Date) - $lastSent[$key]).TotalMinutes -lt $L3.RenotifyMin) { return }
  Send-Alert $subject $body; $lastSent[$key] = Get-Date
}
function Level($key, $isOn, $subject, $body) {   # Enter beim Wechsel aus->an; Recovery an->aus
  $was = $active.ContainsKey($key) -and $active[$key]
  if ($isOn -and -not $was) { $active[$key] = $true;  Fire $key $subject $body }
  elseif (-not $isOn -and $was) { $active[$key] = $false; Send-Alert "$subject -- behoben" "Bedingung wieder normal." }
}

Write-Host "L3-Alerter laeuft (Kanal: $($L3.Channel), Poll ${($L3.PollSeconds)}s). Regler=$($L3.ReglerHost) Broker=$($L3.BrokerHost)"
Send-Alert "L3 gestartet" "Ueberwachung Regler+Broker aktiv."

while ($true) {
  $b = Get-J "http://$($L3.BrokerHost)/status"
  $r = Get-J "http://$($L3.ReglerHost)/status"

  # --- Erreichbarkeit (mit Strike-Filter gegen Netz-Blips) ---
  if ($b) { $fail.b = 0 } else { $fail.b++ }
  if ($r) { $fail.r = 0 } else { $fail.r++ }
  Level "domain_dead" (($fail.b -ge $L3.FailStrikes) -and ($fail.r -ge $L3.FailStrikes)) "STEUER-DOMAENE TOT" "Broker UND Regler nicht erreichbar."
  Level "broker_down" (($fail.b -ge $L3.FailStrikes) -and $r) "Broker nicht erreichbar" "Regler noch da; Kommandopfad evtl. tot."
  Level "regler_down" (($fail.r -ge $L3.FailStrikes) -and $b) "Regler nicht erreichbar" "Broker noch da; Enforcer sollte idlen."

  # --- Broker-basierte Alarme ---
  if ($b) {
    $mt = [int]$b.monitor_trip_count; $by = [int]$b.bypass_trip_count; $bb = [int]$b.boot_count
    if ($prev.monTrip -ge 0 -and $mt -gt $prev.monTrip) { Fire "montrip" "Gate g: Monitor-Trip" "monitor_trip_count $($prev.monTrip)->$mt (ungueltiger Sollwert geblockt), last_inv=$($b.last_trip_inv)" }
    if ($prev.bypass  -ge 0 -and $by -gt $prev.bypass)  { Fire "bypass"  "Gate g: BYPASS erkannt"  "bypass_trip_count $($prev.bypass)->$by -- FREMD-Publish am Gate vorbei!" }
    if ($prev.bootB   -ge 0 -and $bb -gt $prev.bootB)   { Fire "bootB"   "Broker-Reboot"           "boot_count $($prev.bootB)->$bb, reset_reason=$($b.reset_reason)" }
    Level "enforcer" ($b.enforcer_active -eq $true)          "Regler stumm (Enforcer L2 aktiv)" "Heartbeat weg -> Broker haelt Geraet idle."
    Level "clients"  ([int]$b.clients_connected -lt 2)       "Zendure nicht am Broker"          "clients=$($b.clients_connected) -> Regelung blind."
    $prev.monTrip=$mt; $prev.bypass=$by; $prev.bootB=$bb
  }

  # --- Regler-basierte Alarme ---
  if ($r) {
    $rmt = [int]$r.mon_trip_count; $rb = [int]$r.boot_count; $soc = [int]$r.soc
    if ($prev.rMonTrip -ge 0 -and $rmt -gt $prev.rMonTrip) { Fire "rmontrip" "Regler L1-Monitor-Trip" "mon_trip_count $($prev.rMonTrip)->$rmt (interner Endwert-Check)" }
    if ($prev.bootR    -ge 0 -and $rb  -gt $prev.bootR)    { Fire "bootR"    "Regler-Reboot"           "boot_count $($prev.bootR)->$rb, reset_reason=$($r.reset_reason)" }
    Level "lowsoc" (($soc -ge 0) -and ($soc -le $L3.SocAlert)) "SoC niedrig" "SoC=$soc% (naehert sich Floor 30%)."
    $prev.rMonTrip=$rmt; $prev.bootR=$rb
  }

  Start-Sleep -Seconds $L3.PollSeconds
}
