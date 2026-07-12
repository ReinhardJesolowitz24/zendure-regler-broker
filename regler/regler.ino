/*
 * Zendure SolarFlow 2400 Pro - LOKALER REGLER  (read+write)
 * ============================================================================
 * Board : ESP32-S3R8 + onboard W5500 (kabelgebundenes Ethernet, KEIN WLAN)
 * Rolle : liest Shelly-Netzleistung -> rechnet Sollwert -> published per MQTT
 *         an den lokalen Broker (-> Zendure). Empfaengt Zendure-Telemetrie.
 *
 * !! CORE 3.x NOETIG !!  (esp32 by Espressif, Version 3.x)
 *   W5500 via ESP32-nativem ETH.h (SPI) als lwIP-Interface -> WiFiClient laeuft
 *   TRANSPARENT ueber Ethernet (die WIZnet-"Ethernet"-Lib wird NICHT benutzt,
 *   ihr EthernetClient/Server ist auf ESP32 problematisch). ETH.h = im Core.
 *   Headless -> Core 3.x unproblematisch (Waechter bleiben separat auf 2.0.17).
 *
 * BIBLIOTHEKEN (Library Manager):
 *   - PubSubClient (Nick O'Leary) ODER Fork "PubSubClient3" (drop-in, +QoS) -> MQTT-Client
 *   - ArduinoJson (>= 7.x) -> JSON
 *   (ETH/SPI/WiFi = im Core 3.x enthalten.)
 *
 * VOR DEM KOMPILIEREN: arduino_secrets.h ausfuellen.
 * ⚠️ ETH.begin()-Signatur ggf. je 3.x-Version anpassen (siehe Beispiel
 *    File -> Beispiele -> ETH -> "ETH_W5500" auf deinem Board zuerst verifizieren).
 *
 * STATUS: SKELETT. Regel-Algorithmus + Zendure-Topics sind TODO (Referenz:
 *   solarflow-control fuer die Logik, zenSDK fuer Topics/Kommandos).
 * ============================================================================
 */
#include <ETH.h>
#include <SPI.h>
#include <WiFi.h>            // WiFiClient + Network-Events -- laufen ueber ETH
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_system.h"     // esp_reset_reason() fuer /status
#include "esp_task_wdt.h"   // HW-Watchdog (loop-Hang -> Reboot)
#include "esp_timer.h"      // esp_timer_get_time() -> ueberlaufsichere uptime (64-bit us)
#include <Preferences.h>    // persistenter boot_count (NVS, ueberlebt Power-off)
#include "arduino_secrets.h"

// --- Core-Schutz: braucht esp32 core 3.x (W5500 via ETH.h). Falscher Core = Compile-Fehler. ---
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "Falscher Core! Regler braucht 'esp32 by Espressif' >= 3.x (Tools -> Board -> Boards Manager). Die Waechter laufen auf 2.0.17 - hier 3.x waehlen!"
#endif

// ---- W5500 SPI-Pins: Waveshare ESP32-S3-ETH (RJ45/Type-C/TF) ----------------
//   Verifiziert gegen Waveshare-Wiki/ESP-IDF-Beispiel fuer dieses Board.
//   ACHTUNG: andere Belegung als die Variante ESP32-S3-ETH-8DI-8RO!
#define ETH_PHY_TYPE  ETH_PHY_W5500
#define ETH_PHY_ADDR  1
#define PIN_ETH_SCK   13
#define PIN_ETH_MISO  12
#define PIN_ETH_MOSI  11
#define PIN_ETH_CS    14
#define PIN_ETH_IRQ   10     // -1 falls nicht verbunden
#define PIN_ETH_RST    9     // -1 falls nicht verbunden

// ---- Netzwerk-Ziele (aus arduino_secrets.h) --------------------------------
const uint16_t BROKER_PORT = 1883;
const uint16_t SHELLY_PORT = 80;
const char*    SHELLY_PATH = "/rpc/EM.GetStatus?id=0";   // total_act_power

// ---- Shelly-Read-Robustheit -------------------------------------------------
//   Faengt die ~15% sporadischen Verbindungs-Ablehnungen ab, die der Shelly bei
//   haeufigen Kurzverbindungen (1/s) zeigt: bis zu SHELLY_TRIES Versuche pro Poll,
//   kurzer Connect-Timeout, Gesamt-Zeitbudget schuetzt die Regelschleife vor langem Blockieren.
const int           SHELLY_TRIES      = 3;     // Versuche pro Poll
const int           SHELLY_CONNECT_MS = 700;   // Connect-Timeout je Versuch (kurz -> kein langes Blockieren)
const unsigned long SHELLY_RESP_MS    = 1200;  // max. Warten auf 1. Antwortbyte je Versuch
const unsigned long SHELLY_RETRY_MS   = 40;    // Pause zwischen Versuchen
const unsigned long SHELLY_BUDGET_MS  = 2500;  // Gesamt-Zeitbudget pro Poll (Loop-Schutz)
const long          GRID_PLAUSIBLE_W  = 50000; // Plausibilitaetsgrenze: |Netzleistung| groesser -> Read verwerfen
                                               //   (faengt Parse-Muell/NaN ab; kein reales Haus erreicht 50kW)

// ---- Regel-/Timing-Parameter ------------------------------------------------
const unsigned long POLL_MS         = 1000;    // Shelly-Netzleistung lesen
const unsigned long PUBLISH_MS      = 2000;    // Sollwert an Zendure senden
const unsigned long HEARTBEAT_MS    = 2000;
const unsigned long TELE_TIMEOUT_MS = 15000;   // Fail-Safe: kein KONTAKT zum Geraet (kein Echo/Telemetrie) mehr.
                                               //   Echo kommt ~alle 2s (auch idle) -> 15s toleriert einige QoS0-Verluste,
                                               //   erkennt echten Link-Verlust rasch. (Frueher 20s auf SoC bezogen = falsch,
                                               //   SoC kommt nur ~alle 2-3min -> stotterte; jetzt auf Echo-Kontakt bezogen.)
const unsigned long SOC_MAX_AGE_MS  = 600000;  // Fail-Safe: Link lebt (Echo), aber >10min KEIN echter SoC -> SoC misstrauen
                                               //   -> Entladen sperren (Tiefentlade-Schutz); Laden bleibt erlaubt (kann nicht tiefentladen).
