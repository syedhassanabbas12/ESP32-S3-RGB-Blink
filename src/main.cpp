// ESP32-S3 IoT node — Stage 1: control 3 LEDs over MQTT
//
// Topic layout (change "dev1" per board via DEVICE_ID):
//   home/dev1/status          -> "online" / "offline"   (retained, Last-Will)
//   home/dev1/<ch>/set        -> "on" / "off" / "toggle" (app  -> device)
//   home/dev1/<ch>/state      -> "on" / "off"            (device -> app, retained)
//
// Watch what the device is doing over USB serial (115200 baud):
//   ~/.platformio/penv/bin/pio device monitor -b 115200
// Every log line is prefixed with the uptime: [seconds.millis]. See tag legend
// in CLAUDE.md §9.
//
// Try it from the CLI once flashed:
//   mosquitto_sub -h <broker-ip> -t 'home/#' -v
//   mosquitto_pub -h <broker-ip> -t 'home/dev1/red/set' -m on

#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_system.h>   // esp_reset_reason()
#include <stdarg.h>       // va_list for logf()
#include "secrets.h"      // WIFI_SSID / WIFI_PASSWORD (gitignored — see secrets.example.h)

// ===================== CONFIG — edit these =====================
// WiFi credentials live in include/secrets.h (gitignored). Copy
// include/secrets.example.h -> include/secrets.h and fill in yours.

const char *MQTT_HOST = "192.168.173.170";   // your Mac / broker LAN IP
const uint16_t MQTT_PORT = 1883;
const char *MQTT_USER = "";                // leave "" if broker allows anonymous
const char *MQTT_PASS = "";

const char *DEVICE_ID = "dev1";            // must be unique per board
// ==============================================================

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

WiFiClient net;
PubSubClient mqtt(net);

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
    case -2: return "connect failed (broker down / wrong IP / wrong network?)";
    case -1: return "disconnected";
    case  0: return "connected";
    case  1: return "bad protocol";
    case  2: return "bad client id";
    case  3: return "server unavailable";
    case  4: return "bad credentials";
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

void connectWiFi() {
  logf("WiFi: connecting to \"%s\" ...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    if (millis() - start > 15000) {   // still not up after 15s? say so and keep trying
      logf("WiFi: still trying (status=%d) — check SSID/password and that it's 2.4GHz",
           WiFi.status());
      start = millis();
    }
  }
  logf("WiFi: connected  ip=%s  rssi=%d dBm  mac=%s",
       WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.macAddress().c_str());
}

void connectMQTT() {
  char statusTopic[64];
  snprintf(statusTopic, sizeof(statusTopic), "home/%s/status", DEVICE_ID);

  while (!mqtt.connected()) {
    logf("MQTT: connecting to %s:%u as \"%s\" ...", MQTT_HOST, MQTT_PORT, DEVICE_ID);
    // Last-Will: if we drop off the network, the broker publishes "offline" for us.
    bool ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                           statusTopic, 0, true, "offline");
    if (ok) {
      logf("MQTT: connected");
      mqtt.publish(statusTopic, "online", true);          // retained
      char sub[64];
      snprintf(sub, sizeof(sub), "home/%s/+/set", DEVICE_ID);
      mqtt.subscribe(sub);                                // "+" = any channel
      logf("MQTT: subscribed to %s", sub);
      for (size_t i = 0; i < NUM_CH; i++) publishState(channels[i]);  // sync app on connect
    } else {
      logf("MQTT: connect failed rc=%d (%s), retry in 2s",
           mqtt.state(), mqttStateStr(mqtt.state()));
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  logf("==== ESP32-S3 IoT node booting ====");
  logf("device id : %s", DEVICE_ID);
  logf("chip      : %s rev%d, %d core(s)",
       ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  logf("flash     : %u MB", ESP.getFlashChipSize() / (1024 * 1024));
  logf("free heap : %u bytes", ESP.getFreeHeap());
  logf("reset     : %s", resetReasonStr());

  for (size_t i = 0; i < NUM_CH; i++) {
    pinMode(channels[i].pin, OUTPUT);
    channels[i].state = false;
    writePin(channels[i]);   // drive to OFF (active-low relays stay off = pin HIGH)
    logf("CH:   %s on gpio %u -> off%s", channels[i].name, channels[i].pin,
         channels[i].activeLow ? " (active-low)" : "");
  }
  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    logf("WiFi: connection lost, reconnecting");
    connectWiFi();
  }
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();   // must run often — services incoming/outgoing MQTT

  // Heartbeat: every 30s prove we're alive and show link/memory health.
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 30000) {
    lastBeat = millis();
    logf("alive  uptime=%lus  heap=%u  rssi=%d dBm",
         millis() / 1000, ESP.getFreeHeap(), WiFi.RSSI());
  }
}
