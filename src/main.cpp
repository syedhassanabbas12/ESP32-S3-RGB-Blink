// ESP32-S3 IoT node — control LEDs + relays over MQTT (TLS)
//
// WiFi and the MQTT broker are provisioned at RUNTIME via a phone captive
// portal (WiFiManager) and stored in flash (NVS). Nothing is compiled in, so
// you can move networks or brokers without reflashing.
//
//   First boot / saved WiFi fails  -> device becomes a WiFi access point
//     SSID:     "HomeControl-Setup"
//     password: "homecontrol"
//   Connect your phone to it; a setup page opens where you enter your WiFi
//   plus the MQTT host / port / username / password / device id.
//   Hold the BOOT button for ~3s at any time to re-open that setup portal.
//
// Topic layout (device id is set in the portal):
//   home/<id>/status          -> "online" / "offline"   (retained, Last-Will)
//   home/<id>/<ch>/set        -> "on" / "off" / "toggle" (app  -> device)
//   home/<id>/<ch>/state      -> "on" / "off"            (device -> app, retained)
//
// Watch it over USB serial (115200 baud):
//   ~/.platformio/penv/bin/pio device monitor -b 115200

#include <WiFi.h>
#include <WiFiClientSecure.h>   // TLS transport for MQTT (HiveMQ Cloud :8883)
#include <WiFiManager.h>        // tzapu/WiFiManager — captive-portal provisioning
#include <Preferences.h>        // NVS key/value store for saved settings
#include <PubSubClient.h>
#include <esp_system.h>         // esp_reset_reason()
#include <stdarg.h>             // va_list for logf()

// =============== runtime config (loaded from flash / set in portal) ===========
// Defaults are placeholders; the broker host MUST be set via the portal before
// anything can connect. HiveMQ Cloud uses TLS on port 8883.
char     DEVICE_ID[32] = "dev1";   // unique per board
char     MQTT_HOST[80] = "";       // e.g. abcd1234.s1.eu.hivemq.cloud
uint16_t MQTT_PORT     = 8883;
char     MQTT_USER[48] = "";
char     MQTT_PASS[64] = "";
// =============================================================================

const uint8_t BOOT_BTN = 0;   // GPIO0 = BOOT button; hold to (re)open setup portal

Preferences  prefs;
WiFiClientSecure net;         // TLS socket (was plain WiFiClient)
PubSubClient mqtt(net);

static bool g_paramsSaved = false;   // set by the portal's save callback

// A "channel" = one controllable output.
//   activeLow = false -> pin HIGH energizes the load (LEDs driven directly)
//   activeLow = true  -> pin LOW  energizes the load (typical 5V relay boards)
struct Channel {
  const char *name;
  uint8_t pin;
  bool activeLow;
  bool state;
};

Channel channels[] = {
  { "red",    4,  false, false },
  { "green",  5,  false, false },
  { "blue",   6,  false, false },
  { "relay1", 15, true,  false },
  { "relay2", 16, true,  false },
  { "relay3", 17, true,  false },
  { "relay4", 18, true,  false },
};
const size_t NUM_CH = sizeof(channels) / sizeof(channels[0]);

// ---- tiny logger: prints "[seconds.millis] <message>" so every line is
// timestamped and greppable. No floats (some toolchains don't print %f). ----
void logf(const char *fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  uint32_t ms = millis();
  Serial.printf("[%6lu.%03lu] %s\n", ms / 1000, ms % 1000, buf);
}

// Human-readable version of PubSubClient's numeric connection state.
const char *mqttStateStr(int s) {
  switch (s) {
    case -4: return "timeout";
    case -3: return "connection lost";
    case -2: return "connect failed (bad host/port, TLS, or wrong network?)";
    case -1: return "disconnected";
    case  0: return "connected";
    case  1: return "bad protocol";
    case  2: return "bad client id";
    case  3: return "server unavailable";
    case  4: return "bad credentials (check MQTT username/password)";
    case  5: return "not authorized";
    default: return "unknown";
  }
}

// Why did the chip (re)boot? Handy for spotting crashes/brownouts.
const char *resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic / exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout (power dip)";
    default:                return "unknown";
  }
}

// -------------------- persisted settings (NVS) --------------------
void loadConfig() {
  prefs.begin("homecfg", true);   // read-only
  strlcpy(MQTT_HOST, prefs.getString("host", MQTT_HOST).c_str(), sizeof(MQTT_HOST));
  strlcpy(MQTT_USER, prefs.getString("user", MQTT_USER).c_str(), sizeof(MQTT_USER));
  strlcpy(MQTT_PASS, prefs.getString("pass", MQTT_PASS).c_str(), sizeof(MQTT_PASS));
  strlcpy(DEVICE_ID, prefs.getString("devid", DEVICE_ID).c_str(), sizeof(DEVICE_ID));
  MQTT_PORT = prefs.getUShort("port", MQTT_PORT);
  prefs.end();
}

