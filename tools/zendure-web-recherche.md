# Web-/Community-Recherche — Zendure Aktuator & Standby (Stand 2026-07-16)

Ergänzung zu `zendure-aktuator-nichtantwort.md` (offizielle Repos) um **Community-/Praxiswissen**.
Methode: WebSearch + WebFetch; Zendure-Foren via Flarum-API (`/api/discussions/<id>`), da die Web-UI JS-gerendert ist.
**Caveat:** Viel Community-Material bezieht sich auf die **SolarFlow 800 Pro** oder **2400 AC**, nicht 1:1 auf unsere **2400 Pro** — gerätespezifische Felder/Werte vor Übernahme prüfen.

---

## 1. Der 100-%-SoC-Standby-Lockup (unser Fall) — Einordnung

Bereits offiziell belegt via Zendure-HA #1505/#1503/#1489 (siehe Dossier §7). Die Web-Recherche zeigt: **„Standby-fest"
ist ein wiederkehrendes Zendure-Thema mit MEHREREN verschiedenen Ursachen** — unser 100-%-Lockup ist nur eine davon.
Wichtig fürs Dossier: die anderen Ursachen sauber ausschließen (unten §2), damit wir gegenüber Zendure präzise sind.

## 2. Andere Standby-Ursachen — bei uns ausgeschlossen (Ausschlussliste)

- **RJ45 = nur Smart Meter, nicht LAN** (Forum d/27825): Nach FW **V2.0.0** blieb eine 2400 Pro „nur Standby, ohne
  Warnung", sobald ein **normales LAN-Kabel** in den RJ45 gesteckt war. Moderator (Tim) + Community: der RJ45 ist für
  den **Zendure Smart Meter (3ct-s)** gedacht; ein Fremd-LAN-Gerät ohne gültige Meter-Daten → HEMS bleibt im Standby.
  Workaround: **LAN abziehen + Hardware-Reset**. Staff bestätigt FW-Bug (unzureichende Edge-Case-Tests bei V2.0.0).
  → **Bei uns irrelevant:** unsere Zendure hängt per **WLAN** (.109, rssi≈−56); wir stecken nichts in den RJ45.
  Merke aber: **Zendure-RJ45 NICHT als LAN nutzen.**
- **3CT-Meter-Fehlkonfig / HEMS** (Forum d/26283): 2400 AC dauerhaft Standby, 0 W, meist Meter-Platzierung/HEMS-Setup;
  endete im Rückversand. → Bei uns irrelevant (wir regeln über Shelly, nicht über das Zendure-HEMS/3CT).
- **Bypass bei ~98 %** (mehrere Threads): Gerät geht in Bypass und liefert nicht. → Wir setzen `gridReverse=Disallow`;
  relevant nur, falls Bypass-Verhalten reinspielt — im Auge behalten.

