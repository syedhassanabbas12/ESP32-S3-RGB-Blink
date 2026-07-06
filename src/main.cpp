// ESP32-S3 IoT node — control LEDs + relays over MQTT (TLS), WiFi set over BLE
//
// WiFi is provisioned over BLE using Espressif's official provisioning
// (companion app / react-native-esp-idf-provisioning). The MQTT broker is
// stored in flash (NVS) and can be set over USB serial. Nothing is compiled in.
//
//   First boot, or saved WiFi can't connect  -> the device starts BLE
//   provisioning and advertises as "PROV_HomeControl". Open the app, connect,
//   enter the proof-of-possession, pick a network, and it saves + connects.
//   Proof-of-possession (PoP): "homecontrol"
//
//   Re-provision anytime:  send "prov" over USB serial (115200) -> the device
//   reboots into BLE provisioning. "reset" wipes WiFi + broker settings.
//   Set/replace the broker over serial:
//     set host=<h> port=8883 user=<u> pass=<p> id=dev1
//
// Topic layout (device id is configurable):
//   home/<id>/status          -> "online" / "offline"   (retained, Last-Will)
//   home/<id>/<ch>/set        -> "on" / "off" / "toggle" (app  -> device)
//   home/<id>/<ch>/state      -> "on" / "off"            (device -> app, retained)

#include <WiFi.h>
#include <WiFiProv.h>           // Espressif BLE/SoftAP provisioning (BLE here)
#include <WiFiClientSecure.h>   // TLS transport for MQTT (HiveMQ Cloud :8883)
#include <Preferences.h>        // NVS key/value store for saved settings
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>   // over-the-air firmware update over HTTPS
#include <esp_system.h>         // esp_reset_reason()
#include <stdarg.h>             // va_list for logf()

// =============== runtime config (loaded from flash / set over serial) =========
char     DEVICE_ID[32] = "dev1";   // unique per board
char     MQTT_HOST[80] = "";       // e.g. abcd1234.s1.eu.hivemq.cloud
uint16_t MQTT_PORT     = 8883;     // HiveMQ Cloud TLS port
char     MQTT_USER[48] = "";
char     MQTT_PASS[64] = "";
// =============================================================================

// BLE provisioning identity. The app scans for the "PROV_" prefix and must
// supply this proof-of-possession to complete the secure (Security 1) handshake.
const char *PROV_SERVICE_NAME = "PROV_HomeControl";
const char *PROV_POP          = "homecontrol";

Preferences  prefs;
WiFiClientSecure net;         // TLS socket
PubSubClient mqtt(net);

volatile bool g_provisioning  = false;   // true while BLE provisioning is active
bool          g_everConnected = false;   // have we reached STA-connected since boot?
uint32_t      g_bootAt        = 0;

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

// ---- tiny logger: "[seconds.millis] <message>" so every line is timestamped ----
void logf(const char *fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  uint32_t ms = millis();
  Serial.printf("[%6lu.%03lu] %s\n", ms / 1000, ms % 1000, buf);
}

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

// One-shot "reboot into BLE provisioning" flag (survives the restart).
void setForceProv() { prefs.begin("homecfg", false); prefs.putBool("forceprov", true); prefs.end(); }
bool takeForceProv() {
  prefs.begin("homecfg", false);
  bool f = prefs.getBool("forceprov", false);
  if (f) prefs.putBool("forceprov", false);   // clear so it only fires once
  prefs.end();
  return f;
}