const unsigned long GRID_STALE_MS   = 10000;   // Fail-Safe: Shelly-Netzdaten aelter -> NICHT mehr regeln. 2026-07-12 5s->10s: kurze Shelly-Aussetzer (shelly_fail) NICHT mehr sofort auf Idle-0 (Zendure stotterte) -> letzten Sollwert halten (Zendure haelt ohnehin), erst >10s echter Ausfall -> Safe-0
                                               //   (Retry haelt shelly_age i.d.R. <=1-2s; erst echter Verlust greift)
const uint32_t      WDT_TIMEOUT_MS  = 12000;   // HW-Watchdog: loop-Hang laenger -> Reboot. 12s = grosser
                                               //   Abstand zum Worst-Case (Shelly 2,5s + /status 1s), faengt
                                               //   auch etwas laengere Stoerungen ab; reset_reason wird TASK_WDT
const long TARGET_GRID_W = -25;                // Ziel-Netzleistung (W). -25 = bewusst minimaler Bezug,
                                               //   um Einspeisung sicher zu vermeiden (bewaehrter GIGA-Wert).
const long ZEN_MIN_W = 0, ZEN_MAX_W = 2400;    // ENTLADE-Klemme (W): Sollwert >0 -> outputLimit (0..2400).
const long ZEN_CHARGE_MAX_W = 2400;            // LADE-Klemme (W): Sollwert <0 -> inputLimit=|w| (0..2400).
                                               //   Volle Leistung beide Richtungen (eigene 2,5mm2-Leitung).
// BIDIREKTIONAL (Chunk 4, FW 2.0.45): EIN signierter Sollwert. >0 = ENTLADEN (outputLimit + acMode="Output mode"),
//   <0 = LADEN (inputLimit=|w| + acMode="Input mode"), ~0 = idle. Integrator laeuft ueber [-LADE_MAX, +ENTLADE_MAX].
//   Empirie 2026-07-10: Laden lokal bestaetigt (gridInputPower 0->150W, Shelly-Export sank um ~150W).

// ---- AKTUIERUNGS-SCHARFSCHALTUNG (Chunk 2) --------------------------------------------------
//   ⚠️ ACTUATE_ENABLE=false => Regler schreibt NICHT ans Geraet (nur Debug-Topic) = read-only-sicher.
//   ⚠️ Auf true NUR wenn ALLE Gates erfuellt UND ZEN_MAX_W zuerst KLEIN (~200) gesetzt ist (gestuft, Gate d)!
//   Empirie 2026-07-10: Zendure HAELT letzten Sollwert unbegrenzt (a1+a2), QoS0 kann Kommando verlieren
//   -> Sollwert wird jeden PUBLISH_MS-Takt neu gesendet (= Wiederholung); Uebergang auf 0 zusaetzlich als Burst.
const bool          ACTUATE_ENABLE = true;     // MASTER: scharf. 2026-07-10 Nachtlauf mit Tuning-Updates (KI 0.15, Gegenrichtungs-Abbruch, asymm. Slew)
const long          DEV_MINSOC     = 30;       // Geraete-Backstop minSoc (broker-unabh. letzte Linie, Bereich 5..50)
const unsigned long BACKSTOP_MS    = 30000;    // Geraete-Backstops (minSoc + gridReverse=Disallow) periodisch nachsetzen

// ---- SoC-Grenzen (Controller-seitig, UNABHAENGIG vom Geraet = 1. Sicherheitsebene) ----------
//   Defense-in-Depth: (1) HIER stoppt der Regler das Entladen; (2) optional spaeter zusaetzlich das
//   Geraete-minSoc (MQTT number/minSoc, 5..50) als unabhaengigen Backstop. Beide wirken redundant.
//   ⚠️ Die HEMS-Entladeschwelle (30%) gilt NUR mit Cloud-HEMS. Im lokalen/manuellen Betrieb erzwingt
//      sie NIEMAND ausser diesem Regler -> daher hier fest verankert.
const long SOC_STOP_DISCHARGE = 30;   // % - bei/unter diesem SoC NICHT mehr entladen (= HEMS-Schwelle)
const long SOC_RESUME_HYST    = 3;    // % - Hysterese: Freigabe erst ab (STOP+HYST) bzw. (CHARGE-HYST) -> beide Richtungen
const long SOC_STOP_CHARGE    = 98;   // % - bei/ueber diesem SoC NICHT mehr laden (Lade-Decke, analog 30%-Floor).
                                      //   Verhindert Windup gegen die volle Batterie; BMS/socSet schuetzen zusaetzlich.

// ---- Reglerparameter (PI, BEWUSST OHNE D) ----------------------------------------------------
//   D weggelassen: wuerde Messrauschen verstaerken + vertraegt sich schlecht mit der ~10s
//   Zendure-Aktuator-Totzeit. Leitsatz: Stabilitaet > letzte % Wirkungsgrad.
//   Start als reiner I-Regler (KP=0); konservativ, am Geraet vorsichtig hochtasten.
const float KP_REGLER  = 0.0f;        // Proportionalanteil (0 = reiner I-Regler zum Start)
const float KI_REGLER  = 0.15f;       // Integralverstaerkung pro Takt. 2026-07-10: 0.30 -> 0.15, weil bei
                                      //   schwankender PV/Last Hunting (+-500-600W) gegen die ~10s Totzeit; 0.15 = ruhiger.
const long  DEADBAND_W = 20;          // Totband um TARGET_GRID_W (W) -> keine Reaktion auf Messrauschen
// RICHTUNGS-ABHAENGIGE Slew (2026-07-11, Relais-Schonung): Anpassung in die GLEICHE Richtung zuegig;
//   Nulldurchgang Laden<->Entladen (Vorzeichenwechsel = ggf. internes Relais) DEUTLICH langsamer/gedaempft.
const long  SLEW_SAME_W     = 500;    // max. Integralschritt/Takt bei gleicher Richtung (bzw. Abbau fern von 0): zuegig.
const long  SLEW_REVERSAL_W = 120;    // max. Integralschritt/Takt in der Umschalt-Zone nahe 0: SANFT (Relais schonen).
const long  REVERSAL_BAND_W = 400;    // |Sollwert| <= 400 UND Richtung dreht -> Umschalt-Zone -> SLEW_REVERSAL_W.
const long  MODE_HYST_W = 40;         // Mode-Totband (W): |Sollwert| < 40 -> idle. Trennt Laden/Entladen um 2x40W
                                      //   -> verhindert Input<->Output-Modus-Flattern (Relais) am Nulldurchgang.
