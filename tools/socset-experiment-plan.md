# Experiment: `socSet = 99 %` — Test der 100-%-SoC-Standby-Lockup-Hypothese

**Erstellt:** 2026-07-16 · **Status:** geplant (startet NACH dem Baseline-Dauertest 07-16/17)
**Bezug:** `zendure-aktuator-nichtantwort.md` §7, Zendure-HA Issue #1505.

## Hypothese
Die Aktuator-Nichtantwort (`zout_w=0` trotz `setpoint>0`) wird durch einen **Geräte-Lockup nach Erreichen
100 % SoC** ausgelöst (extern belegt in #1505/#1489). **Kappen von `socSet` auf 99 % verhindert den Auslöser
strukturell.** Der Regler stoppt sein eigenes Laden schon bei 98 %, aber PV kann das Gerät ohne `socSet`-Deckel
auf 100 % treiben.

## Einzelvariable (sauberes A/B)
Geändert wird **nur** `socSet`: 100 % → 99 %. Alles andere identisch: Regler-FW 1.23.0, Broker 1.14.0,
gleicher Logger/gleiche Spalten, gleiche Umgebung.

## Sicherheitsanalyse (warum niedriges Risiko)
- `socSet` = **Lade-Decke**. 99 % < 100 % → **weniger** Laden, **kein** Overcharge-Pfad.
- Berührt **weder** den Entlade-Floor (`minSoc`=30 %) **noch** die Leistungsgrenzen (W).
- Vollständig reversibel; kein Flash nötig (App-Weg). ⇒ niedrigste Risikostufe.
- **Skalierung beachten:** unser Gerät liefert `socSet` ×10 → 99 % = **990**. Rücklesung Pflicht (Range-Check
  980–1000), damit kein Skalierungs-Irrtum (990 ≠ 99, aber auch ≠ 9,9 %).

## Ablauf
1. **Baseline zu Ende:** Dauertest `dauerlauf_20260716.csv` (socSet=100) bis 07-17 10:00, auswerten →
   Referenz-Episodenrate (Anteil aktiver Samples mit `zout=0`; 07-13 war **62 %**). Prüfen, ob der Akku in der
   Baseline-Nacht überhaupt 100 % erreichte (sonst fehlt die Referenz → Baseline wiederholen).
2. **socSet auf 99 % setzen** — **Nutzer-Aktion in der Zendure-App** (Claude ändert keine Geräteeinstellungen).
3. **Rücklesung:** `zendure-report-probe.ps1` bzw. direkter Report → `socSet` muss **990** zeigen. Range-Check.
4. **Experiment-Dauertest** über einen vollen **PV-Tag + Nacht** (gleiche Spalten, neuer Dateiname).
5. **A1-Probe parallel** (packState/electricLevel/acMode/tele_age während des Nacht-Entladens).

## Erfolgskriterien
- **Primär:** Anteil aktiver Samples (`setpoint>30 W`) mit `zout_w=0` fällt deutlich unter die Baseline (62 %).
- **Sekundär:** `electricLevel` erreicht max **99 %** (nicht 100 %); keine Nicht-Liefer-Episoden; `tele_age`
  bleibt klein; Nacht-`grid`-Bilanz näher an −25 W.

## Confounder / Vorbehalte
- Braucht einen **sonnigen Tag**, damit der Akku die Decke erreicht. Bedeckter Tag ⇒ evtl. nicht aussagekräftig.
- Erfasst die Baseline-Nacht zufällig **keine** Episoden (Akku < 100 %), ist die Referenz schwach → Baseline wiederholen.

## ⚠️ Trigger-Bedingung (Live-Erkenntnis 2026-07-17) — Experiment feuert nur bei Nachladen-von-unten

Live geprüft: Zendure hat **KEINEN direkten PV-Eingang** (`solarInputPower=0`) → Laden nur per Regler-Kommando
(grid). Der Regler ist bei SoC 97 % **`charge_blocked=True`** (Hysterese, ab 98 % gehalten, Freigabe erst ≤95 %).
Ergebnis: der Mittags-Überschuss wird **exportiert**, der Akku bleibt bei 97 % — **er lädt gerade NICHT hoch**.

**Wie der Akku am 07-13 100 % erreichte:** Regler lud mit **vollem `setpoint=−2400 W`** von 74 % hoch; nahe voll
**sprang die SoC-Schätzung 96→100 % in ~90 s** (Spannungs-Rekalibrierung am CV-Knie) und **übersprang** die
weiche 98 %-Regler-Stopp-Schwelle, bevor der Regler stoppen konnte → 100 % → Lockup.

**Daraus folgt (wichtig):**
1. **Das Experiment feuert nur, wenn der Akku erst < ~95 % fällt und dann unter PV-Überschuss bis zur Decke
   nachlädt.** Nur dann tritt der SoC-Sprung-auf-Decke auf. Bei mildem Sommer-Nachtverbrauch fällt der Akku aber
   nicht unter 95 % (07-16-Baseline blieb die ganze Nacht bei 98 %) → Gefahr, dass das Experiment **inaktiv** bleibt.
2. **Genau deshalb ist `socSet` (Geräte-Hard-Cap) der richtige Hebel** und nicht der Regler-Softstopp: der
   Softstopp (98 %) ist zu langsam für den SoC-Sprung; der Geräte-Cap bei 99 % stoppt die Ladeannahme hart →
   SoC snappt auf 99 statt 100 → kein Lockup.
3. **Um das Experiment schlüssig zu machen, braucht es ein Nachlade-Ereignis:** Akku gezielt < 95 % entladen
   (hohe Hauslast abends/über Nacht) → am Folgetag lädt PV bis zur 99 %-Decke. Sonst auf einen Tag warten, an dem
   der Akku ohnehin < 95 % war.

## Rollback
`socSet` in der App zurück auf 100 %.

## Bei Bestätigung → Code (mit Gates a–g)
Regler setzt `socSet=990` als Geräte-Backstop (in `assertDeviceBackstops()`), mit **Rücklesung**; zusätzlich
`smartMode:1` in den Kommandopfad. Beides aktuierungsnah → Vorzeichen-/Safe-State-Test, gestuftes Hochtasten.
