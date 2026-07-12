/*
 * Lokaler MQTT-BROKER  (ESP32-S3 + W5500)
 * ============================================================================
 * Board : ESP32-S3R8 + onboard W5500 (kabelgebundenes Ethernet, KEIN WLAN)
 * Rolle : zentraler, ANONYMER MQTT-Broker fuer Zendure + Regler. Feste IP.
 *
 * IP: laeuft per DHCP. Beim 1. Boot zeigt der Serial Monitor IP + MAC -> diese
 *     MAC im Router auf eine Wunsch-IP RESERVIEREN, dann diese IP in Regler
 *     (SECRET_BROKER_HOST) + Zendure-App eintragen. (Statisch im Code = Alternative,
 *     siehe auskommentiertes ETH.config() in setup().)
 *
 * !! CORE 3.x NOETIG !!  (esp32 by Espressif, Version 3.x)
 *   W5500 wird ueber das ESP32-native ETH.h (SPI) als lwIP-Interface betrieben
 *   (erstklassiger W5500-Support ab Core 3.x). Dadurch laufen WiFiServer/
 *   WiFiClient -- und damit PicoMQTT -- TRANSPARENT ueber Ethernet.
 *   -> Die WIZnet-"Ethernet"-Lib wird NICHT mehr benutzt (auf ESP32 ist deren
 *      EthernetServer abstrakt/nicht instanziierbar). ETH.h ist Teil des Cores.
 *   Steuer-Geraete sind headless -> Core 3.x unproblematisch (anders als die
 *   Display-Waechter, die auf 2.0.17 bleiben).
 *
 * BIBLIOTHEKEN: PicoMQTT (LGPL, nur eingebunden). ETH/SPI/WiFi = im Core.
 *
 * ⚠️ ETH.begin()-Signatur variiert leicht je 3.x-Version. Falls der Compile/Link
 *    hier klemmt: zuerst das mitgelieferte Beispiel File -> Beispiele -> ETH ->
 *    "ETH_W5500" (o.ae.) auf deinem Board zum Laufen bringen (IP per Ethernet),
 *    dann diese begin()-Zeile exakt daran angleichen.
 * ============================================================================
 */
#include <ETH.h>
#include <SPI.h>
#include <WiFi.h>        // WiFiServer/WiFiClient + Network-Events -- laufen ueber ETH
#include <PicoMQTT.h>
#include "esp_system.h"    // esp_reset_reason() fuer /status
#include "esp_task_wdt.h"  // HW-Watchdog (loop-Hang -> Reboot)
#include "esp_timer.h"     // esp_timer_get_time() -> ueberlaufsichere uptime (64-bit us)
#include <Preferences.h>   // persistenter boot_count (NVS, ueberlebt Power-off)
#include "gate_types.h"     // struct SentRec (Watchdog) -- MUSS per Include (vor Auto-Prototypen), s. Header

// --- Core-Schutz: braucht esp32 core 3.x (W5500 via ETH.h). Falscher Core = Compile-Fehler. ---
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "Falscher Core! Broker braucht 'esp32 by Espressif' >= 3.x (Tools -> Board -> Boards Manager). Die Waechter laufen auf 2.0.17 - hier 3.x waehlen!"
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

// ---- Firmware-Version (fuer /status + Baseline-/Versions-Check) -------------
#define FW_VERSION  "broker-1.12.0"  // 1.12.0: Enforcer-Margin 10s->15s (Defense-in-Depth ggn. Reconnect-Fehltrips; Root-Fix im Regler 1.19.0). Basis 1.11.0. 1.11.0: Gate g Increment 2 - OVERRIDE-WATCHDOG: fremde Direkt-Publishes auf Zendure/.../{out,in}Limit/set -> sofort Safe(0); loopback-sicher via devPublish/wdCheck; /status bypass_trip_count
SET_LOOP_TASK_STACK_SIZE(12288);     // loop-Task-Stack auf 12 KB anheben (Default 8192); Arduino-ESP32-Makro
uint32_t bootCount = 0;              // persistenter Reset-Zaehler (NVS) -> erkennt Resets ueber Neustarts hinweg
const uint32_t WDT_TIMEOUT_MS = 12000;  // HW-Watchdog: loop-Hang laenger -> Reboot. 12s faengt auch etwas
                                        //   laengere Stoerungen ab (reset_reason wird 6=TASK_WDT)

