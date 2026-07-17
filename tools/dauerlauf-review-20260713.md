# Unabhängiges Review — Dauerlauf 2026-07-13/14 (regler-1.23.0 / broker-1.14.0)

**Erstellt:** 2026-07-16 · **Reviewer:** Fable-5-Subagent (unabhängig, Auftrag: Primär-Analyse herausfordern)
**Datenquelle:** `dauerlauf_20260713.csv` (3537 Samples, 07-13 13:16 → 07-14 06:59, ~17 s Takt)
**Status der Vor-Analyse:** in mehreren Punkten korrigiert (siehe §5).

---

## 1. Segmentierung (Boots gewollt/ungewollt)

| Segment | Zeitraum | Zeilen | r_boot | b_boot | Bewertung |
|---|---|---|---|---|---|
| S1: regler-1.22.0 | 13:16:32 – 15:28:45 | 474 | 50 konst. | 51 konst. | **0 ungewollte Reboots** |
| Flash-Fenster | ~15:26 – 15:31 | 13 | 50→54 (+4) | 51→57 (+6) | **gewollt** (Flash beider Geräte) |
| S2: regler-1.23.0 | 15:31:06 – 06:59:57 (15 h 29 min) | 3049 | 54 konst. | 57 konst. | **0 ungewollte Reboots** |

Genau **ein** Boot-Übergang (15:26→15:31); kein Event-Zähler fällt innerhalb S1/S2 → keine versteckten Reboots.
Auffällig: im 5-min-Flash-Fenster bootete Regler 4×, Broker 6× (mehrfache Flash-/Erase-Zyklen).

**⚠️ Beobachtungslücke 19:34:12 → 22:02:50 (2 h 29 min, 16 % des Laufs)** — von der Vor-Analyse nicht erwähnt.
Entlastend: `r_boot` blieb 54, RAM-Zähler blieben konstant → **beweisbar kein Reboot in der Lücke**; das übrige
Verhalten ist dort aber unbelegt. Zusätzlich 27 Zeilen mit leeren Regler-Feldern (Logger-Aussetzer).

## 2. Stabilitäts-Verdikt S2 (produktiver Zustand) — bestanden, mit Sternchen

- 15 h 29 min ohne Reboot/Counter-Reset; `mqtt_reconn`=1 konstant (**+0**), `enforce_count`=114 konstant,
  `enforcer_active` nie True, `gate_soc` folgt soc.
- `shelly_fail` +2650 in S2 → Grundrate ~126/h (1 Fehler/~29 s) auch in gesunden Phasen; **in der Logging-Lücke ~409/h**
  → vermutlich partieller Netzwerkausfall (Logger UND Shelly betroffen, Regler↔Broker nicht → getrennte Segmente).

## 3. Regelgüte (Zahlen — hier weicht das Review deutlich ab)

Gesamt-S2 unbrauchbar als Güte-Metrik, weil Akku ab 15:31 **100 %**: 15:31–19:34 Export bis **2625 W**
(PV-Überschuss, physikalisch unvermeidbar). Gesamt-S2: Median −19 W, Mittel **+240 W**, nur **44,7 %** in [−75…+25 W].

**Nachtfenster 22:02–06:00 (n=1894, fairer Prüfbereich):**
- Median **−49 W**, Mittel **−78 W** (Ziel −25 W), p5=−218 W, p95=+19 W
- **70,6 %** innerhalb ±50 W ums Ziel; nur 6 Zeilen Export >50 W
- **Kein Hunting:** 33 Modus-Übergänge in 8,9 h (3,7/h), 110 Setpoint-Änderungen auf 1893 Übergänge

**🔴 Hauptbefund — Aktuator-Nichtantwort:** In **218 von 352 aktiven Zeilen (62 %)** war `zout_w=0` trotz
`setpoint>30 W`. 17 Episoden, längste ~8–10 min (23:08–23:15 Setpoint bis **399 W**, Import 122–259 W;
05:26–05:35; 01:40–01:48). Auch bei Setpoint >100 W: 38/67 Zeilen ohne Lieferung. Schwere Episoden
korrelieren mit `tele_age` bis **288 s** → **Zendure schlief/reagierte nicht**. Die Nacht-Bilanz −78 W statt −25 W
geht großteils darauf zurück: **Regler stabil, Aktuator folgt nicht.** → eigenes Dokument `zendure-aktuator-nichtantwort.md`.

