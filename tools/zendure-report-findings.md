# Zendure `/properties/report` — Befund-Report (Feld-Referenz + Verifikation)

**Stand:** 2026-07-15
**Quellen:** (a) Fable-5-Review der offiziellen Zendure-Doku
`https://github.com/Zendure/developer-device-data-report` (einzige inhaltliche Datei: `README.md`,
„Zendure Operating System v1.0.0", beschreibt nur den **Cloud-MQTT-Uplink**);
(b) Live-Abgleich am lokalen Gerät `http://192.168.188.109/properties/report` (SolarFlow 2400 Pro).
**Werkzeug:** `zendure-report-probe.ps1` (nur-lesend), CSV-Log `zendure_probe_YYYYMMDD.csv`.

> **Wichtige Einordnung:** Die Cloud-Doku nennt die SolarFlow **2400 Pro nirgends** und dokumentiert
> unseren lokalen HTTP-Endpunkt nicht (steht offiziell unter „Future Plans: device LAN communication").
> Die Doku **stützt**, sie **ersetzt nicht** die empirische Verifikation. Feldsemantik kann sich per
> Firmware-Update unangekündigt ändern → FW-Version des Geräts im Auge behalten.

---

## 1. Skalierungs-Referenz (für die Guards entscheidend)

| Größe | Feld | Rohwert-Beispiel | Umrechnung | Ergebnis | Beleg |
|---|---|---|---|---|---|
| Zellspannung | `packData[].maxVol`/`minVol`/`totalVol` | 370 | `× 0,01` V | 3,70 V | **Doku wörtlich** „value * 0.01, unit: V" ✅ |
| Temperatur | `hyperTmp` (Geräte-/Inverter-Temp) | 2981 | `/10 − 273,15` °C | 25,0 °C | **Live bewiesen** (= Raumtemp) → Kelvin×10 ✅ |
| Temperatur | `packData[].maxTemp`/`minTemp` (Zelle) | *(steht aus)* | `/10 − 273,15` °C | — | Konvention via `hyperTmp` bewiesen; **direkter Zellwert noch zu bestätigen** ⚠️ |
| Akkuspannung | `BatVolt` | 5043 | `× 0,01` V | 50,4 V | Live, plausibel (48-V-Klasse) |
| SoC | `electricLevel` | 98 | — (Prozent) | 98 % | Doku „Device battery percentage" ✅ (= Ground Truth) |

**Konvention dieses Geräts: Temperatur = Kelvin × 10, Spannung = Wert × 0,01 V.**
Die Cloud-Doku sagt bei Temperatur nur „Kelvin" und **lässt den Faktor 10 offen** — deshalb war die
Live-Verifikation nötig. `hyperTmp=2981 → 25,0 °C` schließt diese Lücke für die Konvention.

---

## 2. Geräteeigene Backstops (lesbar, als zusätzliche Grenz-Monitore nutzbar)

| Feld | Rohwert | Bedeutung | Umrechnung | Deckt sich mit |
|---|---|---|---|---|
| `minSoc` | 300 | Entlade-Floor | ×… → **30 %** | Regler `SOC_STOP_DISCHARGE=30` ✅ |
| `socSet` | 1000 | Lade-Ceiling („Charge Capacity Limitation") | **×10 → 100 %** (Doku-Beispiel zeigt Prozent → unser Gerät skaliert ×10!) | Regler `SOC_STOP_CHARGE=98` (Gerät erlaubt bis 100) |
| `chargeMaxLimit` | 2400 | Ladeleistungsgrenze | (W) → **2400 W** | Broker `DEV_MAX_W=2400` ✅ |
| `socLimit` | 0 | unklar (evtl. ungenutzt) | — | — |

→ **Idee für einen billigen L1-Check:** überwachen, dass `minSoc`/`socSet`/`chargeMaxLimit` konstant
und plausibel bleiben (Drift = Fehl-/Umkonfiguration des Geräts). Das Probe-Skript markiert Abweichungen
bereits (rote „BACKSTOP-DRIFT"-Zeile).
**Schreiben** dieser Werte ist von der Doku **nicht** gedeckt (nur Lesen) — nicht setzen.

---

## 3. Lokal vorhandene Felder, die die Cloud-Doku als „Lücke" führte

Der lokale Report hat **~55 Felder statt ~30** in der Doku. Mehrere von Fable als „bestätigte Lücke"
eingestufte Dinge sind lokal **doch da**:

| Feld | Live-Wert | Nutzen | Doku |
|---|---|---|---|
| **`ts` / `tsZone`** | 1784141549 → **2026-07-15 20:52**, 14 | **Geräteseitiger Zeitstempel** → Staleness/Frische lokal prüfbar | nicht dokumentiert |
| `dataReady` | 1 | Daten-Bereit-Flag (Achtung: war 1, obwohl `packData` leer!) | nicht dok. |
| `IOTState` | 2 | Verbindungs-/IoT-Zustand (Semantik unbekannt) | nicht dok. |
| `bindstate` | 0 | Cloud-Bindung (0 = ungebunden, passt zu cloud-frei) | nicht dok. |
| `faultLevel` / `is_error` | 0 / 0 | BMS-Alarm (empirisch: 2=Balancing, 3/is_error=1=Boot-Transient) | nicht dok. |
| `gridState` | 1 | Netz verbunden | nicht dok. |
| `heatState` | 0 | Zellheizung aus | nicht dok. |
| `packNum` | 4 | **4 Batterie-Packs** (aber `packData`-Array hatte nur 1 Eintrag) | teils |

---

## 4. Aufgelöste Fragen & offene Punkte

**Aufgelöst:**
- **A1 (Temperatur ×10):** über `hyperTmp=2981=25,0 °C` als Kelvin×10 **bewiesen** → `maxTemp`-Guard-Skalierung korrekt.
- **A2 (`socSet`-Skalierung):** `socSet=1000` = 100 % → unser Gerät skaliert **×10**, nicht das Prozent-Beispiel der Doku.
- **Zellspannung ×0,01 V:** Doku-wörtlich + konsistent.

**Offen / zu verifizieren:**
- ⚠️ **`packData` ist im Standby (`packState=0`) LEER** — trotz `dataReady=1` und `packNum=4`.
  Folge: **Zell-Guards (Spannung/Temperatur) sehen im Leerlauf keine Daten.** Vermutlich unkritisch
  (Über­lade-/Übertemp-Risiko besteht beim *aktiven* Laden/Entladen), aber **noch nicht bestätigt**,
  dass `packData` bei aktivem Pack zuverlässig füllt. → Probe laufen lassen bis zum nächsten
  Lade-/Entladefenster; das Skript markiert den ersten aktiven Zell-Datensatz (>>> A1 erfüllt).
- `IOTState`/`bindstate`/`socLimit`-Semantik: undokumentiert, nur beobachtbar.

---

## 5. Vollständige beobachtete Top-Level-Feldliste (2026-07-15, Referenz)

```
heatState, packInputPower, outputPackPower, outputHomePower, remainOutTime, packState,
electricLevel, gridInputPower, solarInputPower, solarPower1..4, pass, reverseState, socStatus,
hyperTmp, gridOffPower, dcStatus, pvStatus, acStatus, dataReady, gridState, BatVolt, socLimit,
faultLevel, acCouplingState, offGridState, dryNodeState, writeRsp, acMode, inputLimit, outputLimit,
socSet, minSoc, gridStandard, gridReverse, inverseMaxPower, lampSwitch, gridOffMode, IOTState,
Fanmode, Fanspeed, bindstate, factoryModeState, OTAState, oldMode, VoltWakeup, ts, tsZone,
smartMode, chargeMaxLimit, phaseSwitch, batCalTime, socCompSwitch, packNum, rssi, is_error
```
`packData[]` (nur bei aktivem Pack gefüllt): `maxVol, minVol, totalVol, maxTemp, minTemp, socLevel, sn, state, ...`

---

## 6. Von der Doku bestätigte echte Lücken (unsere Behelfe bleiben richtig)

- **Ladeakzeptanz / BMS-Taper:** kein Feld. Nur indirekt beobachtbar (`inputLimit` Soll vs.
  `outputPackPower`/`gridInputPower` Ist = brauchbarer Taper-Detektor). `inputLimit` fehlt sogar in
  der SolarFlow-Doku-Tabelle → unser Ladepfad ist vollständig reverse-engineert.
- **`faultLevel`/`is_error`-Enum:** nicht dokumentiert → konservativ + entprellt behandeln bleibt korrekt.
- Offizieller Heartbeat/Online-Status im *Cloud*-Sinn: keiner — aber lokal gibt es `ts` (s. §3).

---

## 7. Werkzeug

`zendure-report-probe.ps1` — nur-lesend, greift **nicht** in Regler/Broker/Gerät ein.
```powershell
# Endlos, 30 s Takt (bis Strg+C):
.\zendure-report-probe.ps1
# Über Nacht 60 s Takt:
.\zendure-report-probe.ps1 -IntervalSec 60
# Kurztest:
.\zendure-report-probe.ps1 -MaxSamples 3 -IntervalSec 3
```
Loggt alle relevanten Felder + Umrechnungen nach CSV; markiert (1) den ersten aktiven Zell-Datensatz
(A1), (2) Backstop-Drift, (3) `ts`-Skew zur Notebook-Uhr.

---

## 8. Nächste Schritte (Priorität)

1. **A1 endgültig schließen (klein):** Probe laufen lassen, bis der Pack aktiv wird, und
   `packData[].maxTemp`-Rohwert gegen die Kelvin×10-Konvention prüfen + bestätigen, dass die
   Zell-Guards im Betrieb Daten sehen.
2. **Backstop-Monitor (klein):** `minSoc`/`socSet`/`chargeMaxLimit`-Drift dauerhaft überwachen
   (notebook-seitig via Probe, oder später als L1-Check — Design-Entscheidung offen).
3. **`ts`-Frische beobachten (klein):** `ts_skew_s` über Zeit ansehen; nur wenn stabil, als
   *zusätzliche* (nicht ersetzende) Staleness-Quelle erwägen.
4. **L2-Leistungsbilanz-Beobachter (groß, separat):** `outputHomePower ≈ solarInputPower +
   packInputPower − outputPackPower` + `outputLimit`-Rücklesung als unabhängige Invariante.
   **Echter Umbau am sicherheitskritischen Regler/Broker → eigene Design-+Gate-Runde, nicht nebenbei.**