void saveConfig() {
  prefs.begin("homecfg", false);  // read-write
  prefs.putString("host", MQTT_HOST);
  prefs.putString("user", MQTT_USER);
  prefs.putString("pass", MQTT_PASS);
  prefs.putString("devid", DEVICE_ID);
  prefs.putUShort("port", MQTT_PORT);
  prefs.end();
}

// -------------------- WiFi + broker provisioning --------------------
// Blocks until WiFi is connected. Opens the captive portal when there are no
// saved WiFi credentials, when they fail, or when forcePortal is requested.
void runProvisioning(bool forcePortal) {
  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(180);   // abandon an idle portal after 3 min

  char portStr[8];
  snprintf(portStr, sizeof(portStr), "%u", MQTT_PORT);

  // Extra fields rendered on the portal, seeded with the current values.
  WiFiManagerParameter pHost("host", "MQTT host (e.g. xxx.hivemq.cloud)", MQTT_HOST, sizeof(MQTT_HOST) - 1);
  WiFiManagerParameter pPort("port", "MQTT port (HiveMQ TLS = 8883)",     portStr,   sizeof(portStr) - 1);
  WiFiManagerParameter pUser("user", "MQTT username",                     MQTT_USER, sizeof(MQTT_USER) - 1);
  WiFiManagerParameter pPass("pass", "MQTT password",                     MQTT_PASS, sizeof(MQTT_PASS) - 1);
  WiFiManagerParameter pDev ("devid","Device ID (unique per board)",      DEVICE_ID, sizeof(DEVICE_ID) - 1);
  wm.addParameter(&pHost);
  wm.addParameter(&pPort);
  wm.addParameter(&pUser);
  wm.addParameter(&pPass);
  wm.addParameter(&pDev);

  g_paramsSaved = false;
  wm.setSaveParamsCallback([]() { g_paramsSaved = true; });

  logf("WiFi: setup portal -> connect phone to AP \"HomeControl-Setup\" (pass \"homecontrol\")");
  bool ok = forcePortal
              ? wm.startConfigPortal("HomeControl-Setup", "homecontrol")
              : wm.autoConnect("HomeControl-Setup", "homecontrol");

  if (g_paramsSaved) {   // user submitted the form -> persist the new settings
    strlcpy(MQTT_HOST, pHost.getValue(), sizeof(MQTT_HOST));
    strlcpy(MQTT_USER, pUser.getValue(), sizeof(MQTT_USER));
    strlcpy(MQTT_PASS, pPass.getValue(), sizeof(MQTT_PASS));
    strlcpy(DEVICE_ID, pDev.getValue(),  sizeof(DEVICE_ID));
    MQTT_PORT = (uint16_t) atoi(pPort.getValue());
    saveConfig();
    logf("CFG:  saved  host=%s port=%u user=%s devid=%s", MQTT_HOST, MQTT_PORT, MQTT_USER, DEVICE_ID);
  }

  if (!ok || WiFi.status() != WL_CONNECTED) {
    logf("WiFi: portal closed without a connection — restarting");
    delay(1000);
    ESP.restart();
  }
  logf("WiFi: connected  ip=%s  rssi=%d dBm  mac=%s",
       WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.macAddress().c_str());
}

void publishState(Channel &c) {
  char t[80];
  snprintf(t, sizeof(t), "home/%s/%s/state", DEVICE_ID, c.name);
  mqtt.publish(t, c.state ? "on" : "off", true);   // retained: new subscribers get last value
  logf("MQTT: tx  %s = %s", t, c.state ? "on" : "off");
}

// Drive the pin to match c.state, honoring activeLow (relays energize on LOW).
void writePin(const Channel &c) {
  digitalWrite(c.pin, (c.state ^ c.activeLow) ? HIGH : LOW);
}

void applyChannel(Channel &c, bool on) {
  c.state = on;
  writePin(c);
  logf("CH:   %s (gpio %u) -> %s", c.name, c.pin, on ? "ON" : "off");
  publishState(c);
}

// Called by PubSubClient whenever a subscribed message arrives.
void onMessage(char *topic, byte *payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();

  logf("MQTT: rx  %s = \"%s\"", topic, msg.c_str());   // log every message that arrives

  String t = topic;
  for (size_t i = 0; i < NUM_CH; i++) {
    String setTopic = String("home/") + DEVICE_ID + "/" + channels[i].name + "/set";
    if (t == setTopic) {
      if      (msg == "on")     applyChannel(channels[i], true);
      else if (msg == "off")    applyChannel(channels[i], false);
      else if (msg == "toggle") applyChannel(channels[i], !channels[i].state);
      else    logf("      ignored: \"%s\" is not on/off/toggle", msg.c_str());
      return;
    }
  }
  logf("      ignored: no channel matches this topic");
}