// PicoMQTT-Server mit Diagnose-Zaehlern (verbundene Clients, durchgereichte Nachrichten) fuer /status.
class DiagBroker : public PicoMQTT::Server {
  public:
    int      clients    = 0;    // momentane Client-Zahl (klein) -> int reicht HW-neutral
    int      maxClients = 0;    // Spitze (klein)
    uint64_t msgCount   = 0;    // ALLE durchgereichten Nachrichten (hohe Rate) -> 64-bit gegen Ueberlauf
  protected:
    void on_connected(const char *)    override { clients++; if (clients > maxClients) maxClients = clients; }
    void on_disconnected(const char *) override { if (clients > 0) clients--; }
};
DiagBroker mqtt;            // Default-Transport = WiFiServer -> laeuft transparent ueber ETH

WiFiServer    httpServer(80);        // read-only /status-API (unabhaengig von MQTT)
unsigned long brokerLastMsgMs = 0;   // Zeitpunkt der letzten MQTT-Nachricht (Diagnose)
volatile unsigned long ethDownCount = 0;  // volatile+32-bit: im Event-Task geschrieben, im loop-Task (/status) gelesen
static volatile bool eth_up = false;      // volatile: wird im Netzwerk-Event-Task geschrieben, im loop-Task gelesen

// ---- BROKER-ENFORCER (L2, Chunk 3) ----------------------------------------------------------
//   Empirie 2026-07-10: Zendure HAELT letzten Sollwert unbegrenzt (a1+a2). Faellt der Regler aus
//   (Heartbeat weg), entlaedt ODER laedt die Zendure sonst stur weiter. -> Broker sendet dann SELBST aktiv
//   outputLimit=0 UND inputLimit=0 (WIEDERHOLT gegen QoS0-Verlust). Fail-safe by design: nur Richtung sicher (idle).
//   ⚠️ Seit Regler bidirektional (Chunk 4) MUSS der Enforcer BEIDE Richtungen nullen (sonst laeuft ein
//      haengendes Laden weiter). idle = out=0 + in=0, unabhaengig vom acMode.
//   ⚠️ ENFORCER_ENABLE=false = Default. Erst mit Schliessen der Schleife (zusammen mit Regler-ACTUATE_ENABLE) an.
#include "arduino_secrets.h"                       // ZEN_DEV (Zendure deviceId) -- lokal, .gitignore't (nie einchecken!)
const char* T_HEARTBEAT    = "regler/heartbeat";
const char* T_OUTLIMIT_SET = "Zendure/number/" ZEN_DEV "/outputLimit/set";
const char* T_INLIMIT_SET  = "Zendure/number/" ZEN_DEV "/inputLimit/set";   // Chunk 4: Laden ebenfalls stoppen
const char* T_ACMODE_SET   = "Zendure/select/" ZEN_DEV "/acMode/set";
// ---- GATE g (Gatekeeper, 2026-07-11): Broker validiert die Regler-ANTRAEGE (regler/cmd/*) gegen ABSOLUTE
//   Geraete-Physik und ist EINZIGER Schreiber ans Geraet. Unabhaengig von der Regler-Config (regler-update-fest).
//   Grenzen als uint (dokumentiert nicht-negativ; im Vergleich Promotion nach 32-bit signed int -> kein Wrap).
//   Bracket-Regel (s. 02_gate-g §8c): DEV_MAX_W >= Regler-ZEN_MAX_W ; MON_SOC_FLOOR <= Regler-SOC_STOP_DISCHARGE, NIE < 10%.
const bool     GATE_ENABLE   = true;   // MASTER: Gatekeeper aktiv? (Regler routet auf regler/cmd/* -> braucht dies zum Aktuieren!)
const uint16_t DEV_MAX_W     = 2400;   // absolute Leistungsgrenze [W]
const uint8_t  MON_SOC_FLOOR = 15;     // Backstop-Entlade-Floor [%] (Entladen darunter -> Safe)
const char* T_CMD_OUTLIMIT = "regler/cmd/outputLimit";
const char* T_CMD_INLIMIT  = "regler/cmd/inputLimit";
const char* T_CMD_ACMODE   = "regler/cmd/acMode";
const char* T_SOC          = "Zendure/sensor/" ZEN_DEV "/electricLevel";   // fuer I4 (SoC-Floor)
const bool          ENFORCER_ENABLE     = true;   // MASTER: Enforcer scharf. 2026-07-10 Nachtlauf (L2 fuer unbeaufsichtigt scharfen Regler)
const unsigned long ENFORCER_TIMEOUT_MS = 15000;  // Heartbeat laenger weg -> Regler gilt als tot. 2026-07-12 10s->15s: Defense-in-Depth, toleriert einen kurzen sauberen Reconnect (~2s Luecke) ohne Fehltrip; bleibt eng genug fuer echten Regler-Tod (Geraet haelt Sollwert, SoC-Floor+Backstops als tiefere Ebene). Root-Fix der langen Luecken sitzt im Regler (1.19.0 Reconnect-Haertung).
const unsigned long ENFORCER_REPUB_MS   = 1000;   // solange still: out=0 + in=0 alle 1s neu senden
unsigned long lastHeartbeatMs = 0;                // letzter regler/heartbeat (im subscribe-Callback gesetzt)
unsigned long lastEnforceMs   = 0;                // letztes Enforcer-Safe-Kommando
uint32_t      enforceCount    = 0;                // wie oft eingegriffen (Diagnose)
bool          enforcerActive  = false;            // aktueller Zustand (Diagnose)
// Gate-Zustand:
long          gateSoc      = -1;                  // letzter electricLevel (%), -1 = unbekannt (I4 nur wenn >=0)
uint32_t      monTripCount = 0;                   // wie oft eine Invariante verletzt -> Safe erzwungen
int           lastTripInv  = 0;                   // Diagnose: 1=Range 2=Negativ 4=SoC-Floor 8=Parse 16=acMode
// struct SentRec liegt jetzt in gate_types.h (per #include oben) -> Arduino-Auto-Prototype kennt den Typ.

void onNetEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_ETH_GOT_IP) {
    eth_up = true;
    Serial.print("ETH IP: ");  Serial.print(ETH.localIP());
    Serial.print("   MAC: ");  Serial.println(ETH.macAddress());   // <- diese MAC im Router reservieren
  } else if (event == ARDUINO_EVENT_ETH_DISCONNECTED || event == ARDUINO_EVENT_ETH_STOP) {
    eth_up = false;
    ethDownCount++;
    Serial.println("ETH down");
  }
}

// ---- GATE g / OVERRIDE-WATCHDOG (Increment 2): faengt Direkt-Publishes am Gate VORBEI ab ----
//   Ein fehlerhafter Regler oder Fremdgeraet koennte direkt auf Zendure/.../set publizieren (Bypass des Gates).
//   Der Broker abonniert diese Topics ('#') und ueberschreibt sofort mit Safe(0), WENN der Wert NICHT der ist, den
//   er gerade SELBST gesendet hat. LOOPBACK-SICHER (unabhaengig davon, ob PicoMQTT eigene Publishes zurueckspiegelt):
//   jeder eigene Geraete-Write geht durch devPublish() und merkt sich {Wert,Zeit}; wdCheck vergleicht dagegen.
const bool          WATCHDOG_ENABLE = true;   // Override-Watchdog aktiv?
const unsigned long WD_MATCH_MS     = 800;    // Fenster: Loopback des eigenen Publishes vs. fremder Publish
SentRec  wdOut = { "", 0 };                   // letzter eigener outputLimit/set-Wert (SentRec s. oben, vor onNetEvent)
SentRec  wdIn  = { "", 0 };                   // letzter eigener inputLimit/set-Wert
uint32_t bypassTripCount = 0;                 // wie oft ein Fremd-/Bypass-Publish ueberschrieben wurde

// Geraete-Write MIT Merken (fuer den Watchdog): erst {Wert,Zeit} sichern, DANN publishen (reentrant-sicher).
void devPublish(SentRec &rec, const char* topic, const char* payload) {
  strncpy(rec.pl, payload, sizeof(rec.pl) - 1); rec.pl[sizeof(rec.pl) - 1] = '\0';
  rec.t = millis();
  mqtt.publish(topic, payload);
}
// Watchdog: erscheint auf einem Geraete-/set-Topic ein Wert, den wir NICHT gerade selbst gesendet haben
//   (Bypass am Gate vorbei) -> sofort Safe(0) erzwingen (und als eigen merken).
void wdCheck(SentRec &rec, const char* topic, const char* payload) {
  if (strcmp(payload, rec.pl) == 0 && (millis() - rec.t) < WD_MATCH_MS) return;   // eigen (Loopback) -> ok
  bypassTripCount++; lastTripInv = 32;                                            // fremd -> Bypass -> Safe
  devPublish(rec, topic, "0");
}