// GEGENRICHTUNGS-ABBRUCH (Feed-in-Schutz, 2026-07-10): schneller Stop auf dem ROHEN Netz. Der fruehere asymm.
//   "toward-zero fast"-Slew ist ersetzt durch RETREAT_RAW_W (harter Stop) + die richtungs-abhaengige Slew oben.
const long  RETREAT_RAW_W = 300;      // Feed-in-Schutz auf dem ROHEN Netz: entladen trotz Einspeisung > 300W (oder
                                      //   laden trotz Bezug > 300W) -> Ruecknahme. Hoch genug, dass kleine Blinker
                                      //   durchlaufen (PI regelt gefiltert), aber echter grosser Wegfall reagiert schnell.
// SANFTER Retreat (2026-07-12): FRUEHER hart Integrator=0 (return 0) -> Zendure STOPPTE bei jedem Roh-Blip und
//   brauchte ~30-40s Wiederanlauf -> sichtbares "immer wieder auf 0W"-Stottern. JETZT proportional:
const float RETREAT_GAIN       = 0.5f;   // nimm diesen Anteil des Roh-Ueberschusses raus (0=aus..1=voll). Klein=sanft.
const float RETREAT_FILT_BLEND = 0.5f;   // Filter-Nachzug beim Retreat (0=gar nicht..1=harter Reset auf Roh). Halb=weniger Nachdrall bei Blips.
// NETZ-TIEFPASS (2026-07-11, aus Nachtlauf: Abend-Std 700-900W durch Jagen taktender Lasten -> Loecher treffen).
//   EMA auf gridPowerW -> Regler+Retreat regeln den MITTELWERT ("Ruhe"). tau > Aktuator-Totzeit (~10s), damit
//   Sekunden-Blinker ausgemittelt werden, echte (langsame) Laststufen aber noch durchkommen. Poll ~1s.
const float GRID_FILT_TAU_S = 15.0f;  // Zeitkonstante (s). 2026-07-11 Feintuning: 25 -> 15s -> reagiert ~40% schneller
                                      //   auf echte Laststufen + mehr Phasenreserve (weniger Filter-Lag); bleibt >~Totzeit(10s),
                                      //   Ruhe erhalten. KI bewusst unveraendert (Stabilitaet). alpha = dt/tau (dt>=tau -> Auto-Reinit).

// ---- MQTT-Topics (echte Zendure-HA-Discovery-Topics, am Geraet verifiziert 2026-07-10) ------
const char* T_HEARTBEAT = "regler/heartbeat";
// LESEN (Klartext-Payload, KEIN JSON): SoC + tatsaechliche Ausgangsleistung
const char* T_SOC       = "Zendure/sensor/" SECRET_ZEN_SN "/electricLevel";     // -> zSoc (SoC %)
const char* T_OUTHOME   = "Zendure/sensor/" SECRET_ZEN_SN "/outputHomePower";   // -> zOutW (Ist-Ausgang W)
// SCHREIBEN (Chunk 2, noch NICHT genutzt): Entlade-Sollwert + Modus
const char* T_OUTLIMIT_SET = "Zendure/number/" SECRET_ZEN_SN "/outputLimit/set";
const char* T_INLIMIT_SET  = "Zendure/number/" SECRET_ZEN_SN "/inputLimit/set";     // Chunk 4: Lade-Sollwert
// Device-Echo (State-Topics OHNE /set): Geraet bestaetigt Sollwert ~1s nach Schreiben -> Liveness/ACK-Signal
const char* T_OUTLIMIT_ST  = "Zendure/number/" SECRET_ZEN_SN "/outputLimit";
const char* T_INLIMIT_ST   = "Zendure/number/" SECRET_ZEN_SN "/inputLimit";
const char* T_ACMODE_SET   = "Zendure/select/" SECRET_ZEN_SN "/acMode/set";
const char* T_GRIDREV_SET  = "Zendure/select/" SECRET_ZEN_SN "/gridReverse/set";   // Backstop: kein Feed-in
const char* T_MINSOC_SET   = "Zendure/number/" SECRET_ZEN_SN "/minSoc/set";        // Backstop: kein Tiefentladen
// Chunk 1 Diagnose: berechneter Sollwert (NUR beobachten, NICHT ans Geraet)
const char* T_SETPOINT_DBG = "regler/setpoint_debug";
// GATE g (Gatekeeper): der Regler publiziert NICHT mehr direkt auf Zendure/.../set, sondern stellt ANTRAEGE auf
//   regler/cmd/*. Der BROKER validiert (absolute Physik) und ist einziger Schreiber ans Geraet (s. 02_gate-g).
//   -> Broker MUSS mit GATE_ENABLE laufen, sonst erreichen die Antraege das Geraet nicht (Broker zuerst flashen!).
const char* T_CMD_OUTLIMIT = "regler/cmd/outputLimit";
const char* T_CMD_INLIMIT  = "regler/cmd/inputLimit";
const char* T_CMD_ACMODE   = "regler/cmd/acMode";
// INTERNER Level-1-Monitor (Endwert-Check vor dem Antrag; deckt zufaellige Regler-Fehler frueh; NICHT common-cause-fest
//   -> dafuer der Broker-Gate auf getrennter MCU). Eigene Grenzen, uint16_t (dokumentiert nicht-negativ; im Vergleich
//   Integer-Promotion nach 32-bit signed int -> kein Unsigned-Wrap-Fallstrick).
const uint16_t MON_DISCHARGE_MAX_W = 2400;   // unabhaengig von ZEN_MAX_W
const uint16_t MON_CHARGE_MAX_W    = 2400;   // unabhaengig von ZEN_CHARGE_MAX_W

// ---- Firmware-Version (fuer /status + Baseline-/Versions-Check) -------------
#define FW_VERSION  "regler-1.20.0"  // 1.20.0: SANFTER Retreat (proportional RETREAT_GAIN=0.5 + Teil-Filter-Blend statt hart auf 0 -> Zendure drosselt statt zu stoppen, kein 0W-Stottern) + GRID_STALE_MS 5s->10s (kurze Shelly-Aussetzer halten letzten Sollwert). Basis 1.19.0. 1.19.0: MQTT-Reconnect-Haertung - eindeutiger Client-Name je Connect (keine Session-Kollision) + setSocketTimeout(4)/setKeepAlive(10) (Reconnect friert Loop/Heartbeat nicht 15s ein) -> beseitigt ~27s-Heartbeat-Luecken -> keine Enforcer-Fehltrips. Basis 1.18.0 (GATE g). 1.18.0: publishSetpoint stellt ANTRAEGE auf regler/cmd/* (Broker validiert+ist einziger Geraete-Schreiber); interner L1-Monitor. BROKER MUSS mit GATE_ENABLE laufen (zuerst flashen!)
SET_LOOP_TASK_STACK_SIZE(12288);     // loop-Task-Stack auf 12 KB anheben (Default 8192); Arduino-ESP32-Makro
uint32_t bootCount = 0;              // persistenter Reset-Zaehler (NVS) -> erkennt Resets ueber Neustarts hinweg

