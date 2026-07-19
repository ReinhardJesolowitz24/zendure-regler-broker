# Zendure SolarFlow 2400 Pro — Aktuator-Nichtantwort im lokalen Betrieb

**Status:** ENTWURF / Beweis-Sammlung (wächst mit jedem Dauertest).
**Zweck:** technisch sauber dokumentieren, dass der Zendure im lokalen MQTT-Betrieb **kommandierte
Entladeleistung zeitweise nicht liefert** — als Grundlage, das nach weiteren Tests ggf. an Zendure
zu melden. *(Aktuell Deutsch; für eine Einreichung bei Zendure später ins Englische übersetzen.)*

---

## 1. Symptom (kurz)

Der Regler kommandiert einen **positiven Entlade-Sollwert** (`setpoint > 0`, via lokalem MQTT
`outputLimit`), aber der Zendure meldet über längere Zeit **`outputHomePower`/`zout_w = 0`** und liefert
keine Leistung. Das Haus bezieht in dieser Zeit weiter aus dem Netz, obwohl der Akku ausreichend geladen
ist und liefern soll. Der **Regler arbeitet korrekt** (Sollwert steht an, innerhalb aller Grenzen) — der
**Aktuator (Zendure) folgt nicht**.

## 2. Messtechnische Evidenz — Dauerlauf 2026-07-13/14 (regler-1.23.0)

Quelle: `dauerlauf_20260713.csv`, unabhängig ausgewertet (siehe `dauerlauf-review-20260713.md`).

- **62 % der aktiven Samples betroffen:** in **218 von 352 Zeilen** mit `setpoint > 30 W` war `zout_w = 0`.
- Auch bei kräftigem Sollwert: bei `setpoint > 100 W` waren **38 von 67 Zeilen** ohne Lieferung.
- **17 zusammenhängende Episoden**, längste ~8–10 min. Beispiele:
  - **23:08–23:15** — Sollwert bis **399 W**, dennoch `zout_w=0`, anhaltender Netzbezug **122–259 W**.
  - **05:26–05:35** und **01:40–01:48** — analog.
- **Korrelation mit Telemetrie-Alter:** in den schweren Episoden stieg `tele_age_s` (Alter der letzten
  Zendure-Telemetrie) auf bis zu **288 s** → der Zendure meldete auch nichts mehr, wirkte „schlafend".
- Kontext: SoC hoch (97–100 %), Nacht, niedrige Last. Bekannt dazu: im Standby ist `packData` im
  `/properties/report` **leer** (`packState=0`) — konsistent mit einem Ruhe-/Standby-Modus.

## 3. Auswirkung

Die Nacht-Regelbilanz lag bei **−78 W** statt Ziel **−25 W**; der Großteil der Abweichung geht auf
diese Nicht-Lieferungs-Episoden zurück (unnötiger Netzbezug). Sicherheitskritisch ist es **nicht**
(die Grenzen bleiben gewahrt) — es ist ein **Regelgüte-/Verfügbarkeits-Thema**.

## 4. Hypothesen (Ursache noch NICHT gesichert)

1. **Standby-/Schlaf-Modus:** Der Zendure geht bei niedriger Aktivität/hohem SoC in einen Ruhemodus und
   reagiert auf moderate `outputLimit`-Kommandos nur verzögert oder gar nicht, bis er „geweckt" wird.
2. **Weck-/Latenz-Schwelle:** Möglicherweise wird erst ab einer bestimmten Leistung / einem bestimmten
   Trigger aufgeweckt (399 W reichten in einer Episode nicht).
3. **Lokaler Kommandopfad:** MQTT-Kommando kommt an, wird aber im Standby verworfen/verzögert; die
   gleichzeitige Telemetrie-Stille (`tele_age` ↑) deutet auf geräteinterne Ruhephase, nicht auf
   Netzwerkverlust (Regler↔Broker-Pfad lief weiter, `mqtt_reconn` blieb 0).

**Noch auszuschließen (unsererseits):** dass unser Soft-Retreat/Staleness-Handling in diesen Fenstern den
Sollwert selbst zurücknimmt. Erste Sichtung: Die Episoden liegen NICHT in Retreat-Fenstern (Retreat feuerte
nur 19:27–19:31). Zu bestätigen mit Sollwert-Verlauf je Episode.

## 5. Was wir noch brauchen, bevor wir es an Zendure melden

- [ ] **Reproduktion über mehrere Nächte** (Dauertest 2026-07-16/17 läuft, gleiche Spalten).
- [ ] **Sollwert-Verlauf je Episode** gegen `zout_w` und `tele_age_s` plotten (unser Kommando eindeutig als anstehend belegen).
- [ ] **Weck-Schwelle eingrenzen:** ab welcher kommandierten Leistung liefert er zuverlässig? Gibt es eine Mindest-Wachhaltung?
- [ ] **Gegenprobe Zendure-Telemetrie**: `packState`, `hyperTmp`, `IOTState` während der Episoden mitschneiden (Probe `zendure-report-probe.ps1`), um „Standby" zu bestätigen.
- [ ] Prüfen, ob ein periodisches „Keep-Alive"/Wieder-Senden des Sollwerts das Verhalten ändert (**nur als Diagnose**, nicht als Dauerlösung ohne Verständnis).

