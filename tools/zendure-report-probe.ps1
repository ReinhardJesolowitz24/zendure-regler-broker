<#
  zendure-report-probe.ps1
  --------------------------------------------------------------------------
  Notebook-seitige, NUR-LESENDE Diagnose fuer das lokale Zendure /properties/report.
  Greift NICHT in Regler/Broker/Geraet ein - reines HTTP GET + CSV-Log.

  Bedient drei Punkte aus dem Fable-5-Doku-Review + Live-Abgleich (2026-07-15):

   (1) A1-Verifikation Temperatur-Skalierung:
       Sobald packState != 0 UND packData gefuellt ist, werden die Zell-Rohwerte
       maxTemp/maxVol/minVol mitgeschnitten und in physikalische Einheiten
       umgerechnet (Kelvin*10 -> degC ;  value*0.01 -> V). Damit laesst sich
       endgueltig bestaetigen, dass unsere Guard-Skalierung stimmt UND dass die
       Zell-Guards im aktiven Betrieb ueberhaupt Daten sehen.

   (2) Backstop-Monitor:
       Prueft, ob die geraeteeigenen Grenzen konstant/plausibel bleiben:
         minSoc (erwartet 300 = 30%), socSet (1000 = 100%), chargeMaxLimit (2400 W).
       Drift wird auf der Konsole rot markiert und in der CSV vermerkt.

   (3) ts-Drift-Beobachtung:
       Der geraeteseitige Unix-Zeitstempel ts wird je Sample geloggt (roh + dekodiert
       + Differenz zur Notebook-Uhr), als Grundlage fuer eine spaetere Bewertung,
       ob ts als zusaetzliche Frische-Quelle taugt.

  Aufruf-Beispiele:
    # Endlos alle 30 s bis Strg+C:
    .\zendure-report-probe.ps1
    # Ueber Nacht, 60 s Takt:
    .\zendure-report-probe.ps1 -IntervalSec 60
    # Kurzer Funktionstest, 3 Samples:
    .\zendure-report-probe.ps1 -MaxSamples 3 -IntervalSec 3
#>

param(
  [string]$Ip            = "192.168.188.109",
  [int]   $IntervalSec   = 30,
  [int]   $MaxSamples    = 0,          # 0 = unbegrenzt
  [double]$DurationMin   = 0,          # 0 = unbegrenzt
  [string]$OutCsv        = "",         # leer -> zendure_probe_YYYYMMDD.csv neben dem Skript
  # erwartete Geraete-Backstops (Live-Stand 2026-07-15) fuer die Drift-Pruefung:
  [int]   $ExpMinSoc         = 300,
  [int]   $ExpSocSet         = 1000,
  [int]   $ExpChargeMaxLimit = 2400
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($OutCsv)) {
  $OutCsv = Join-Path $PSScriptRoot ("zendure_probe_{0}.csv" -f (Get-Date -Format "yyyyMMdd"))
}

# Umrechner ------------------------------------------------------------------
function ConvertTo-DegC([object]$rawK10) {
  if ($null -eq $rawK10 -or "$rawK10" -eq "") { return $null }
  return [math]::Round(([double]$rawK10) / 10.0 - 273.15, 2)
}
function ConvertTo-Volt([object]$rawCv) {
  if ($null -eq $rawCv -or "$rawCv" -eq "") { return $null }
  return [math]::Round(([double]$rawCv) * 0.01, 3)
}
function FieldOrBlank([object]$obj, [string]$name) {
  if ($null -eq $obj) { return "" }
  $prop = $obj.PSObject.Properties[$name]
  if ($null -eq $prop) { return "" }
  return $prop.Value
}