// ---- Laufzeit-Status --------------------------------------------------------
WiFiClient   netClient;        // MQTT-Socket (ueber ETH)
WiFiClient   httpClient;       // separater Client fuer Shelly-HTTP
WiFiServer   httpServer(80);   // read-only /status-API (unabhaengig von MQTT)
PubSubClient mqtt(netClient);
long  gridPowerW = 0;          // <0 Bezug / >0 Einspeisung  (= -total_act_power, s. Vorzeichen-Fix in readShellyOnce). RAW = nur Diagnose.
float gridPowerFilt = 0.0f;    // EMA-TIEFPASS des Netzsignals = REGELGROESSE (Regler+Retreat regeln den Mittelwert, s. GRID_FILT_TAU_S)
bool  gridFiltInit = false;    // Filter erstmals initialisiert?
unsigned long lastFiltMs = 0;  // Zeit des letzten Filter-Updates (fuer variables dt / Auto-Reinit nach Luecke)
long  zSoc = -1, zOutW = 0, setpointW = 0;
unsigned long lastPoll=0, lastPub=0, lastBeat=0, lastTele=0, lastBackstop=0, lastSocMs=0;
float ctrlInteg   = 0.0f;      // Regler-Integratorzustand (global -> /status-Diagnose)
bool  ctrlBlocked = false;     // Entlade-Sperre aktiv (SoC-Floor) (global -> /status)
bool  ctrlChargeBlocked = false;// Lade-Sperre aktiv (SoC-Decke) (global -> /status)
bool  ctrlSocOld  = false;     // SoC-Wert zu alt (Link lebt, aber kein SoC) -> Entladen gesperrt (global -> /status)
int   ctrlActMode = 0;         // 0=idle 1=entladen 2=laden (global -> /status)
bool  ctrlGridStale = false;   // Netzdaten veraltet -> Regler haelt sicheren Zustand (global -> /status)
bool  ctrlTeleStale = false;   // Zendure-Telemetrie veraltet -> SoC ungueltig -> sicherer Zustand (global -> /status)
bool  shellyOk    = false;     // letzter Shelly-Read erfolgreich (global -> /status)
unsigned long lastShellyMs = 0;
long  ctrlFehler = 0;          // Regelabweichung W vor Totband (Diagnose -> /status). long: passt zu gridPowerW
long  ctrlDelta  = 0;          // letzter Integralschritt W, slew-limitiert (Diagnose)
bool  ctrlSlew   = false;      // Slew-Rate-Begrenzung war aktiv (Diagnose)
bool  ctrlDeadband = false;    // Totband war aktiv (Diagnose)
unsigned long cntPub=0, cntShellyFail=0, cntShellyRetry=0, cntMqttReconn=0, cntRetreat=0, cntMonTrip=0;  // Zaehler (Diagnose); cntMonTrip=interner L1-Monitor
uint64_t      cntTele=0;       // empfangene Zendure-Telemetrie (hohe Rate -> 64-bit gegen Ueberlauf)
static volatile bool eth_up = false;   // volatile: wird im Netzwerk-Event-Task geschrieben, im loop-Task gelesen
char  buf[256];

void onNetEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_ETH_GOT_IP) { eth_up = true; Serial.print("ETH IP: "); Serial.println(ETH.localIP()); }
  else if (event == ARDUINO_EVENT_ETH_DISCONNECTED || event == ARDUINO_EVENT_ETH_STOP) { eth_up = false; Serial.println("ETH down"); }
}

void ethSetup() {
  Network.onEvent(onNetEvent);
  SPI.begin(PIN_ETH_SCK, PIN_ETH_MISO, PIN_ETH_MOSI);
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, PIN_ETH_CS, PIN_ETH_IRQ, PIN_ETH_RST, SPI);   // DHCP
  unsigned long t0 = millis(); while (!eth_up && millis() - t0 < 8000) delay(50);
}

// Shelly per HTTP lesen -> total_act_power. EIN Versuch (Connect/Read/Parse), kurzer Connect-Timeout.
bool readShellyOnce() {
  if (!httpClient.connect(SECRET_SHELLY_HOST, SHELLY_PORT, SHELLY_CONNECT_MS)) return false;
  httpClient.setTimeout(1000);   // ms - Inter-Byte-Timeout fuer readString()/readStringUntil()
  httpClient.print(String("GET ") + SHELLY_PATH + " HTTP/1.1\r\nHost: " + SECRET_SHELLY_HOST + "\r\nConnection: close\r\n\r\n");
  unsigned long t0 = millis();
  while (httpClient.connected() && !httpClient.available()) {
    if (millis() - t0 > SHELLY_RESP_MS) { httpClient.stop(); return false; }
    delay(1);
  }
  while (httpClient.available()) { String line = httpClient.readStringUntil('\n'); if (line == "\r" || line.length() <= 1) break; }
  String body = httpClient.readString();
  httpClient.stop();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (doc["total_act_power"].isNull()) return false;
  float p = doc["total_act_power"].as<float>();
  if (!(p >= -(float)GRID_PLAUSIBLE_W && p <= (float)GRID_PLAUSIBLE_W)) return false;  // Unsinn/NaN -> als Fehlversuch werten
  // ⚠️ VORZEICHEN-FIX (2026-07-10, Gate b EMPIRISCH belegt): Shelly-Rohwert total_act_power ist in
  //   DIESER Installation POSITIV=Bezug / negativ=Einspeisung. Die Regler-Konvention ist umgekehrt
  //   (gridPowerW<0=Bezug). Daher hier INVERTIEREN. Beleg: 150W Entladen -> Shelly fiel um ~150W
  //   (t6 +131W -> t8 -22W). Ohne Invertierung => positive Rueckkopplung => Runaway.
  gridPowerW = -(long)p;
  return true;
}