// -------------------- provisioning event handler --------------------
void onProvEvent(arduino_event_t *e) {
  switch (e->event_id) {
    case ARDUINO_EVENT_PROV_START:
      g_provisioning = true;
      logf("PROV: BLE provisioning started — open the app, device \"%s\", PoP \"%s\"",
           PROV_SERVICE_NAME, PROV_POP);
      break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
      logf("PROV: received WiFi credentials for SSID \"%s\"",
           (const char *)e->event_info.prov_cred_recv.ssid);
      break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
      logf("PROV: credentials FAILED (wrong WiFi password or AP unreachable)");
      break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      logf("PROV: WiFi credentials accepted");
      break;
    case ARDUINO_EVENT_PROV_END:
      g_provisioning = false;
      logf("PROV: provisioning finished (BLE released)");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      g_everConnected = true;
      logf("WiFi: connected  ip=%s  rssi=%d dBm  mac=%s",
           WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.macAddress().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // ESP32 auto-reconnects; only log to avoid noise.
      break;
    default: break;
  }
}

// Start BLE provisioning. reset=true forces it even if WiFi was already saved.
void startProvisioning(bool reset) {
  logf("PROV: begin (force=%s)", reset ? "yes" : "no");
  // FREE_BTDM frees the Bluetooth stack once provisioning ends, reclaiming RAM
  // for TLS/MQTT during normal operation.
  WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM,
                          WIFI_PROV_SECURITY_1, PROV_POP, PROV_SERVICE_NAME,
                          NULL, NULL, reset);
}

// -------------------- serial command handler --------------------
// "set host=.. port=.. user=.. pass=.. id=.."  provision/replace the broker
// "prov"   reboot into BLE WiFi provisioning
// "reset"  wipe WiFi + broker settings, then reboot into provisioning
// Returns true when the broker host was newly set (caller reconnects MQTT).
bool handleSerialConfig() {
  if (!Serial.available()) return false;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "prov") {
    logf("CMD:  rebooting into BLE provisioning");
    setForceProv();
    delay(300); ESP.restart();
  }
  if (line == "reset") {
    logf("CMD:  wiping WiFi + broker settings, rebooting into provisioning");
    prefs.begin("homecfg", false); prefs.clear(); prefs.end();
    WiFi.disconnect(true, true);   // erase stored WiFi credentials
    setForceProv();
    delay(500); ESP.restart();
  }
  // Recovery/dev fallback for setting WiFi without the BLE app. Format uses a
  // "|" separator so SSIDs/passwords containing spaces work:
  //   wifi <SSID>|<PASSWORD>
  if (line.startsWith("wifi ")) {
    String rest = line.substring(5);
    int bar = rest.indexOf('|');
    String ssid = bar >= 0 ? rest.substring(0, bar) : rest;
    String pass = bar >= 0 ? rest.substring(bar + 1) : "";
    logf("CMD:  setting WiFi over serial, ssid=\"%s\"", ssid.c_str());
    WiFi.persistent(true);         // save creds to NVS (survives reboot)
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    return false;
  }
  if (!line.startsWith("set ")) return false;

  for (int i = 4; i < (int)line.length(); ) {
    int sp = line.indexOf(' ', i);
    if (sp < 0) sp = line.length();
    String kv = line.substring(i, sp);
    int eq = kv.indexOf('=');
    if (eq > 0) {
      String k = kv.substring(0, eq), v = kv.substring(eq + 1);
      if      (k == "host") strlcpy(MQTT_HOST, v.c_str(), sizeof(MQTT_HOST));
      else if (k == "user") strlcpy(MQTT_USER, v.c_str(), sizeof(MQTT_USER));
      else if (k == "pass") strlcpy(MQTT_PASS, v.c_str(), sizeof(MQTT_PASS));
      else if (k == "id")   strlcpy(DEVICE_ID, v.c_str(), sizeof(DEVICE_ID));
      else if (k == "port") MQTT_PORT = (uint16_t) v.toInt();
    }
    i = sp + 1;
  }
  saveConfig();
  logf("CFG:  saved from serial  host=%s port=%u user=%s devid=%s",
       MQTT_HOST, MQTT_PORT, MQTT_USER, DEVICE_ID);
  return MQTT_HOST[0] != 0;
}

void publishState(Channel &c) {
  char t[80];
  snprintf(t, sizeof(t), "home/%s/%s/state", DEVICE_ID, c.name);
  mqtt.publish(t, c.state ? "on" : "off", true);   // retained
  logf("MQTT: tx  %s = %s", t, c.state ? "on" : "off");
}

void writePin(const Channel &c) {
  digitalWrite(c.pin, (c.state ^ c.activeLow) ? HIGH : LOW);
}

void applyChannel(Channel &c, bool on) {
  c.state = on;
  writePin(c);
  logf("CH:   %s (gpio %u) -> %s", c.name, c.pin, on ? "ON" : "off");
  publishState(c);
}