## 6. Offene Fragen an Zendure (Entwurf)

1. Hat die 2400 Pro im lokalen Betrieb einen **Standby-/Schlafmodus**, der die Reaktion auf
   `outputLimit`-Kommandos verzögert oder unterdrückt? Ab welchen Bedingungen?
2. Gibt es einen **dokumentierten Weck-Mechanismus** oder eine Mindest-Wiederholrate, mit der ein
   lokaler Regler das Gerät aktiv/reaktionsbereit hält?
3. Wie ist die erwartete **Latenz** von `outputLimit`-Kommando bis `outputHomePower`-Ist im Normal- und
   im Ruhezustand?
4. Ist das Verhalten SoC-/temperatur-/leistungsabhängig?

---

## 7. Abgleich mit offiziellen Zendure-Quellen (github.com/Zendure, Stand 2026-07-16)

**Unser Symptom ist KEIN Einzelfall — es ist ein offenes, mehrfach gemeldetes Zendure-Issue.**

- **`Zendure-HA` Issue #1505** (angelegt **2026-07-12**, offen, „bug"): *„[Bug] SF2400 Pro stops discharging
  after reaching 100% SoC — output stays at 0 W despite valid output limit."* Auslöser laut Issue: SoC-Sprung
  98→100 % bei `socSet=100`; danach Standby, Gerät ignoriert Entlade-Kommandos. **Eskalation bis 2400 W half nicht.**
  Kein Zendure-/Maintainer-Statement. Flankierend: **#1503** (2400 Pro, 100 %), **#1312** (2400 AC+, Standby trotz
  Überschuss), **#1489** (Hyper 2000, 100-%-Lockup, Workaround SoC-Limit 99 %).
  URLs: https://github.com/Zendure/Zendure-HA/issues/1505 (#1503/#1312/#1489 analog).

**Konsequenz für unsere Hypothesen:**
- Hypothese 2 (Weck-**Leistungs**schwelle) **geschwächt** — in #1505 half selbst 2400 W nicht.
- Hypothese 1 (**Standby-Lockup nach SoC-voll**) **gestärkt** — Auslöser ist der 100-%-Zustand, keine Amplitude.
- Ehrlicher Unterschied: in #1505 lief die Telemetrie weiter; bei uns stieg `tele_age` bis 288 s (tiefere Ruhe).
  Möglicherweise dieselbe Ursache, tieferer Zustand — **nicht** durch die Quelle gedeckt.

**Offizielle Kommando-Semantik (Referenz: `Zendure-HA/.../device.py`, `en_properties.md`, `zenSDK`):**
- Der offizielle Client schreibt **nie nacktes `outputLimit`**, sondern ein **Kommando-Tripel**:
  `{smartMode:1, acMode:2, outputLimit:x, inputLimit:0}` (Entladen; Laden analog `acMode:1, inputLimit:|x|`).
  → In #1505 wurde „bare property write ohne smartMode/acMode" als Fehlerquelle Nr. 1 benannt.
- **`smartMode=1` = „Parameter NICHT ins Flash schreiben", empfohlen für häufige Änderungen** → ein zyklischer
  Regler MUSS damit arbeiten (sonst Flash-Verschleiß). **Unser Regler setzt `smartMode` bisher NICHT.**
- **Kickstart:** offizieller Client überhöht das Kommando, wenn `homeOutput==0` (POWER_START=50 W → bis 2×);
  startet Geräte **nie unter 50 W**; erneuert Kommandos alle ~2–4 s (de-facto Keep-Alive, nicht spezifiziert).
- **`socSet` ist offiziell RW 70–100 %** (`en_properties.md`) → der frühere Vorbehalt „Schreiben nicht gedeckt"
  ist aufgehoben. Workaround aus #1505/#1489: **`socSet ≤ 99 %` halten** verhindert den Lockup-Auslöser strukturell.

**Unser Code-Stand (regler.ino):** Wir senden bereits `acMode` (`Output/Input mode`) + `outputLimit`/`inputLimit`
über die Gate-Topics (besser als der #1505-Fall), **aber ohne `smartMode`**, und wir kappen `socSet` nicht →
PV kann das Gerät auf 100 % treiben (= Lockup-Auslöser), obwohl der Regler sein eigenes Laden bei 98 % stoppt.

**Weitere offizielle Bestätigungen (Nebenprodukt):**
- Lokale API ist mit **`zenSDK` offiziell**, **2400 Pro namentlich „supported"** (mDNS + HTTP + konfigurierbarer
  lokaler MQTT) → unser lokaler Pfad ist legitimiert.