// Robuster Shelly-Read: mehrere Versuche pro Poll mit Gesamt-Zeitbudget (schuetzt die Loop).
//   cntShellyRetry zaehlt gescheiterte Einzelversuche -> zeigt, wie oft der Retry tatsaechlich greift.
bool readShelly() {
  unsigned long start = millis();
  for (int t = 0; t < SHELLY_TRIES; t++) {
    if (readShellyOnce()) return true;
    cntShellyRetry++;                                  // dieser Einzelversuch ist gescheitert
    if (millis() - start > SHELLY_BUDGET_MS) break;    // Zeitbudget erschoepft -> Loop nicht laenger blockieren
    delay(SHELLY_RETRY_MS);
  }
  return false;                                        // alle Versuche gescheitert (Loop zaehlt cntShellyFail)
}

// Zendure-Telemetrie: FW 2.0.45 publiziert PRO TOPIC einen Klartext-Wert (KEIN JSON).
//   LIVENESS-ARCHITEKTUR (2026-07-10, nach Kadenz-Messung): die Sensor-Topics (electricLevel/outputHomePower)
//   kommen SEHR SELTEN (~2-3 min) -> als Frische-Signal untauglich. Das Geraet ECHOT dagegen jeden
//   outputLimit/inputLimit-Schreibvorgang binnen ~1s zurueck (State-Topic ohne /set) -> das ist ein
//   End-to-End-ACK und kommt ~alle 2s (wir senden jeden Takt, auch idle=0). Daher:
//     - lastTele = "letzter KONTAKT zum Geraet" -> auf JEDE empfangene Geraetenachricht (inkl. Echo).
//     - lastSocMs = "letzter echter SoC-Wert" -> nur auf electricLevel (SoC-Alter separat, s. computeSetpoint).
void onMqtt(char* topic, byte* payload, unsigned int len) {
  cntTele++;
  lastTele = millis();                            // JEDE Geraetenachricht = Lebenszeichen/Kontakt (Echo zaehlt!)
  char val[24];                                   // Payload = kleine Klartext-Zahl -> kopieren + terminieren
  unsigned int n = (len < sizeof(val) - 1) ? len : sizeof(val) - 1;
  memcpy(val, payload, n); val[n] = '\0';
  if (strcmp(topic, T_SOC) == 0) {
    zSoc = atol(val);
    lastSocMs = millis();                         // echter SoC-Wert (steuert SoC-Alters-Guard, s.u.)
  } else if (strcmp(topic, T_OUTHOME) == 0) {
    zOutW = atol(val);                            // Ist-Ausgang (Diagnose / Soll-Ist-Vergleich)
  }
  // outputLimit/inputLimit-Echo: kein Feld noetig -> hat oben bereits lastTele (Kontakt) aufgefrischt.
}

bool mqttReconnect() {
  if (mqtt.connected()) return true;
  // EINDEUTIGER Client-Name je Verbindungsversuch: verhindert Kollision mit einer noch
  // nicht abgeraeumten alten Session gleichen Namens am Broker (sonst ~keepalive-lange
  // Reconnect-Luecke -> Heartbeat weg -> Enforcer-Fehltrip). PicoMQTT haelt eine dyn.
  // Client-Liste (kein festes Limit); alte Session raeumt via Keepalive selbst ab.
  char cid[40];
  snprintf(cid, sizeof(cid), "zendure-regler-%lu", (unsigned long)millis());
  if (mqtt.connect(cid)) {
    mqtt.subscribe(T_SOC);
    mqtt.subscribe(T_OUTHOME);
    mqtt.subscribe(T_OUTLIMIT_ST);                // Device-Echo -> schnelles Liveness-Signal (~2s)
    mqtt.subscribe(T_INLIMIT_ST);
    cntMqttReconn++;
    if (ACTUATE_ENABLE) assertDeviceBackstops();   // Backstops direkt nach (Wieder-)Verbindung setzen
    Serial.println("MQTT verbunden + subscribed (electricLevel, outputHomePower).");
    return true;
  }
  return false;
}