**Morgens 06:00–07:00:** 92 % Export >50 W (bis 321 W) bei SoC 97, act_mode=0 — kein Nachladen (SoC-Band 95–98, bewusst).
`act_mode=2` (Laden) kommt in **ganz S2 nie** vor → Ladepfad unter 1.23.0 ungetestet.

## 4. Feature-Evidenz

- **F4/Reconnect-Härtung: BESTÄTIGT (stark).** S1: +14 Reconnects/2,2 h (~6,4/h); S2: **+0 / 15,5 h** bei ähnlicher shelly_fail-Rate. Entkopplung Shelly↔MQTT belegt.
- **F1/Grid-Blind-Eskalation: UNGETESTET.** `grid_stale` nur **1 Sample** True (18:36:47, ≤~35 s), nie annähernd 12,5 min → kein Funktionsnachweis. Nur per Fault-Injection belegbar.
- **Soft-Retreat: 6 Auslösungen, alle 19:27–19:31**, echte Last-/Netzturbulenz (grid −1716…+370 W, Setpoint bis 1605 W, Zendure lieferte 1272–1532 W); korrekt auf 0 zurückgezogen. Endet 3 min vor der Logging-Lücke → vermutlich dasselbe Störereignis ~19:30.
- **Enforcer/Gate: S2 sauber** (+0, nie aktiv). Flash-Fenster: enforce_count 25→114 (**+89 in ~3 min**) — plausibel Klemmen bei >2 kW Export während Regler/Broker down; gegen Enforcer-Log gegenprüfen.

## 5. Abgleich mit der Primär-Analyse

| Primär-Behauptung | Review-Verdikt |
|---|---|
| mqtt_reconn +0 (F4) | ✅ bestätigt |
| 0 ungewollte Reboots | ✅ bestätigt (NVS-Beweis), aber 2,5-h-Lücke ungenannt |
| Enforcer/Gate sauber | ✅ bestätigt für S2 |
| grid_stale=0, „F1 nie gefeuert" | ⚠️ halb falsch: 1 Sample True; F1 *unverifiziert*, nicht *bewährt* |
| clients immer 2 | ❌ falsch: 4× clients=3 (16:50/17:03/17:27/23:16) + 1× clients=1 (17:16) — **dritter MQTT-Client** unbekannt |
| tele_stale „graziös" | ⚠️ beschönigt: 22 Episoden bis 288 s, schwere korrelieren mit Nicht-Lieferung |
| „rock-solid", Median −44 W, 78 % in 50 W | ❌ nicht reproduzierbar: Gesamt-S2 44,7 %, Nacht 70,6 %/Median −49 W; „rock-solid" verdeckt die 62 % Aktiv-Samples ohne Aktuator-Antwort |

## 6. Verdeckte Probleme / offene Punkte (Review)

1. **2,5-h-Logging-Lücke** 19:34–22:02 mit verdreifachter shelly_fail-Rate → Netzwerk-Teilausfall? Ursache klären. 16 % Blindzeit gehört in den Bericht.
2. **Aktuator-Nichtantwort** (zout=0 bei aktivem Setpoint) — größtes reales Regelgüte-Problem. → separates Dokument.
3. **Ladepfad (act_mode=2) unter 1.23.0 komplett ungetestet** — Lauf lief mit vollem Akku.
4. **Kein Nachladen bei SoC 97 trotz Export** — SoC-Band 95–98 (bewusste Hysterese, regler.ino:329-330); als Sommer-Optimierung akzeptiert.
5. **F1 ohne Feldnachweis** — Fault-Injection-Test nötig (Shelly >12,5 min trennen).
6. **Dritter MQTT-Client** sporadisch — identifizieren.
7. **shelly_fail-Grundrate ~1/29 s** auch im Normalbetrieb — Polling-Robustheit, bevor sie F1-Grenzen berührt.

**Gesamtverdikt:** *Plattform*-Stabilität von 1.23.0 mit harten Zahlen bestätigt (kein Crash/Reconnect, Enforcer ruhig).
Das *Qualitätsurteil* „rock-solid" hält nicht: Regelgüte durch schlafenden Aktuator + fehlende Lade-Abdeckung
schwächer belegt als behauptet; 2,5-h-Blindzeit gehört benannt.