// ---- GATE g: Validierung der Regler-Antraege + Weiterreichen ans Geraet (oder Safe) ----
//   Prueft I8(Parse)/I2(negativ)/I1(Range)/I4(SoC-Floor); reicht bei OK den ORIGINAL-Payload weiter, sonst 0.
//   uint-Grenzen -> Integer-Promotion nach 32-bit signed int im Vergleich -> KEIN Unsigned-Wrap.
//   Alle Geraete-Writes ueber devPublish() (Watchdog-Merken).
void gateForwardOutput(const char* p) {                 // ENTLADEN-Antrag
  char* end = nullptr; long v = strtol(p, &end, 10);
  int inv = 0;
  if (end == p || *end != '\0')                                   inv = 8;   // nicht rein-numerisch
  else if (v < 0)                                                 inv = 2;   // I2: nie negativ
  else if (v > (long)DEV_MAX_W)                                   inv = 1;   // I1: nie ueber Geraete-Physik
  else if (v > 0 && gateSoc >= 0 && gateSoc <= (int)MON_SOC_FLOOR) inv = 4;  // I4: kein Entladen unter Floor
  if (inv) { monTripCount++; lastTripInv = inv; devPublish(wdOut, T_OUTLIMIT_SET, "0"); return; }
  devPublish(wdOut, T_OUTLIMIT_SET, p);                  // gueltig -> Original ans Geraet
}
void gateForwardInput(const char* p) {                  // LADEN-Antrag (kein SoC-Floor: Laden kann nicht tiefentladen)
  char* end = nullptr; long v = strtol(p, &end, 10);
  int inv = 0;
  if (end == p || *end != '\0') inv = 8;
  else if (v < 0)               inv = 2;
  else if (v > (long)DEV_MAX_W) inv = 1;
  if (inv) { monTripCount++; lastTripInv = inv; devPublish(wdIn, T_INLIMIT_SET, "0"); return; }
  devPublish(wdIn, T_INLIMIT_SET, p);
}
void gateForwardAcMode(const char* p) {                 // nur bekannte Modi weiterreichen
  if (strcmp(p, "Output mode") == 0 || strcmp(p, "Input mode") == 0) mqtt.publish(T_ACMODE_SET, p);
  else { monTripCount++; lastTripInv = 16; }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[MQTT-Broker] Start (W5500 via ETH.h)");

  // Persistenter Boot-/Reset-Zaehler (NVS, ueberlebt Power-off): +1 pro Start -> Resets erkennbar.
  { Preferences p; p.begin("diag", false); bootCount = p.getUInt("boot", 0) + 1; p.putUInt("boot", bootCount); p.end(); }
  Serial.printf("Boot #%lu\n", (unsigned long)bootCount);

  Network.onEvent(onNetEvent);
  SPI.begin(PIN_ETH_SCK, PIN_ETH_MISO, PIN_ETH_MOSI);
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, PIN_ETH_CS, PIN_ETH_IRQ, PIN_ETH_RST, SPI);   // DHCP
  // Alternative statt DHCP (feste IP im Code):
  //   ETH.config(IPAddress(192,168,188,110), IPAddress(192,168,188,1), IPAddress(255,255,255,0), IPAddress(192,168,188,1));

  unsigned long t0 = millis();                       // kurz auf Link/IP warten
  while (!eth_up && millis() - t0 < 8000) delay(50);

  // Optional: alles mitloggen (Diagnose)
  mqtt.subscribe("#", [](const char* topic, const char* payload) {
    mqtt.msgCount++;
    brokerLastMsgMs = millis();
    if      (strcmp(topic, T_HEARTBEAT) == 0)                   lastHeartbeatMs = millis();  // Regler lebt -> Enforcer ruht
    else if (strcmp(topic, T_SOC) == 0)                         gateSoc = atol(payload);     // Gate I4 (SoC-Floor)
    else if (GATE_ENABLE && strcmp(topic, T_CMD_OUTLIMIT) == 0) gateForwardOutput(payload);  // Antrag -> validieren -> Geraet
    else if (GATE_ENABLE && strcmp(topic, T_CMD_INLIMIT)  == 0) gateForwardInput(payload);
    else if (GATE_ENABLE && strcmp(topic, T_CMD_ACMODE)   == 0) gateForwardAcMode(payload);
    else if (WATCHDOG_ENABLE && strcmp(topic, T_OUTLIMIT_SET) == 0) wdCheck(wdOut, T_OUTLIMIT_SET, payload);  // Bypass-Schutz
    else if (WATCHDOG_ENABLE && strcmp(topic, T_INLIMIT_SET)  == 0) wdCheck(wdIn,  T_INLIMIT_SET,  payload);
    // Serial.printf("MQTT  %s = %s\n", topic, payload);   // Debug bei Bedarf (sonst zu viel Traffic)
  });

  mqtt.begin();
  httpServer.begin();

  // Hardware-Watchdog ERST JETZT scharf schalten (nach dem Netz-Setup; ETH.begin wartet bis zu 8s).
  //   Core 3.x initialisiert den TWDT bereits -> reconfigure (nicht init), dann loop-Task ueberwachen.
  esp_task_wdt_config_t wdtCfg = { .timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdtCfg);
  esp_task_wdt_add(NULL);

  Serial.println("Broker gestartet (Port 1883, anonym). HTTP /status auf Port 80. HW-Watchdog aktiv.");
}