- `packState 0=Standby / 1=Charging / 2=Discharging` offiziell (openapi.yaml) → unsere Standby-Deutung bestätigt.
- **Temperatur-Skalierung offiziell:** `maxTemp = (roh − 2731)/10 °C` = Kelvin×10 → **A1 dokumentarisch bestätigt**.
- **Risiko:** unter **EN 18031** kann neuere FW den lokalen HTTP-Zugang **standardmäßig deaktivieren**
  (Re-Enable via HEMS in der App) → bekannter Ausfallpfad nach FW-Updates. Lokale API-Empfangslänge 512 Byte.

**Abgeleitete Fix-Kandidaten (alle NUR mit Sicherheits-Gates, nach Verständnis + gestuftem Test):**
1. **`socSet ≤ 99 %` als Geräte-Backstop setzen** (unser Gerät ×10 → **990**; mit Rücklesung/Range-Check!) —
   verhindert den 100-%-Lockup-Auslöser strukturell. *Führender Kandidat.*
2. **`smartMode:1`** in den Kommandopfad aufnehmen (Flash-Schonung; Verhalten wie offizieller Client).
3. **Wake-Puls als überwachte Recovery:** bei erkannter Episode `outputLimit=0` → ~20 s → Sollwert (#1505-Workaround).
4. **Mindest-Entlade-Sollwert ≥ 50 W** (POWER_START-Analogie).

---

## 8. Baseline-Dauertest 07-16/17 (socSet=100 %) — ZWEI Mechanismen unterscheiden

Wichtige Verfeinerung: Die „Aktuator-Nichtantwort" ist **nicht ein** Phänomen, sondern (mindestens) **zwei**:

**(A) Minimum-Output-Floor — gutartig (dieser Lauf):** 67 % der aktiven Entlade-Samples (81/121) `zout=0`,
ABER bei **kleinen** Sollwerten (77–143 W) und **frischer** Telemetrie (`tele_age` 0–1 s → Gerät WACH). Die
Zendure liefert unterhalb ihrer Mindestabgabe (~150 W?) schlicht 0 statt eines Kleinbetrags (deckt sich mit
Community „nie < 50 W", „Sättigung 150 W"). **Impact gering:** `grid` blieb im Band (Nacht 78 % in [−75..25],
Median −42 W) — die nicht gelieferten Beträge sind klein. **Kein Lockup.**

**(B) 100-%-SoC-Standby-Lockup — schwer (07-13, hier NICHT reproduziert):** große Sollwerte bis 399 W ignoriert,
`tele_age` bis 288 s (Gerät SCHLÄFT), nach Erreichen 100 % SoC. Das ist der #1505-Fall.

**Schlüssel:** Der 07-16-Baseline erreichte **nie 100 % SoC** (durchgehend 98 %) → der schwere Lockup (B) wurde
**nicht ausgelöst**. Grund: PV lud diesmal nicht bis 100 %; am 07-13 schob PV den Akku auf 100 % (Solar-MPPT lädt
**unabhängig** vom Grid-Sollwert des Reglers). ⇒ **`socSet=99 %` zielt genau auf (B)**: es kappt die PV-Ladung bei
99 % und verhindert den 100-%-Auslöser. Gegen (A) hilft es nicht — (A) ist ohnehin gutartig.

**Konsequenz fürs Experiment:** Es braucht einen **starken PV-Tag** (sonst erreicht der Akku die 99-%-Decke nicht,
und (B) tritt weder mit noch ohne Deckel auf). Referenz-Baseline für (B) = der **07-13-Lauf** (100 % erreicht, schwere Episoden).

**Plattform-Stabilität 07-16/17 (Nebenbefund):** 20 h, **0 Reboots, 0 Reconnects, 0 `shelly_fail`** (bestätigt:
die 07-13-`shelly_fail`-Flut war umgebungsbedingt, nicht systematisch), Enforcer flat, Regelgüte gut (Nacht 78 %
in Band). Ladepfad (`act_mode=2`) weiterhin ungetestet (SoC nie < 95 %). Der 3.-MQTT-Client trat nur im
09:08-Reconnect-Cluster auf → vermutlich **Reconnect-Overlap** (Broker sieht alte + neue Zendure-Session kurz
gleichzeitig), **kein Fremd-Client**.

---

## 9. ROOT CAUSE (Live-Test 07-17): Regler SPERRT Entladen bei stale SoC — Standby-Deadlock

**Live-Test:** Nutzer schaltet ~3,4 kW Hauslast bei SoC 97 %. Ergebnis aus `dauerlauf_20260717.csv`:
**13,5 Minuten** durchgehend `setpoint=0`, `act_mode=0`, während `grid` = −3000…−4000 W (voller Netzbezug),
`tele_age` 0–1 s, `tele_stale=False` (Kontakt lebt), `zout=0`. Dann **11:26:57 Sprung 0 → 2400 W in EINEM
Schritt**; Gerät wacht auf (`packState=2`, `outputHomePower=2398 W`), grid erholt sich auf ~−1000 W (Last > 2400 W).

**Ursache (bestätigt): `ctrlSocOld`.** Der Regler sperrt Entladen (`hiLimit=0`), wenn der **SoC-WERT älter als
10 min** ist (`SOC_MAX_AGE_MS`, regler.ino:80/326/331) — Fail-Safe „Echo lebt, aber kein echter SoC → misstrauen".
Im **Standby publiziert die Zendure keinen SoC** (nur der `outputLimit`-Echo hält `tele_age` klein) → SoC-Wert
veraltet >10 min → Entlade-Sperre. Regler kommandiert 0 → Gerät bleibt im Standby → weiter kein SoC → **DEADLOCK**,
bis das Gerät zufällig einen frischen SoC sendet (hier ~11:26:48, `soc_age→1`, `soc_old→false`) → Sperre fällt,
Integrator springt (Anti-Windup) sofort auf 2400 → Gerät entlädt.

**Impact:** bis >10 min **voller Netzbezug bei vollem Akku**, wenn eine Last im Standby auftritt. HOCH — Kostentreiber
und konterkariert die Nulleinspeisung genau im Lastfall.

**Abgrenzung der Mechanismen (jetzt drei + Messfehler):**
- **(1) Regler-Entlade-Sperre bei stale SoC (dieser Test):** `setpoint=0`, Regler-seitig, Standby-Deadlock. NEU, HOCH.
- **(A) Minimum-Output-Floor:** kleine Sollwerte <~150 W, Gerät wach, gutartig.
- **(B) 100-%-SoC-Lockup (#1505):** Gerät schläft nach 100 % SoC trotz großem Sollwert.
- **Messfehler `zout_w`:** meldet **0 trotz real 2398 W** → alle „`zout=0`"-Kennzahlen (62/67 %) sind teils Artefakt;
  verlässlich ist nur **grid-Reaktion** (bewegt sich grid bei setpoint>0 Richtung Ziel?).

**Fix-Kandidaten (Diagnose zuerst; Aktuierungsnahes nur mit Gates):**
1. *(Diagnose)* `soc_old`/`soc_age` + `zout_w`-vs-`outputHomePower`-Diskrepanz in `/status`+CSV mitloggen.
2. **SoC aktiv auffrischen statt passiv sperren:** wenn Entladen gewünscht (großer grid-Fehler) UND SoC stale →
   HTTP `/properties/report` pollen (`electricLevel` dort IMMER frisch) oder Wake/Query senden → bricht den Deadlock.
3. **Fail-Safe verfeinern:** letzten bekannten SoC mit Marge nutzen — war er zuletzt weit über dem Floor (z. B. >40 %),
   Entladen für eine Übergangszeit ERLAUBEN statt hart sperren (Akku fällt in 10–15 min nicht an den 30 %-Floor).
   Hart sperren nur, wenn letzter SoC nahe Floor. (FuSi-Abwägung: Über-Entlade-Schutz vs. Netzbezug-Vermeidung.)
4. **`zout_w`-Telemetrie fixen** (Quelle klären: warum 0 trotz 2398 W?).

---

## 10. Geräte-Firmware-FREEZE (Live 07-17 11:44:32) — vierter, schwerster Fall

Nach ~17 min Entladen unter Last (ab 11:27, 2400 W) fror die Zendure um **11:44:32** komplett ein:
- **HTTP-Report EINGEFROREN:** `ts` steht über mehrere Reads (11:52:28/35/42) konstant bei 11:44:32 (roh 1784281472
  unverändert). `packState=0`, `outputHomePower=0`, `outputLimit=0`.
- **Ignoriert das Entladekommando:** Regler kommandierte 1170 W; Shelly maß unabhängig −2840 W Bezug → real 0 Abgabe.
- **Zombie-Verbindung:** TCP „verbunden" (`clients=2`), aber kein Echo, `tele_age` klettert 253→333 s+.
- `soc_old=False` → **NICHT** der 1.24.0-Deadlock. SoC 94 %, faultLevel/is_error 0 (Report aber eh eingefroren).

**Einordnung:** reiner **Geräte-Firmware-Hänger**, deckt sich mit #1505/Foren („stuck in standby, needs restart").
**Nur Geräte-Neustart** holt es zurück; MQTT-Wake wirkungslos (Firmware verarbeitet nichts mehr). Regler/1.24.0
kann das prinzipiell nicht beheben — es ist geräteseitig.

**Mechanismen jetzt vollständig (VIER + Messfehler):**
1. **Regler-Entlade-Sperre bei stale SoC (Deadlock)** — Regler-seitig → **gefixt in 1.24.0**.
2. **Geräte-Firmware-Freeze** (dieser Fall) — Gerät, nur Neustart, Report `ts` friert ein.
- **(A)** Minimum-Output-Floor (<~150 W) — gutartig.
- **(B)** 100-%-SoC-Standby-Lockup (#1505) — Gerät.
- **Messfehler:** `zout_w` meldet 0 trotz realer Abgabe.

**NEUER Detektions-Kandidat (L3, hoher Wert):** „Gerät eingefroren" erkennen = **`ts` läuft nicht mehr** ODER
`tele_age` klettert trotz `clients=2` über eine Schwelle → **alarmieren** (Push/Wächter). Grund: das System kann
sich hier **NICHT selbst erholen** (Neustart ist manuell). Reine Diagnose, kein Aktuierungseingriff.

---

## 11. V2.0.1-Update + Freeze-Persistenz + MQTT-Trigger-Verdacht (Live 07-17 ~12:24)

**Kontext:** Am 07-17 fror die Zendure ab 11:44 wiederholt ein (§10), auch nach mehreren Power-Cycles (jeweils
Re-Freeze binnen ~2 min). Firmware war **V2.0.0** (dokumentierte Standby-Bugs, Forum d/27682/27825). Update auf
**V2.0.1** durchgeführt (mit MQTT aus, SoC 95 %) — App meldete „complete".

**Ergebnis: V2.0.1 behebt den Freeze NICHT.** ~2 min nach dem Update-Reboot fror die lokale Melde-/Steuer-Seite
wieder ein: `ts` steht seit 12:24:57 fest (>4 min), `tele_age` (MQTT) klettert — zwei unabhängige Signale, gleiches
Muster wie V2.0.0.

**Aber Update hat NICHTS verschlimmert:** lokale API erreichbar (kein EN-18031-Lockout), `socSet=990` (99 %×10)
überlebt, Skalierung intakt (`hyperTmp`→Kelvin×10, `BatVolt`~50 V), am Broker (`clients=2`). Feldzahl 55→58.

**Konkreter Schaden des Freeze:** Gerät hängt auf letztem Kommando fest — bei 12:24:57 war „Laden" aktiv → **lädt
stur 655 W weiter, aktuell AUS DEM NETZ** (828 W Bezug, kein Überschuss mehr), weil der **Kommando-Eingang**
eingefroren ist (nicht nur die Meldung). Gerätekern läuft (führt letztes Kommando aus), lokale Schnittstelle tot.

**Trigger-Verdacht (wichtig): UNSER MQTT-Schreib-Stream.** Die Freezes fielen jeweils mit aktivem MQTT/Regler
zusammen; das Update lief stabil durch, **während MQTT aus war**; der Re-Freeze kam **im Moment der MQTT-Reaktivierung**
(12:24:57). Kandidaten: 2-s-Schreibrate, Einzel-Property-Topics **ohne `smartMode`** (#1505-Muster), 30-s-Backstop-
Writes (`gridReverse`/`minSoc`). System lief zuvor TAGE stabil → evtl. erst durch heutige Heavy-Load-/socSet-Änderung
oder V2.0.x-Fragilität getriggert.

**Isolationstest läuft (07-17 ~12:30, 1 h):** Nutzer hat **MQTT aus + HEMS an** → Gerät unter EIGENER Steuerung,
ohne unseren Stream, 1 h Einschwingen (ggf. mit Geräte-Neustarts). Danach MQTT wieder an.
- Friert es auch unter reinem HEMS → **reiner Firmware-Bug** (Zendure melden).
- Bleibt stabil → **unser Stream triggert** → Fix: Kommando-Tripel `{smartMode:1, acMode, outputLimit, inputLimit}`,
  Schreibrate senken, Backstop-Writes drosseln.

---

## 12. Auflösung / Stand 07-17 Nachmittag: SLEEP bestätigt, `ts` untauglich, Rate-Limit läuft

**Live-Lasttest 13:53** (broker-1.15.0 Rate-Limit aktiv, MQTT an): Zendure **ENTLÄDT auf Kommando**,
`outputHomePower` folgt dem Sollwert (316→735 W ≈ setpoint), grid erholt sich, `tele_age` 0–3 s.
→ **Es war SLEEP, kein harter Freeze** — das MQTT-Kommando weckt das Gerät. (Nutzer-These bestätigt.)

**KERNKORREKTUR: `ts` (HTTP-Report) ist als Liveness-/Freeze-Indikator UNTAUGLICH.** `ts` stand bei 13:46:01
fest, **während im selben Report `outputHomePower` 316→735 hochlief.** `ts` ist also kein Report-Zeitstempel
(vermutlich letzter Cloud-/Config-Sync). **Verlässlich sind nur: MQTT `tele_age`/`tele_stale` (Regler) +
grid-Reaktion (Shelly) + geänderte Report-Datenfelder.** Die frühere Freeze-Diagnose via `ts` war teils irreführend.
→ Freeze-/Sleep-Detektor künftig über **tele_age + grid**, NICHT `ts`.

**Echte Fehler gab es dennoch:** die Vormittags-Nicht-Antwort (11:49 unter Last, `tele_age` KLETTERND,
Shelly-bestätigt keine Abgabe) war real; ebenso der soc_old-Deadlock (gefixt 1.24.0). Das Gerät kann unter
Bedingungen also doch hängen. **V2.0.0 UND V2.0.1 sind buggy** (Forum: V2.0.1-User laufen wenige Stunden nach
Release Sturm) → Zendure-Firmware-Problem, unabhängig von uns (HEMS-Baseline 13:35–13:43 stand ohne unser MQTT).

**Broker-Rate-Limit (broker-1.15.0) läuft im Betrieb einwandfrei** — Gerät reagiert unter gedrosseltem Stream sauber.

**Offen:** (a) `zout_w` weiter buggy (Regler meldet 565, real 316–735); (b) `socSet` wurde zeitweise von HEMS auf
1000 zurückgesetzt → Workaround socSet≤99 % muss HEMS-fest bleiben (aktuell wieder 990); (c) Freeze-Detektor
(tele_age+grid statt ts) als L3-Kandidat; (d) smartMode-Topic verifizieren, falls Rate-Limit nicht reicht.

---

## 13. Wake-Latenz aus Standby (Live-Capture 07-17 18:34–18:39, 5 min @ 3 s-Takt)

Vollmitschnitt mit mehrfachem Last-An/Aus (bis 8 kW Bezug). Stand: regler-1.24.0 / broker-1.15.0 (Rate-Limit) /
socSet 99 % / Zendure V2.0.1.

**Bestätigt GUT:** kein Freeze (`tele_age` max **6 s**), kein Deadlock (`soc_old`/`soc_stale_ovr` **nie**),
Entlade-Tracking punktgenau (`outputHomePower` 2397–2403 W bei Sollwert 2400), Soft-Retreat feuert bei Last-Wegfall
(max. Einspeise-Spike **1569 W** — begrenzt; ohne Retreat wären es +2400 W).

**BEFUND — Geräte-WAKE-LATENZ ~13–25 s:** Ist das Gerät im Standby (`packState=0`) und eine Last erscheint,
kommandiert der Regler **sofort** Entladen (Sollwert in ~10 s auf 2400), aber das Gerät braucht **~15–20 s** bis
`packState 0→2` und Leistung fließt. Drei saubere Messungen: **22 s / 13 s / 19 s**. In diesen Fenstern läuft die
**volle Last (bis 8 kW) übers Netz** (= die 15 „Nicht-Antwort"-Samples der Auswertung).
- **Keine Fehlfunktion:** nicht der Regler (kommandiert instant), nicht der Deadlock (`soc_old=False`), nicht ein
  Freeze (sie wacht ja auf). Es ist die **aggressive Standby-/Aufwach-Charakteristik des Geräts.**
- **Impact:** stetige Last unkritisch (bleibt wach, trackt perfekt); **intermittierende** große Lasten →
  ~15–20 s Netzbezug je Einschaltung.
- **Fix-Kandidat:** Anti-Standby / **Mindest-Abgabe** („Sättigung", Community) zum Wachhalten → keine Wake-Latenz.
  Trade-off: dauernde Kleinst-Entladung (Wirkungsgrad/Zyklen) vs. ~15–20 s Netzbezug pro Lastsprung.
  Prioritäten-Entscheidung, kein Muss.

---

## 14. Nacht-Validierung 07-17/18 (beobachtung_20260717_nacht.csv, 1858 Samples @ 30 s, 15,5 h)

Gesamt-Paket unter echter Lade-/Entladelast über eine volle Nacht: **regler-1.24.0 + broker-1.15.0 (Rate-Limit) +
socSet 99 % + Zendure V2.0.1**. Zeitraum 18:25 → 09:59.

| Kriterium | Ergebnis |
|---|---|
| **Stabilität** | makellos — **0 Reboots, 0 Reconnects** über 15,5 h, Enforcer flat |
| **Freeze/Deadlock** | **keiner** — `soc_old`/`soc_stale_ovr` **0/0**; nur 7/1858 kurze `tele_stale` (≤30 s, self-corrected) |
| **socSet** | **990 (99 %) durchgehend gehalten** — HEMS hat nicht zurückgesetzt |
| **SoC** | **91–98 %** — nie 100 % (Lockup vermieden), nie am 30 %-Floor; sicher zykliert |
| **Aktivität** | 757× Laden, 633× Entladen, 468× idle — Entladepfad intensiv geprüft |
| **Regelgüte 00–06** (n=716) | **Median −28 W** (Ziel −25), **99 % im Band [−75..25]**, p5 −52 / p95 0 |
| **Entlade-Tracking** | 241 liefernde Samples, mittl. `|setpoint − outputHomePower|` = **14 W** |
| **Wake-Latenz** | 9/633 Entlade-Samples (~1,4 %) — bekanntes Sleep→Wake-Verhalten, vernachlässigbar |

**Urteil:** Das Gesamt-Paket ist über eine volle Nacht unter echter Last **bestanden** — kein Freeze, kein Deadlock,
99 % Zielgenauigkeit, punktgenaues Tracking, socSet-Workaround HEMS-fest. Was UNSER Code beeinflusst, läuft
rock-solid. **Verbleibende (optionale) Baustelle:** geräteseitige Wake-Latenz ~15–20 s (Anti-Standby-Kandidat §13).
Randbedingung: `socSet=99 %` muss HEMS-fest bleiben (100 %-Lockup-Schutz, #1505).

---

## 15. Unabhängiges Fable-5-Review 07-18 (Ergebnisse + V2.0.1-Netzlage) — offene Risiken & Priorisierung

Unabhängige Prüfung nach V2.0.1 + regler-1.24.0 + broker-1.15.0. Kernbotschaft: Debugging-Qualität hoch, **Fixes
richtig gerichtet, aber NICHT vollständig validiert**; die „User-Sturm"-Einschätzung zu V2.0.1 war überzeichnet.

**Kritik an unseren Fixes:**
- **Deadlock-Fix (1.24.0) im Anforderungsfall UNGETESTET:** in der Nacht `soc_old`/`soc_stale_ovr` = 0/0 → Override nie
  gefeuert. Zudem **SoC-Band <50 % ungetestet** → Deadlock-Restrisiko dort (Winter). Besserer Fix (aktiver HTTP-Poll
  von `electricLevel` bei stale SoC) liegt ungenutzt; `electricLevel`-Frische im **Tief-Standby** unverifiziert.
- **Rate-Limit (1.15.0) evtl. am falschen Problem:** dokumentiertes Schreibpfad-Risiko ist **fehlendes `smartMode:1`**
  (Flash-Verschleiß im Gerät), nicht die Rate. → smartMode hochpriorisieren.
- **Nacht-Validierung deckt Fehlerräume nicht:** 99 %-Decke nie erreicht (**socSet-Workaround unbewiesen**), Override nie
  gefeuert, kein Tief-Standby+Lastsprung, HEMS-Nicht-Reset unverstanden (≠ „HEMS-fest").

**V2.0.1-Netzlage (VORSICHT — Frühbild ~2 Tage, dünn; Foren-Meinungen mit Vorbehalt):**
- Fixt den 100-%-/SOCFULL-Lockup **nicht** ([d/29969](https://forum.zendure.com/d/29969); #1505/#1503 offen; kein Zendure-Statement).
- Ein belegter neuer 2.0.1-Bug: [d/29977](https://forum.zendure.com/d/29977) — **HEMS-Nulleinspeisung („Einspeisung verboten") kaputt**,
  manuelle/HA-Steuerung ok. Kein V2.0.2/Changelog gefunden; **Downgrade praktisch nicht verfügbar**; lokaler Pfad intakt.
- Wake-Latenz + **Relais-Verschleiß** (durch wiederholtes `outputLimit=0`) unabhängig bestätigt ([d/29968](https://forum.zendure.com/d/29968)); Community-Gegenmittel = Mindest-Output.
- Neu (GitHub): [PR#1506](https://github.com/Zendure/Zendure-HA/pull/1506) `inverseMaxPower` fluktuiert (800 statt 2400) → Gerät verwirft Kommandos darüber (stille Nicht-Antwort); [#1514](https://github.com/Zendure/Zendure-HA/issues/1514) stille Readback-Freezes im Lokalmodus.

**Nutzer-Einordnung (07-18):** Foren-Meinungen mit Vorsicht behandeln. Zendure-SW hat erfahrungsgemäß (schon V2.0.0)
ein **starkes Einschwing-/Neustart-Verhalten** → die gestrige Instabilität war evtl. teils Einschwingen nach Update +
unseren Eingriffen (MQTT/HEMS-Toggles, Power-Cycles). **Diese Nacht-Validierung = BASELINE; in ~1 Woche (≈07-25) erneut
abgleichen** (settled vs. jetzt).

**Neue offene Risiken:** (1) **HEMS-Warm-Standby-Failover evtl. gebrochen** (d/29977 → würde einspeisen statt nullregeln;
Failover-Pfad + `gridReverse=Disallow`-Backstop testen); (2) Override-Pfad ungetestet; (3) `inverseMaxPower`-Fluktuation
(mitloggen); (4) `electricLevel`-Frische Tief-Standby; (5) Relais-Zyklen durch Retreat-auf-0; (6) SoC<50 %/Ladepfad-niedrig.

**Angepasste Priorität (WENN wir handeln, kein Sofort-Zwang):** 1. **Freeze-Detektor** (tele_age+grid, L3-Alarm, verstärkt
durch #1514); 2. **smartMode:1** (Flash + offizielle Form); 3. Tests: socSet-Decke (starker PV-Tag), Deadlock-Override,
HEMS-Failover; 4. `inverseMaxPower` mitloggen; 5. Mindest-Output erwägen (Wake-Latenz + Relais); 6. `zout_w` klären/streichen.

**SOFORT-HANDLUNGSBEDARF: KEINER.** System stabil (Nacht-Validierung), `socSet=99 %` schützt den bekannten Lockup.
Alles Weitere = Tests/Diagnose/Slow-Burn. Zendure einschwingen lassen, in 1 Woche abgleichen.

---

## 16. Fable-Web/GitHub-Recherche 07-18 — Verbesserungen + Umsetzungsplan

**#1505 wurde HEUTE (07-18) INTEGRATIONSSEITIG geschlossen** (PR #1506/#1507), **kein Firmware-Fix, kein V2.0.2**.

**Sofort verifiziert (read-only, kein Code nötig):**
- ✅ **gridReverse-Backstop korrekt:** Gerät meldet `gridReverse=2` (FORBIDDEN) auf unser „Disallow backflow" →
  PR-#1512-Dreiwert-Stolperstein (0/1/2) trifft uns nicht.
- ✅ **3.-MQTT-Client = Reconnect-Overlap** ([#1519](https://github.com/Zendure/Zendure-HA/issues/1519): Zendure nutzt wechselnde Client-IDs +
  „session taken over") → §8-Deutung bestätigt, kein Fremd-Client, kein Alarm nötig.
- ✅ **„ZEN STALE"-Semantik** = „Telemetrie tot", nicht „Gerät tot" ([#1514](https://github.com/Zendure/Zendure-HA/issues/1514)) — Label/Zustand passt.

**Umsetzungsplan (aktuierungsnah → NACH dem Nacht-Dauertest, mit Gates a–g):**
1. **smartMode-Tripel (REGLER/BROKER) — #1, offizieller Fix ([PR #1507](https://github.com/Zendure/Zendure-HA/pull/1507)).** Mechanik geklärt:
   offizieller Client schreibt EINEN kombinierten Befehl `{properties:{smartMode:1, acMode:2, outputLimit:X,
   inputLimit:0}}` (device.py:781), **NICHT** Einzel-Property-Topics wie wir. → **Design-Entscheidung:**
   (a) smartMode-Einzel-Topic ergänzen (Topic verifizieren) ODER (b) Broker-Geräteschreiben auf kombiniertes
   `properties/write`-JSON umstellen (Gate/Enforcer anpassen). Beleg heute: Gerät war `smartMode=1` → responsiv;
   der Bug tritt auf, wenn es aus dem Smart Mode fällt (PR #1507: bare writes >90 min ignoriert). Der bestehende
   broker-1.15.0-smartMode-Hook (deaktiviert) ist Ansatz (a) — Topic muss verifiziert werden.
2. **inverseMaxPower-Clamp (REGLER):** Sollwert auf gemeldetes `inverseMaxPower` clampen (nur nach unten =
   sicherheitsneutral); Telemetrie-Quelle (MQTT-Feld/HTTP) identifizieren. PR #1506: 46 Kommandos/45 min verworfen
   bei `inverseMaxPower=800`. (Aktuell 2400 = ok; Fluktuation intermittierend.)
3. **acMode-Cooldown ~30 s (REGLER): VERWORFEN (07-19).** In 1.25.0 vorbereitet, dann verworfen — **redundant**:
   die bestehende Reversal-Slew (`SLEW_REVERSAL_W=120 W/Takt` im ±400-W-Band → ~14 s Nulldurchgang) schont das
   Relais bereits *sanft*; kein Bedarfsnachweis; der bewusst non-jagende PI ist durch Dauertests abgesichert. KISS.
   Regler bleibt 1.24.0.
4. **Schreibtakt ≥14 s testen**, falls Freeze wiederkehrt (unser Keep-Alive = 8 s; iobroker: <14 s friert Sensoren ein).

**KEIN Sofort-Flash** (Nacht-Dauertest läuft bis 07-19 10:00). Umsetzung + Flash mit Gate-Disziplin danach; smartMode zuerst.

---

## Änderungshistorie
- **2026-07-16 (a):** Erstfassung auf Basis Dauerlauf 07-13/14 (Fable-Review). Beweis-Sammlung offen.
- **2026-07-18 (d):** §14 Nacht-Validierung 07-17/18 — Gesamt-Paket über 15,5 h bestanden (0 Reboots, kein
  Freeze/Deadlock, socSet 99 % gehalten, Regelgüte 99 % im Band, Tracking 14 W; Wake-Latenz als einzige Rest-Schwäche).
- **2026-07-17 (c):** Live-Lasttest: §9 Deadlock (gefixt 1.24.0), §10 Firmware-Freeze, `zout_w`-Bug, §11 V2.0.1 +
  MQTT-Trigger-Verdacht, §12 Sleep bestätigt + `ts` untauglich + Rate-Limit läuft, §13 Wake-Latenz ~15–20 s.
- **2026-07-17 (c):** Live-Lasttest: Root Cause §9 (Regler-Entlade-Sperre-Deadlock, gefixt regler-1.24.0),
  §10 (Geräte-Firmware-Freeze 11:44:32, nur Neustart), `zout_w`-Messfehler, Freeze-Detektion als L3-Kandidat.
- **2026-07-16 (b):** §7 ergänzt — Abgleich mit offiziellen Zendure-Repos (`zenSDK`, `Zendure-HA`): Symptom als
  offenes Issue #1505 extern reproduziert (100-%-Standby-Lockup), offizielle Kommando-Semantik (Tripel + smartMode +
  Kickstart), `socSet≤99 %`-Workaround, A1-Temp-Skalierung dokumentarisch bestätigt, EN-18031-Risiko.