**Fazit §2:** Unsere Signatur (SoC 97–100 %, nachts, `zout=0` trotz gültigem Sollwert, `tele_age`↑) passt auf den
**100-%-Lockup (#1505)**, nicht auf die Meter-/LAN-/HEMS-Fälle. Das stärkt die socSet≤99 %-Hypothese.

## 3. Lokale Steuerung — Community-Best-Practice (HA-Community „SF800 Pro completely local")

Quelle: community.home-assistant.io/t/…/980110 (ausgereiftes lokales Zero-Feed-in-Projekt, 800 Pro).
- **Schreiben über lokale HTTP-REST, NICHT MQTT:** `POST http://<ip>/properties/write`. Community-Konsens:
  **MQTT-*Write* ist in neuerer FW eingeschränkt/verschlüsselt** → Steuern via REST, MQTT nur zum Lesen.
  *(Deckt sich mit zenSDK: `/properties/write` ist der offizielle Schreibweg; und mit EN-18031-Hinweis.)*
- **Kommandoformat (800 Pro!):** `{"properties":{"outputLimit":W,"chargeMaxLimit":0},"sn":…}` fürs Entladen,
  Felder getauscht fürs Laden. **Achtung:** dieses Projekt nutzt `chargeMaxLimit` als Lade-*Leistung* — unser
  Gerät nutzt `inputLimit` (empirisch bestätigt). **Gerätespezifisch, nicht blind übernehmen.**
- **Serial-Number-Gate:** „Strict serial number verification before every HTTP command" — Sicherheitsschranke
  vor jedem Write. (Analog unserem Gate g.)
- **API-Feld-Swap zwischen FW-Versionen:** „Corrects swapped API values (input/output) that occur with some
  firmware versions." → **Robustheits-Warnung:** Feldzuordnung kann sich per FW ändern → Rücklesung/Plausibilität nötig.
- **Regelzyklus:** 5 s Steuerung, 15 s Sensor-Poll, `timeout:30`; **memory-based** (`last_sent_limit`) statt auf
  träges Sensor-Echo zu warten → „closed-loop without lag". (Wir machen das ähnlich: jeden Takt senden + Echo-Liveness.)

## 4. Anti-Standby / Wake — Techniken „aus der Wildnis"

- **Sättigungs-/Mindestabgabe:** o. g. Projekt erzwingt **min. 150 W Abgabe, wenn SoC > 98 % UND PV-Überschuss**,
  um das Gerät aktiv zu halten. → empirische Bestätigung unseres Kandidaten **„Mindest-Entlade-Sollwert"** und der
  Wake-Idee. (Offizieller HA-Client: nie < 50 W, Kickstart-Überhöhung bei `homeOutput==0` — Dossier §7.)
- **Manuelles AC-Input-Verstellen weckt** (Forum d/1312-Kontext, mehrere Threads): kurzes Setzen eines AC-Input-Werts
  reaktiviert das Gerät → stützt den **Wake-Puls** (`outputLimit=0` → ~20 s → Sollwert).
- **Konsens-Workaround gegen 100-%-Lockup:** **Lade-/SoC-Limit unter 100 %** halten (99 %) — mehrfach genannt.

## 5. Firmware-Vorsicht (belegt)

- **V2.0.0** hat dokumentierte Standby-Edge-Case-Bugs (Forum d/27825, Staff-bestätigt).
- **MQTT-Write-Restriktion / EN 18031:** lokale HTTP-API kann per FW standardmäßig deaktiviert werden (Re-Enable via
  HEMS); MQTT-Write in neuerer FW eingeschränkt. → **FW-Version pinnen, Änderungen beobachten**, Ausfallpfad dokumentieren.
- **Feld-Swap input/output** je FW → nach jedem FW-Update Vorzeichen/Feldzuordnung per Rücklesung verifizieren.

## 6. Neue/geschärfte Punkte für unsere Regler-/Broker-Analyse

1. **socSet ≤ 99 %** bleibt der führende Fix — jetzt zusätzlich durch Community-Konsens gestützt. (Experiment aufgesetzt.)
2. **Mindest-Entlade-Sollwert / Sättigung** (≥ 50–150 W bei SoC hoch + Bedarf) als Anti-Standby — empirisch verbreitet.
3. **Strategisches Robustheits-Risiko: MQTT-Write könnte per FW wegfallen.** Unser gesamter L2-Gate/Enforcer-Bau
   sitzt auf dem MQTT-*Write*-Pfad. Falls Zendure MQTT-Write weiter einschränkt, müssten wir auf `/properties/write`
   (REST) ausweichen → das **umgeht** unsere MQTT-Gate-Architektur. **Kein Umbau jetzt**, aber als Risiko notieren und
   FW-Verhalten beobachten. (Gegenprobe: schreibt unser Pfad heute zuverlässig? Echo bestätigt ja — bei FW-Update erneut prüfen.)
4. **Feld-Swap-Absicherung:** unsere Rücklesung/Plausibilität (z. B. socSet ×10 → 990) und FW-Pinning sind gegen den
   dokumentierten input/output-Swap goldrichtig — beibehalten.
5. **RJ45-Warnung** in die Deployment-Notizen: Zendure-RJ45 ist Smart-Meter-Port, kein LAN.

## 7. Referenz-Projekte (für spätere Vertiefung)
- `reinhard-brandstaedter/solarflow-control` (GitHub) — reifer Dritt-Controller für Solarflow-Hub (Flexibilität, Bedarfsanpassung).
- `arselzer/solarflow-homeassistant` (GitHub) — HA-Paket für (mehrere) 800 Pro.
- HA-Community „SF800 Pro completely local" — lokaler REST-Regler mit Sättigungs-/Fast-Clamp-Logik.

---

## Quellen
- https://github.com/Zendure/Zendure-HA/issues/1505 (100-%-Discharge-Stop, 2400 Pro) · #1503 · #1489 · #1312 · #1320 · #1230
- https://forum.zendure.com/d/27825 (2400 Pro nur Standby nach FW V2.0.0 + LAN; RJ45=Smart-Meter)
- https://forum.zendure.com/d/26283 (2400 AC Standby-fest, 3CT/HEMS)
- https://community.home-assistant.io/t/zendure-solarflow-800-pro-completely-local-zereo-feed-in-without-cloud-and-even-faster-no-mqtt-no-hacs-easy-mode/980110 (lokaler REST-Regler, Sättigung 150 W, Fast-Clamp)
- https://github.com/Zendure/zenSDK (offizielle lokale API)
- https://forum.iobroker.net/topic/82263 (zenSDK lokale API / SmartMode) · https://github.com/reinhard-brandstaedter/solarflow-control · https://github.com/arselzer/solarflow-homeassistant