// ---- read-only Diagnose-API: GET /status -> JSON (unabhaengig von MQTT) -----
void handleHttpStatus() {
  WiFiClient client = httpServer.available();
  if (!client) return;
  unsigned long t0 = millis();
  while (!client.available() && millis() - t0 < 1000) delay(1);
  client.readStringUntil('\n');                                   // Request-Zeile (nur GET /status)
  while (client.available()) { String h = client.readStringUntil('\n'); if (h.length() <= 1) break; }  // Header weg

  long lastMsgAge = (brokerLastMsgMs == 0) ? -1 : (long)((millis() - brokerLastMsgMs) / 1000UL);
  long hbAge      = (lastHeartbeatMs == 0) ? -1 : (long)((millis() - lastHeartbeatMs) / 1000UL);
  char body[768];
  int n = snprintf(body, sizeof(body),
    "{\"fw\":\"%s\",\"build\":\"%s %s\",\"role\":\"mqtt-broker\","
    "\"uptime_s\":%lu,\"boot_count\":%lu,\"reset_reason\":%d,\"free_heap\":%u,\"heap_min\":%u,\"stack_min\":%u,"
    "\"eth_up\":%s,\"ip\":\"%s\",\"mac\":\"%s\",\"eth_down_count\":%lu,"
    "\"clients_connected\":%d,\"max_clients\":%d,\"msg_count\":%llu,\"last_msg_age_s\":%ld,"
    "\"heartbeat_age_s\":%ld,\"enforcer_active\":%s,\"enforce_count\":%lu,"
    "\"gate_enabled\":%s,\"monitor_trip_count\":%lu,\"gate_soc\":%ld,\"last_trip_inv\":%d,\"bypass_trip_count\":%lu}",
    FW_VERSION, __DATE__, __TIME__,
    (unsigned long)(esp_timer_get_time() / 1000000LL), (unsigned long)bootCount,
    (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
    (unsigned)uxTaskGetStackHighWaterMark(NULL),
    eth_up ? "true" : "false", ETH.localIP().toString().c_str(), ETH.macAddress().c_str(), (unsigned long)ethDownCount,
    mqtt.clients, mqtt.maxClients, (unsigned long long)mqtt.msgCount, lastMsgAge,
    hbAge, enforcerActive ? "true" : "false", (unsigned long)enforceCount,
    GATE_ENABLE ? "true" : "false", (unsigned long)monTripCount, (long)gateSoc, lastTripInv, (unsigned long)bypassTripCount);
  if (n < 0) n = 0; else if (n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;  // Content-Length nie > tatsaechlich gesendet

  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
               "Access-Control-Allow-Origin: *\r\nConnection: close\r\n");
  client.printf("Content-Length: %d\r\n\r\n", n);
  client.print(body);
  client.flush();
  delay(5);
  client.stop();
}

void loop() {
  esp_task_wdt_reset();   // HW-Watchdog fuettern (1x pro loop-Durchlauf)
  mqtt.loop();
  handleHttpStatus();

  // Broker-Enforcer (L2): Regler-Heartbeat weg -> aktiv out=0 + in=0 WIEDERHOLT an die Zendure (idle).
  if (ENFORCER_ENABLE) {
    unsigned long now = millis();
    enforcerActive = (lastHeartbeatMs == 0) || (now - lastHeartbeatMs > ENFORCER_TIMEOUT_MS);
    if (enforcerActive && (now - lastEnforceMs >= ENFORCER_REPUB_MS)) {
      lastEnforceMs = now;
      devPublish(wdOut, T_OUTLIMIT_SET, "0");      // SAFE: Entladen aus (via devPublish -> Watchdog kennt eigenen Send)
      devPublish(wdIn,  T_INLIMIT_SET,  "0");      // SAFE: Laden aus -> Geraet idle, acMode-unabhaengig
      enforceCount++;
    }
  }
}