void connectMQTT() {
  if (!MQTT_HOST[0]) {
    logf("MQTT: no broker configured — hold BOOT ~3s to open the setup portal");
    delay(3000);
    return;
  }
  char statusTopic[80];
  snprintf(statusTopic, sizeof(statusTopic), "home/%s/status", DEVICE_ID);

  while (!mqtt.connected()) {
    logf("MQTT: connecting to %s:%u (TLS) as \"%s\" ...", MQTT_HOST, MQTT_PORT, DEVICE_ID);
    // Last-Will: if we drop off the network, the broker publishes "offline" for us.
    bool ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                           statusTopic, 0, true, "offline");
    if (ok) {
      logf("MQTT: connected");
      mqtt.publish(statusTopic, "online", true);          // retained
      char sub[80];
      snprintf(sub, sizeof(sub), "home/%s/+/set", DEVICE_ID);
      mqtt.subscribe(sub);                                // "+" = any channel
      logf("MQTT: subscribed to %s", sub);
      for (size_t i = 0; i < NUM_CH; i++) publishState(channels[i]);  // sync app on connect
    } else {
      logf("MQTT: connect failed rc=%d (%s), retry in 3s",
           mqtt.state(), mqttStateStr(mqtt.state()));
      delay(3000);
      return;   // back to loop() so WiFi/button stay serviced between attempts
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  logf("==== ESP32-S3 IoT node booting ====");
  logf("chip      : %s rev%d, %d core(s)",
       ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  logf("flash     : %u MB", ESP.getFlashChipSize() / (1024 * 1024));
  logf("free heap : %u bytes", ESP.getFreeHeap());
  logf("reset     : %s", resetReasonStr());

  pinMode(BOOT_BTN, INPUT_PULLUP);

  for (size_t i = 0; i < NUM_CH; i++) {
    pinMode(channels[i].pin, OUTPUT);
    channels[i].state = false;
    writePin(channels[i]);   // drive to OFF (active-low relays stay off = pin HIGH)
    logf("CH:   %s on gpio %u -> off%s", channels[i].name, channels[i].pin,
         channels[i].activeLow ? " (active-low)" : "");
  }

  loadConfig();
  logf("CFG:  host=%s port=%u user=%s devid=%s",
       MQTT_HOST[0] ? MQTT_HOST : "(unset)", MQTT_PORT, MQTT_USER, DEVICE_ID);

  // Hold BOOT at power-up to force the setup portal even if WiFi is saved.
  bool force = (digitalRead(BOOT_BTN) == LOW);
  if (force) logf("BTN:  BOOT held at boot — opening setup portal");
  runProvisioning(force);

  // A device with no broker can't do anything useful, so open the setup portal
  // automatically (no button required) and keep reopening it until the broker
  // details are entered and saved. This also covers the case where WiFi was
  // already saved (from earlier firmware) and the portal would otherwise be
  // skipped, leaving no chance to enter the MQTT settings.
  while (!MQTT_HOST[0]) {
    logf("CFG:  no broker configured — opening setup portal to enter MQTT details");
    runProvisioning(true);
  }

  net.setInsecure();          // skip TLS cert validation (simplest; fine for a home broker)
  mqtt.setBufferSize(512);    // roomier buffer for TLS records
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
}

void loop() {
  // Serial escape hatch — reliable even if the BOOT button isn't wired on this
  // board. Type "portal" to (re)open setup, or "reset" to wipe all settings.
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "portal") {
      logf("CMD:  opening setup portal on request");
      runProvisioning(true);
      net.setInsecure();
      mqtt.setServer(MQTT_HOST, MQTT_PORT);
    } else if (cmd == "reset") {
      logf("CMD:  wiping saved settings + WiFi, restarting");
      prefs.begin("homecfg", false); prefs.clear(); prefs.end();
      WiFiManager wm; wm.resetSettings();
      delay(500); ESP.restart();
    }
  }

  // Keep WiFi up without reopening the portal on a transient drop.
  if (WiFi.status() != WL_CONNECTED) {
    logf("WiFi: connection lost, reconnecting");
    WiFi.reconnect();
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(300);
    if (WiFi.status() != WL_CONNECTED) { logf("WiFi: still down, will retry"); return; }
    logf("WiFi: reconnected ip=%s", WiFi.localIP().toString().c_str());
  }

  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();   // must run often — services incoming/outgoing MQTT

  // Hold BOOT for ~3s during normal operation to re-open the setup portal.
  static uint32_t btnDownAt = 0;
  if (digitalRead(BOOT_BTN) == LOW) {
    if (btnDownAt == 0) btnDownAt = millis();
    else if (millis() - btnDownAt > 3000) {
      logf("BTN:  BOOT held — reopening setup portal");
      runProvisioning(true);
      net.setInsecure();
      mqtt.setServer(MQTT_HOST, MQTT_PORT);
      btnDownAt = 0;
    }
  } else {
    btnDownAt = 0;
  }

  // Heartbeat: every 30s prove we're alive and show link/memory health.
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 30000) {
    lastBeat = millis();
    logf("alive  uptime=%lus  heap=%u  rssi=%d dBm",
         millis() / 1000, ESP.getFreeHeap(), WiFi.RSSI());
  }
}