// PI-Regler Richtung Nulleinspeisung -- BIDIREKTIONAL (Chunk 4). SIGNIERTER Sollwert:
//   >0 = ENTLADEN (Bezug decken), <0 = LADEN (Ueberschuss absorbieren), ~0 = idle.
//   Anti-Windup BY CONSTRUCTION: der Integrator `ctrlInteg` IST die Stellwert-Basis und wird selbst hart auf
//   [loLimit, hiLimit] geklemmt -> kann bei Saturierung (Last/Ueberschuss > 2400W) nicht ueber die Grenze
//   aufwachsen. Die Grenzen sind SoC-abhaengig (Defense-in-Depth, 1. Ebene, mit Hysterese):
//     - SoC <= 30% : hiLimit=0 -> ENTLADEN gesperrt, Laden bleibt erlaubt (hebt SoC vom Floor).
//     - SoC >= 98% : loLimit=0 -> LADEN gesperrt, Entladen bleibt erlaubt.
//   Saturierung = definierter ruhender Zustand auf der jeweiligen Grenze (keine Oszillation).
//   Wird im PUBLISH_MS-Takt (2s) aufgerufen = Abtastzeit des I-Anteils.
long computeSetpoint() {
  // Integrator `ctrlInteg` + Sperr-Flags sind GLOBAL (Diagnose via /status).
  ctrlFehler = 0; ctrlDelta = 0; ctrlSlew = false; ctrlDeadband = false;  // Diagnose-Default (inaktiv)

  // --- 1. Sicherheitsebene: Frische der Daten -> sonst sicherer Zustand (0 W = idle, beide Richtungen aus) ---
  // 1a) GRID-STALENESS: ohne frische Shelly-Netzdaten NICHT auf alten Daten regeln.
  ctrlGridStale = (lastShellyMs == 0) || (millis() - lastShellyMs > GRID_STALE_MS);
  if (ctrlGridStale) { ctrlInteg = 0.0f; return 0; }          // Integrator leeren, idle (bumpless)

  // 1b) KONTAKT-STALENESS (Link tot): kein Echo/keine Nachricht mehr vom Geraet -> FULL idle (beide Richtungen).
  //     lastTele wird auf JEDE Geraetenachricht (inkl. outputLimit/inputLimit-Echo ~alle 2s) aufgefrischt.
  ctrlTeleStale = (lastTele == 0) || (millis() - lastTele > TELE_TIMEOUT_MS);
  if (ctrlTeleStale) { ctrlInteg = 0.0f; return 0; }

  // 1b2) SoC NIE empfangen -> Floor UND Decke unbekannt -> FULL idle bis zum 1. echten SoC.
  if (zSoc < 0 || lastSocMs == 0) { ctrlInteg = 0.0f; return 0; }

  // 1b3) SoC-Wert zu ALT trotz lebendem Link (>10min kein echter SoC) -> misstrauen: nur ENTLADEN sperren
  //      (Tiefentlade-Schutz). LADEN bleibt erlaubt (kann per Definition nicht tiefentladen). KEIN return.
  ctrlSocOld = (millis() - lastSocMs > SOC_MAX_AGE_MS);

  // 1c) SoC-abhaengige RICHTUNGS-Sperren (Floor/Decke, je mit Hysterese) -> setzen die Integrator-Grenzen.
  if (zSoc <= SOC_STOP_DISCHARGE)                        ctrlBlocked = true;         // Entlade-Sperre rein
  else if (zSoc >= SOC_STOP_DISCHARGE + SOC_RESUME_HYST) ctrlBlocked = false;        // erst mit Hysterese raus
  if (zSoc >= SOC_STOP_CHARGE)                           ctrlChargeBlocked = true;   // Lade-Sperre rein
  else if (zSoc <= SOC_STOP_CHARGE - SOC_RESUME_HYST)    ctrlChargeBlocked = false;  // erst mit Hysterese raus
  long hiLimit = (ctrlBlocked || ctrlSocOld) ? 0 :  ZEN_MAX_W;         // Entladen: Floor ODER SoC-zu-alt sperrt
  long loLimit = ctrlChargeBlocked           ? 0 : -ZEN_CHARGE_MAX_W;  // max. Laden (negativ)

  // --- 2. Regelabweichung auf dem GEFILTERTEN Netz (TIEFPASS = "Ruhe") ---
  //   Bezug -> fehler>0 -> integ steigt -> ENTLADEN.  Einspeisung -> fehler<0 -> integ faellt -> LADEN.
  //   gridPowerFilt statt gridPowerW: der PI-Regler regelt den MITTELWERT, jagt keine ms/s-Blinker mehr
  //   -> kein "Loecher treffen". (Der schnelle Feed-in-Schutz sitzt separat auf dem ROHEN Netz, s. 2a.)
  long gridF = (long)(gridPowerFilt >= 0.0f ? gridPowerFilt + 0.5f : gridPowerFilt - 0.5f);
  long fehler = TARGET_GRID_W - gridF;         // long -> kein int-Overflow/Vorzeichen-Flip
  ctrlFehler = fehler;                                          // Diagnose: Regelabweichung (vor Totband)

  // --- 2a. GEGENRICHTUNGS-ABBRUCH auf dem ROHEN Netz (ZWEI-SIGNAL-DESIGN, 2026-07-11) ---
  //   Der PI regelt gefiltert (Ruhe) - aber der Feed-in-Schutz MUSS schnell sein, darf NICHT dem Filter-Lag
  //   folgen (Extremtest: 8kW-Last weg -> grid_w sofort Einspeisung, grid_filt hing ~tau nach -> Retreat zu
  //   spaet -> -3335W eingespeist). Daher HIER das ROHE gridPowerW mit HOHER Schwelle RETREAT_RAW_W:
  //   kleine Blinker (<Schwelle) ignoriert (kein Saegezahn), ECHTER grosser Wegfall SOFORT gestoppt.
  //     entladen (integ>0) trotz Einspeisung > RETREAT_RAW_W  ODER  laden (integ<0) trotz Bezug > RETREAT_RAW_W
  //   -> SANFT proportional zurueck (RETREAT_GAIN * Roh-Ueberschuss) statt hart auf 0. Kleiner Blip -> winzige
  //   Ruecknahme (Zendure DROSSELT statt zu STOPPEN -> kein ~30-40s Wiederanlauf, kein Stottern); echter grosser
  //   Wegfall -> grosse Ruecknahme (feed-in bleibt schnell begrenzt). NICHT ueber 0 (keine Richtungsumkehr per
  //   Retreat -> Relais-Schonung). Filter nur TEIL-nachziehen (RETREAT_FILT_BLEND) statt harter Reset -> weniger
  //   PI-Nachdrall bei transienten Blips, aber genug Anti-Flatter bei echtem Wegfall. Dieser Zyklus per return.
  if ((ctrlInteg > 0.0f && gridPowerW >  RETREAT_RAW_W) ||
      (ctrlInteg < 0.0f && gridPowerW < -RETREAT_RAW_W)) {
    ctrlInteg -= RETREAT_GAIN * (float)gridPowerW;
    if (ctrlInteg < 0.0f && gridPowerW > 0) ctrlInteg = 0.0f;   // war Entladen -> nicht ins Laden kippen
    if (ctrlInteg > 0.0f && gridPowerW < 0) ctrlInteg = 0.0f;   // war Laden  -> nicht ins Entladen kippen
    gridPowerFilt += RETREAT_FILT_BLEND * ((float)gridPowerW - gridPowerFilt);
    cntRetreat++;
    if (ctrlInteg > (float)hiLimit) ctrlInteg = (float)hiLimit;
    if (ctrlInteg < (float)loLimit) ctrlInteg = (float)loLimit;
    return (long)(ctrlInteg >= 0.0f ? ctrlInteg + 0.5f : ctrlInteg - 0.5f);
  }

  ctrlDeadband = (fehler > -DEADBAND_W && fehler < DEADBAND_W);
  if (ctrlDeadband) fehler = 0;                                 // Totband -> kein Jagen auf Rauschen

  // --- 3. Integralschritt mit RICHTUNGS-ABHAENGIGER Slew (Relais-Schonung) ---
  //   Gleiche Richtung (Stellwert waechst) ODER Abbau FERN von 0 -> zuegig (SLEW_SAME_W). Aber Abbau/Umschalten
  //   NAHE 0 (Vorzeichenwechsel Laden<->Entladen -> ggf. internes Relais) -> LANGSAM (SLEW_REVERSAL_W): der
  //   Nulldurchgang wird sanft, wenige Schaltzyklen. Feed-in-Sicherheit bleibt schnell (harter RETREAT_RAW_W ueberschreibt).
  float delta = KI_REGLER * (float)fehler;
  bool sameDir = (delta >= 0.0f) == (ctrlInteg >= 0.0f);                        // Stellwert waechst in aktueller Richtung?
  bool reversalZone = !sameDir && (ctrlInteg <= (float)REVERSAL_BAND_W && ctrlInteg >= -(float)REVERSAL_BAND_W);
  long lim = reversalZone ? SLEW_REVERSAL_W : SLEW_SAME_W;    // Umschalt-Zone nahe 0 -> sanft, sonst zuegig
  ctrlSlew = (delta > lim || delta < -lim);                   // Diagnose: Slew-Begrenzung aktiv?
  if (delta >  lim) delta =  lim;
  if (delta < -lim) delta = -lim;
  ctrlDelta = (long)delta;                                     // Diagnose: tatsaechlicher Integralschritt
  ctrlInteg += delta;

  // --- 4. ANTI-WINDUP: Integrator auf [loLimit, hiLimit] klemmen (SoC-abhaengig, BEIDE Seiten) ---
  if (ctrlInteg > (float)hiLimit) ctrlInteg = (float)hiLimit;
  if (ctrlInteg < (float)loLimit) ctrlInteg = (float)loLimit;

  // --- 5. Stellgroesse = I-Anteil (+ optionaler P-Anteil), final geklemmt; korrektes Runden fuer +/- ---
  float u = ctrlInteg + KP_REGLER * (float)fehler;
  if (u > (float)hiLimit) u = (float)hiLimit;
  if (u < (float)loLimit) u = (float)loLimit;
  return (long)(u >= 0.0f ? u + 0.5f : u - 0.5f);
}