$url = "http://$Ip/properties/report"
Write-Host ("Zendure-Report-Probe -> {0}" -f $url) -ForegroundColor White
Write-Host ("CSV: {0}" -f $OutCsv) -ForegroundColor DarkGray
Write-Host ("Takt {0}s | MaxSamples {1} | DurationMin {2} | erwartet minSoc={3} socSet={4} chargeMaxLimit={5}" -f `
            $IntervalSec,$MaxSamples,$DurationMin,$ExpMinSoc,$ExpSocSet,$ExpChargeMaxLimit) -ForegroundColor DarkGray
Write-Host "Strg+C zum Beenden.`n" -ForegroundColor DarkGray

$startUtc      = (Get-Date).ToUniversalTime()
$n             = 0
$sawActiveCell = $false   # A1: schon einmal aktiver Pack MIT Zelldaten gesehen?

while ($true) {
  $n++
  $nowLocal = Get-Date
  $row = [ordered]@{
    local_time      = $nowLocal.ToString("yyyy-MM-dd HH:mm:ss")
    ok              = 0
    ts_raw          = ""
    ts_decoded      = ""
    ts_skew_s       = ""     # Geraete-ts minus Notebook-Uhr (s)
    electricLevel   = ""
    packState       = ""
    packNum         = ""
    dataReady       = ""
    packInputPower  = ""
    outputPackPower = ""
    outputHomePower = ""
    gridInputPower  = ""
    solarInputPower = ""
    inputLimit      = ""
    outputLimit     = ""
    hyperTmp_raw    = ""
    hyperTmp_C      = ""
    BatVolt_raw     = ""
    BatVolt_V       = ""
    cell_maxTemp_raw= ""
    cell_maxTemp_C  = ""
    cell_minTemp_raw= ""
    cell_minTemp_C  = ""
    cell_maxVol_raw = ""
    cell_maxVol_V   = ""
    cell_minVol_raw = ""
    cell_minVol_V   = ""
    cell_totalVol_V = ""
    faultLevel      = ""
    is_error        = ""
    heatState       = ""
    IOTState        = ""
    gridState       = ""
    minSoc          = ""
    socSet          = ""
    chargeMaxLimit  = ""
    socLimit        = ""
    backstop_drift  = ""
    note            = ""
  }

  try {
    $j = Invoke-RestMethod $url -TimeoutSec 6
    $p = $j.properties
    $row.ok = 1

    $row.electricLevel   = FieldOrBlank $p "electricLevel"
    $row.packState       = FieldOrBlank $p "packState"
    $row.packNum         = FieldOrBlank $p "packNum"
    $row.dataReady       = FieldOrBlank $p "dataReady"
    $row.packInputPower  = FieldOrBlank $p "packInputPower"
    $row.outputPackPower = FieldOrBlank $p "outputPackPower"
    $row.outputHomePower = FieldOrBlank $p "outputHomePower"
    $row.gridInputPower  = FieldOrBlank $p "gridInputPower"
    $row.solarInputPower = FieldOrBlank $p "solarInputPower"
    $row.inputLimit      = FieldOrBlank $p "inputLimit"
    $row.outputLimit     = FieldOrBlank $p "outputLimit"
    $row.faultLevel      = FieldOrBlank $p "faultLevel"
    $row.is_error        = FieldOrBlank $p "is_error"
    $row.heatState       = FieldOrBlank $p "heatState"
    $row.IOTState        = FieldOrBlank $p "IOTState"
    $row.gridState       = FieldOrBlank $p "gridState"
    $row.minSoc          = FieldOrBlank $p "minSoc"
    $row.socSet          = FieldOrBlank $p "socSet"
    $row.chargeMaxLimit  = FieldOrBlank $p "chargeMaxLimit"
    $row.socLimit        = FieldOrBlank $p "socLimit"

    # (3) ts-Drift
    $tsRaw = FieldOrBlank $p "ts"
    if ("$tsRaw" -ne "") {
      $row.ts_raw = $tsRaw
      try {
        $tsDt = [DateTimeOffset]::FromUnixTimeSeconds([int64]$tsRaw).ToLocalTime()
        $row.ts_decoded = $tsDt.ToString("yyyy-MM-dd HH:mm:ss")
        $row.ts_skew_s  = [math]::Round(($tsDt.LocalDateTime - $nowLocal).TotalSeconds, 0)
      } catch { $row.note = "ts nicht als Unix-s dekodierbar; " }
    }

    # hyperTmp / BatVolt (top-level, auch im Standby vorhanden)
    $row.hyperTmp_raw = FieldOrBlank $p "hyperTmp"
    $row.hyperTmp_C   = ConvertTo-DegC $row.hyperTmp_raw
    $row.BatVolt_raw  = FieldOrBlank $p "BatVolt"
    $row.BatVolt_V    = ConvertTo-Volt $row.BatVolt_raw

    # (1) A1: Zell-Rohwerte NUR wenn packData gefuellt
    $p0 = @($p.packData)[0]
    $cellPresent = $false
    if ($null -ne $p0 -and ($p0.PSObject.Properties.Count -gt 0)) {
      $mt = FieldOrBlank $p0 "maxTemp"; $nt = FieldOrBlank $p0 "minTemp"
      $mv = FieldOrBlank $p0 "maxVol";  $nv = FieldOrBlank $p0 "minVol"
      $tv = FieldOrBlank $p0 "totalVol"
      if ("$mt" -ne "" -or "$mv" -ne "") {
        $cellPresent = $true
        $row.cell_maxTemp_raw = $mt; $row.cell_maxTemp_C = ConvertTo-DegC $mt
        $row.cell_minTemp_raw = $nt; $row.cell_minTemp_C = ConvertTo-DegC $nt
        $row.cell_maxVol_raw  = $mv; $row.cell_maxVol_V  = ConvertTo-Volt $mv
        $row.cell_minVol_raw  = $nv; $row.cell_minVol_V  = ConvertTo-Volt $nv
        $row.cell_totalVol_V  = ConvertTo-Volt $tv
      }
    }

    # (2) Backstop-Drift
    $drift = @()
    if ("$($row.minSoc)"         -ne "" -and [int]$row.minSoc         -ne $ExpMinSoc)         { $drift += ("minSoc={0}!={1}"         -f $row.minSoc,$ExpMinSoc) }
    if ("$($row.socSet)"         -ne "" -and [int]$row.socSet         -ne $ExpSocSet)         { $drift += ("socSet={0}!={1}"         -f $row.socSet,$ExpSocSet) }
    if ("$($row.chargeMaxLimit)" -ne "" -and [int]$row.chargeMaxLimit -ne $ExpChargeMaxLimit) { $drift += ("chargeMaxLimit={0}!={1}" -f $row.chargeMaxLimit,$ExpChargeMaxLimit) }
    if ($drift.Count -gt 0) { $row.backstop_drift = ($drift -join "; ") }

    # Konsolen-Zeile
    $active = ("$($row.packState)" -ne "" -and [int]$row.packState -ne 0)
    $line = ("[{0}] SoC={1}% packState={2} hyperTmp={3}C pIn={4} pOut={5} home={6} grid={7}" -f `
             $nowLocal.ToString("HH:mm:ss"),$row.electricLevel,$row.packState,$row.hyperTmp_C,`
             $row.packInputPower,$row.outputPackPower,$row.outputHomePower,$row.gridInputPower)
    if ($cellPresent) {
      Write-Host ($line + (" | ZELLE maxTemp={0}C minTemp={1}C maxVol={2}V minVol={3}V" -f `
                  $row.cell_maxTemp_C,$row.cell_minTemp_C,$row.cell_maxVol_V,$row.cell_minVol_V)) -ForegroundColor Green
      if (-not $sawActiveCell) {
        $sawActiveCell = $true
        Write-Host ("  >>> A1 erfuellt: aktiver Pack MIT Zelldaten. Zell-maxTemp roh={0} -> {1}C (Kelvin*10-Konvention {2}). Zell-Guards sehen Daten." -f `
                    $row.cell_maxTemp_raw, $row.cell_maxTemp_C, `
                    ($(if ($row.cell_maxTemp_C -ge -20 -and $row.cell_maxTemp_C -le 70) { "PLAUSIBEL" } else { "PRUEFEN!" }))) -ForegroundColor Yellow
        $row.note += "A1: erste aktive Zelldaten; "
      }
    } else {
      $col = if ($active) { "Cyan" } else { "Gray" }
      Write-Host ($line + $(if ($active) { " | aktiv, aber packData leer" } else { " | idle" })) -ForegroundColor $col
    }
    if ($row.backstop_drift -ne "") {
      Write-Host ("  !!! BACKSTOP-DRIFT: {0}" -f $row.backstop_drift) -ForegroundColor Red
    }
    if ("$($row.is_error)" -ne "" -and [int]$row.is_error -ne 0) {
      Write-Host ("  !!! is_error={0} faultLevel={1}" -f $row.is_error,$row.faultLevel) -ForegroundColor Red
    }
  }
  catch {
    Write-Host ("[{0}] FEHLER: {1}" -f $nowLocal.ToString("HH:mm:ss"), $_.Exception.Message) -ForegroundColor Red
    $row.note += ("fetch-fail: " + $_.Exception.Message)
  }

  [pscustomobject]$row | Export-Csv -Path $OutCsv -NoTypeInformation -Append -Encoding UTF8

  # Abbruchbedingungen
  if ($MaxSamples  -gt 0 -and $n -ge $MaxSamples) { break }
  if ($DurationMin -gt 0 -and ((Get-Date).ToUniversalTime() - $startUtc).TotalMinutes -ge $DurationMin) { break }
  Start-Sleep -Seconds $IntervalSec
}

Write-Host ("`nFertig. {0} Samples -> {1}" -f $n, $OutCsv) -ForegroundColor White
if (-not $sawActiveCell) {
  Write-Host "Hinweis: In diesem Lauf war der Pack durchgehend im Standby (packData leer) - A1-Zellverifikation steht noch aus. Skript laufen lassen, bis ein Lade-/Entladefenster kommt." -ForegroundColor Yellow
}