void doOTA(const String &url); // defined below; used by onMessage

// Blink the RGB LEDs a few times to help locate this device, then restore their
// previous state. Briefly blocking (~1s) — fine for an on-demand action.
void identifyBlink() {
  bool prev[3] = { channels[0].state, channels[1].state, channels[2].state };
  for (int i = 0; i < 6; i++) {
    bool on = (i % 2) == 0;
    for (int ch = 0; ch < 3; ch++) {
      channels[ch].state = on;
      writePin(channels[ch]);
    }
    delay(160);
  }
  for (int ch = 0; ch < 3; ch++) {
    channels[ch].state = prev[ch];
    writePin(channels[ch]);
    publishState(channels[ch]); // restore + keep the app consistent
  }
}

void onMessage(char *topic, byte *payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  logf("MQTT: rx  %s = \"%s\"", topic, msg.c_str());

  String t = topic;
  if (t == String("home/") + DEVICE_ID + "/ota") {
    doOTA(msg); // payload is the firmware image URL
    return;
  }
  if (t == String("home/") + DEVICE_ID + "/identify") {
    logf("CMD:  identify — blinking to locate device");
    identifyBlink();
    return;
  }
  if (t == String("home/") + DEVICE_ID + "/restart") {
    logf("CMD:  restart requested over MQTT — rebooting");
    char statusTopic[80];
    snprintf(statusTopic, sizeof(statusTopic), "home/%s/status", DEVICE_ID);
    mqtt.publish(statusTopic, "offline", true); // tell the app before we go
    delay(300);
    ESP.restart();
  }
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

#define FW_VERSION "2.6.0"

// Publish device health (firmware, signal, uptime, free heap) as retained JSON
// so the app can show live diagnostics in the device detail sheet.
// Topic: home/<id>/telemetry
void publishTelemetry() {
  if (!mqtt.connected()) return;
  char topic[80], payload[128];
  snprintf(topic, sizeof(topic), "home/%s/telemetry", DEVICE_ID);
  snprintf(payload, sizeof(payload),
           "{\"fw\":\"%s\",\"rssi\":%d,\"uptime\":%lu,\"heap\":%u}",
           FW_VERSION, WiFi.RSSI(), (unsigned long)(millis() / 1000), ESP.getFreeHeap());
  mqtt.publish(topic, payload, true);
}

void publishOtaStatus(const char *msg) {
  char t[80];
  snprintf(t, sizeof(t), "home/%s/ota/status", DEVICE_ID);
  mqtt.publish(t, msg, true);
  logf("OTA:  %s", msg);
}

// Download + flash a firmware image over HTTPS, then reboot into it. Triggered
// by an MQTT message on home/<id>/ota carrying the image URL. Blocking.
void doOTA(const String &url) {
  if (url.length() < 8) {
    publishOtaStatus("error: no url");
    return;
  }
  logf("OTA:  updating from %s", url.c_str());
  publishOtaStatus("starting");

  WiFiClientSecure otaClient;
  otaClient.setInsecure(); // skip cert check (matches the broker setup)
  otaClient.setTimeout(20000);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // GitHub redirects
  httpUpdate.onProgress([](int cur, int total) {
    static int last = -1;
    int pct = total > 0 ? (int)((int64_t)cur * 100 / total) : 0;
    if (pct / 25 != last / 25) { // ~every 25%
      last = pct;
      char m[24];
      snprintf(m, sizeof(m), "downloading %d%%", pct);
      publishOtaStatus(m);
    }
  });

  t_httpUpdate_return ret = httpUpdate.update(otaClient, url);
  if (ret == HTTP_UPDATE_FAILED) {
    char m[110];
    snprintf(m, sizeof(m), "error %d: %s", httpUpdate.getLastError(),
             httpUpdate.getLastErrorString().c_str());
    publishOtaStatus(m);
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    publishOtaStatus("no update");
  }
  // HTTP_UPDATE_OK reboots into the new image before returning.
}

void connectMQTT() {
  if (!MQTT_HOST[0]) {
    logf("MQTT: no broker configured — send over serial: set host=.. port=8883 user=.. pass=..");
    delay(3000);
    return;
  }
  char statusTopic[80];
  snprintf(statusTopic, sizeof(statusTopic), "home/%s/status", DEVICE_ID);

  while (!mqtt.connected()) {
    logf("MQTT: connecting to %s:%u (TLS) as \"%s\" ...", MQTT_HOST, MQTT_PORT, DEVICE_ID);
    bool ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                           statusTopic, 0, true, "offline");
    if (ok) {
      logf("MQTT: connected");
      mqtt.publish(statusTopic, "online", true);
      char sub[80];
      snprintf(sub, sizeof(sub), "home/%s/+/set", DEVICE_ID);
      mqtt.subscribe(sub);
      logf("MQTT: subscribed to %s", sub);
      char restartTopic[80];
      snprintf(restartTopic, sizeof(restartTopic), "home/%s/restart", DEVICE_ID);
      mqtt.subscribe(restartTopic);
      char identifyTopic[80];
      snprintf(identifyTopic, sizeof(identifyTopic), "home/%s/identify", DEVICE_ID);
      mqtt.subscribe(identifyTopic);
      char otaTopic[80];
      snprintf(otaTopic, sizeof(otaTopic), "home/%s/ota", DEVICE_ID);
      mqtt.subscribe(otaTopic);
      for (size_t i = 0; i < NUM_CH; i++) publishState(channels[i]);
      publishTelemetry();
    } else {
      logf("MQTT: connect failed rc=%d (%s), retry in 3s",
           mqtt.state(), mqttStateStr(mqtt.state()));
      delay(3000);
      return;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(200);
  delay(300);
  Serial.println();
  g_bootAt = millis();
  logf("==== ESP32-S3 IoT node booting ====");
  logf("chip      : %s rev%d, %d core(s)",
       ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  logf("flash     : %u MB", ESP.getFlashChipSize() / (1024 * 1024));
  logf("free heap : %u bytes", ESP.getFreeHeap());
  logf("reset     : %s", resetReasonStr());

  for (size_t i = 0; i < NUM_CH; i++) {
    pinMode(channels[i].pin, OUTPUT);
    channels[i].state = false;
    writePin(channels[i]);
    logf("CH:   %s on gpio %u -> off%s", channels[i].name, channels[i].pin,
         channels[i].activeLow ? " (active-low)" : "");
  }

  loadConfig();
  logf("CFG:  host=%s port=%u user=%s devid=%s",
       MQTT_HOST[0] ? MQTT_HOST : "(unset)", MQTT_PORT, MQTT_USER, DEVICE_ID);

  net.setInsecure();          // skip TLS cert validation (fine for a home broker)
  mqtt.setBufferSize(512);
  mqtt.setCallback(onMessage);
  if (MQTT_HOST[0]) mqtt.setServer(MQTT_HOST, MQTT_PORT);

  // Start WiFi. beginProvision connects with saved credentials, or (first boot
  // / forced) advertises over BLE for the app to provision. FREE_BTDM reclaims
  // Bluetooth RAM once provisioning ends.
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onProvEvent);
  bool force = takeForceProv();
  if (force) logf("PROV: force flag set — entering BLE provisioning");
  startProvisioning(force);
}

void loop() {
  // Serial commands (set / prov / reset) — reliable even without a button.
  if (handleSerialConfig()) {           // broker changed over serial -> reconnect
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    if (mqtt.connected()) mqtt.disconnect();
  }

  if (WiFi.status() != WL_CONNECTED) {
    // If we're actively provisioning over BLE, just wait for the user/app.
    // Otherwise, if we had saved WiFi but never connected this boot (e.g. the
    // network moved/changed), reboot into BLE provisioning after a grace period
    // so the app can reach us over Bluetooth and hand over new credentials.
    if (!g_provisioning && !g_everConnected && millis() - g_bootAt > 90000) {
      logf("WiFi: no connection in 90s — rebooting into BLE provisioning");
      setForceProv();
      delay(500); ESP.restart();
    }
    return;
  }

  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 30000) {
    lastBeat = millis();
    logf("alive  uptime=%lus  heap=%u  rssi=%d dBm",
         millis() / 1000, ESP.getFreeHeap(), WiFi.RSSI());
    publishTelemetry();
  }
}