// Signierten Sollwert ALS ANTRAG an den Gate (regler/cmd/*), NICHT direkt ans Geraet. >0 ENTLADEN, <0 LADEN,
//   |w|<MODE_HYST_W IDLE. Der BROKER validiert (absolute Physik) + ist einziger Schreiber ans Geraet (Gate g).
//   MODE-HYSTERESE (idle-Zone [-40,+40]) + QoS0-Wiederholung jeden Takt + Burst auf idle: unveraendert.
//   INTERNER Level-1-Monitor: Endwert-Betrag gegen eigene Grenze -> Verletzung = idle beantragen (SAFE).
//   ACTUATE_ENABLE=false -> nur Debug-Topic, kein Antrag.
void publishSetpoint(long w) {
  cntPub++;
  int desired = (w >= MODE_HYST_W) ? 1 : (w <= -MODE_HYST_W ? 2 : 0);    // 1=Entladen 2=Laden 0=idle
  if (!ACTUATE_ENABLE) {                          // NICHT scharf -> nur Debug (signiert: +Entladen/-Laden)
    snprintf(buf, sizeof(buf), "%ld", w);
    mqtt.publish(T_SETPOINT_DBG, buf);
    ctrlActMode = desired;                        // Diagnose auch im Dry-Run sichtbar
    return;
  }
  // INTERNER Level-1-Monitor: |Endwert| ueber eigener Grenze -> Regler-interner Defekt -> idle beantragen.
  long mag = (w >= 0) ? w : -w;
  long lim = (w >= 0) ? (long)MON_DISCHARGE_MAX_W : (long)MON_CHARGE_MAX_W;   // Promotion -> signed-Vergleich
  if (mag > lim) { w = 0; desired = 0; cntMonTrip++; }
  bool changed = (desired != ctrlActMode);
  if (desired == 1) {                             // ENTLADEN (Antrag)
    mqtt.publish(T_CMD_ACMODE, "Output mode");
    if (changed) mqtt.publish(T_CMD_INLIMIT, "0");   // Gegenrichtung beim Wechsel sicher auf 0
    snprintf(buf, sizeof(buf), "%ld", w);
    mqtt.publish(T_CMD_OUTLIMIT, buf);
  } else if (desired == 2) {                      // LADEN (Antrag)
    mqtt.publish(T_CMD_ACMODE, "Input mode");
    if (changed) mqtt.publish(T_CMD_OUTLIMIT, "0");  // Gegenrichtung beim Wechsel sicher auf 0
    snprintf(buf, sizeof(buf), "%ld", -w);           // Lade-Betrag = |w|
    mqtt.publish(T_CMD_INLIMIT, buf);
  } else {                                        // IDLE: beide Richtungen 0
    mqtt.publish(T_CMD_OUTLIMIT, "0");
    mqtt.publish(T_CMD_INLIMIT, "0");
    if (changed) {                                // Uebergang aktiv -> idle: Burst (QoS0-robust, schneller sicher)
      for (int k = 0; k < 4; k++) { delay(30); mqtt.publish(T_CMD_OUTLIMIT, "0"); mqtt.publish(T_CMD_INLIMIT, "0"); }
    }
  }
  ctrlActMode = desired;
}

// Geraete-Backstops (broker-unabhaengige LETZTE Linie, greift auch wenn Regler+Broker tot):
//   gridReverse=Disallow (kein Feed-in bei Lastabfall) + minSoc (kein Tiefentladen). Periodisch nachsetzen.
void assertDeviceBackstops() {
  mqtt.publish(T_GRIDREV_SET, "Disallow backflow");
  snprintf(buf, sizeof(buf), "%ld", DEV_MINSOC);
  mqtt.publish(T_MINSOC_SET, buf);
}

void failSafeCheck() {
  if (lastTele != 0 && (millis() - lastTele > TELE_TIMEOUT_MS)) {
    Serial.println("FAIL-SAFE: Zendure-Telemetrie veraltet!");
    // Sicherer Zustand (Sollwert 0) wird bereits in computeSetpoint() Ebene 1b (ctrlTeleStale) erzwungen.
    // Hier spaeter: aktive Notification (Mail/Telegram) -> Gate (e), "Regler stumm" absichern.
  }
}

// ---- read-only Diagnose-API: GET /status -> JSON (unabhaengig von MQTT) -----
void handleHttpStatus() {
  WiFiClient client = httpServer.available();
  if (!client) return;
  unsigned long t0 = millis();
  while (!client.available() && millis() - t0 < 1000) delay(1);
  client.readStringUntil('\n');                                   // Request-Zeile (nur GET /status)
  while (client.available()) { String h = client.readStringUntil('\n'); if (h.length() <= 1) break; }  // Header weg

  long teleAge   = (lastTele == 0)    ? -1 : (long)((millis() - lastTele) / 1000UL);
  long socAge    = (lastSocMs == 0)   ? -1 : (long)((millis() - lastSocMs) / 1000UL);
  long shellyAge = (lastShellyMs == 0)? -1 : (long)((millis() - lastShellyMs) / 1000UL);
  char body[832];
  int n = snprintf(body, sizeof(body),
    "{\"fw\":\"%s\",\"build\":\"%s %s\",\"role\":\"regler\",\"actuate\":%s,"
    "\"uptime_s\":%lu,\"boot_count\":%lu,\"reset_reason\":%d,\"free_heap\":%u,\"heap_min\":%u,\"stack_min\":%u,"
    "\"eth_up\":%s,\"ip\":\"%s\",\"mac\":\"%s\","
    "\"mqtt_connected\":%s,\"grid_w\":%ld,\"grid_filt_w\":%ld,\"soc\":%ld,\"zout_w\":%ld,\"setpoint_w\":%ld,"
    "\"integ\":%ld,\"fehler_w\":%ld,\"delta_w\":%ld,\"slew_active\":%s,\"deadband_active\":%s,"
    "\"grid_stale\":%s,\"tele_stale\":%s,\"discharge_blocked\":%s,\"charge_blocked\":%s,\"soc_old\":%s,\"act_mode\":%d,\"saturated\":%s,"
    "\"tele_age_s\":%ld,\"soc_age_s\":%ld,\"shelly_ok\":%s,\"shelly_age_s\":%ld,"
    "\"pub_count\":%lu,\"tele_count\":%llu,\"shelly_fail\":%lu,\"shelly_retry\":%lu,\"mqtt_reconn\":%lu,\"retreat_count\":%lu,\"mon_trip_count\":%lu}",
    FW_VERSION, __DATE__, __TIME__, ACTUATE_ENABLE ? "true" : "false",
    (unsigned long)(esp_timer_get_time() / 1000000LL), (unsigned long)bootCount,
    (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
    (unsigned)uxTaskGetStackHighWaterMark(NULL),
    eth_up ? "true" : "false", ETH.localIP().toString().c_str(), ETH.macAddress().c_str(),
    mqtt.connected() ? "true" : "false", gridPowerW, (long)(gridPowerFilt >= 0.0f ? gridPowerFilt + 0.5f : gridPowerFilt - 0.5f), zSoc, zOutW, setpointW,
    (long)(ctrlInteg + 0.5f), ctrlFehler, ctrlDelta, ctrlSlew ? "true" : "false", ctrlDeadband ? "true" : "false",
    ctrlGridStale ? "true" : "false", ctrlTeleStale ? "true" : "false", ctrlBlocked ? "true" : "false", ctrlChargeBlocked ? "true" : "false", ctrlSocOld ? "true" : "false", ctrlActMode, (setpointW >= ZEN_MAX_W || setpointW <= -ZEN_CHARGE_MAX_W) ? "true" : "false",
    teleAge, socAge, shellyOk ? "true" : "false", shellyAge,
    (unsigned long)cntPub, (unsigned long long)cntTele, (unsigned long)cntShellyFail, (unsigned long)cntShellyRetry, (unsigned long)cntMqttReconn, (unsigned long)cntRetreat, (unsigned long)cntMonTrip);
  if (n < 0) n = 0; else if (n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;  // Content-Length nie > tatsaechlich gesendet

  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
               "Access-Control-Allow-Origin: *\r\nConnection: close\r\n");
  client.printf("Content-Length: %d\r\n\r\n", n);
  client.print(body);
  client.flush();
  delay(5);
  client.stop();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[Zendure-Regler] Start (W5500 via ETH.h)");

  // Persistenter Boot-/Reset-Zaehler (NVS, ueberlebt Power-off): +1 pro Start -> Resets erkennbar.
  { Preferences p; p.begin("diag", false); bootCount = p.getUInt("boot", 0) + 1; p.putUInt("boot", bootCount); p.end(); }
  Serial.printf("Boot #%lu\n", (unsigned long)bootCount);

  ethSetup();
  IPAddress bip; bip.fromString(SECRET_BROKER_HOST);
  mqtt.setServer(bip, BROKER_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setBufferSize(512);
  // Reconnect darf den Loop (Heartbeat!) nicht lange einfrieren: PubSubClient-Default
  // MQTT_SOCKET_TIMEOUT=15s wuerde bei ausbleibendem CONNACK ~15s pro Versuch blockieren
  // (2 zaehe Versuche ~ die beobachtete 27s-Heartbeat-Luecke). 4s -> Versuch scheitert
  // schnell, Loop bedient mqtt.loop()+Heartbeat weiter. Keepalive 10s -> tote Session
  // raeumt der Broker frueher ab (schnellerer sauberer Reconnect).
  mqtt.setSocketTimeout(4);
  mqtt.setKeepAlive(10);
  httpServer.begin();

  // Hardware-Watchdog ERST JETZT scharf (nach Netz-Setup). Core 3.x hat TWDT bereits init -> reconfigure.
  esp_task_wdt_config_t wdtCfg = { .timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdtCfg);
  esp_task_wdt_add(NULL);

  Serial.println("HTTP /status auf Port 80. HW-Watchdog aktiv.");
}

void loop() {
  esp_task_wdt_reset();   // HW-Watchdog fuettern (1x pro loop-Durchlauf)
  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();
  handleHttpStatus();

  unsigned long now = millis();
  if (now - lastPoll >= POLL_MS) {
    lastPoll = now; shellyOk = readShelly();
    if (shellyOk) {
      lastShellyMs = now;
      // EMA-Tiefpass auf das Netzsignal (bei ~1s Poll). alpha aus echtem dt -> robust gegen variable Poll-Zeit;
      //   dt >= tau (Luecke) -> alpha=1 = Sprung auf aktuellen Wert (Auto-Reinit, kein Nachlaufen alter Werte).
      if (!gridFiltInit || lastFiltMs == 0) { gridPowerFilt = (float)gridPowerW; gridFiltInit = true; }
      else {
        float a = (float)(now - lastFiltMs) / (GRID_FILT_TAU_S * 1000.0f);
        if (a > 1.0f) a = 1.0f;
        gridPowerFilt += a * ((float)gridPowerW - gridPowerFilt);
      }
      lastFiltMs = now;
    } else cntShellyFail++;
  }
  if (now - lastPub  >= PUBLISH_MS) { lastPub  = now; setpointW = computeSetpoint(); if (mqtt.connected()) publishSetpoint(setpointW); }
  if (now - lastBeat >= HEARTBEAT_MS){ lastBeat = now; if (mqtt.connected()) mqtt.publish(T_HEARTBEAT, "1"); }
  if (ACTUATE_ENABLE && now - lastBackstop >= BACKSTOP_MS){ lastBackstop = now; if (mqtt.connected()) assertDeviceBackstops(); }
  failSafeCheck();
}
