/*
  ESP32 FLIGHT RADAR
  GC9A01 round TFT (radar), SSD1306 OLED (info), rotary encoder.
  Data: OpenSky Network REST API (OAuth2), type lookup via hexdb.io.

  Setup: WiFi, location and OpenSky credentials are entered on a web page and
  stored in flash. The device opens the WiFi "FlightRadar-Setup" (captive portal)
  on first start, when WiFi fails, or when the encoder is held at power-on.
  When connected, the same page is at http://flightradar.local/

  Encoder:  turn = zoom (ZOOM mode) / pick plane (SELECT mode)
            click = switch ZOOM/SELECT
            long press = next OLED page (flight / details / statistics)
  SELECT mode returns to ZOOM after SELECT_TIMEOUT_MS without input.

  Libraries: Adafruit GFX, Adafruit SSD1306, GFX Library for Arduino,
  ArduinoJson (v6 or v7).
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

const char *SETUP_AP_NAME   = "FlightRadar-Setup";
const char *MDNS_NAME = "flightradar";

const unsigned long POLL_INTERVAL_MS   = 30000;
const unsigned long BACKOFF_429_MS     = 180000;
const unsigned long SELECT_TIMEOUT_MS  = 10000;
const unsigned long LONG_PRESS_MS      = 700;
const unsigned long WIFI_TIMEOUT_MS    = 20000;
const unsigned long PORTAL_TIMEOUT_MS  = 600000;

#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4
#define TFT_SCK  18
#define TFT_MOSI 23

#define ENC_CLK 25
#define ENC_DT  26
#define ENC_SW  27

WiFiClientSecure secureClient;

Adafruit_SSD1306 infoDisplay(128, 64, &Wire, -1);

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED, VSPI);
Arduino_GFX *radar = new Arduino_GC9A01(bus, TFT_RST, 0, true);

#define RADAR_W 240
#define RADAR_H 240
#define RADAR_CX 120
#define RADAR_CY 120
#define RADAR_MAX_R 110

uint16_t COL_BG, COL_RING, COL_TEXT, COL_PLANE, COL_SELECTED, COL_HOME;

struct Plane {
  String icao24;
  String callsign;
  double lat, lon;
  float altitude_m;
  float velocity_ms;
  float heading_deg;
  float vrate_ms;
  bool on_ground;
  float distance_km;
  float bearing_deg;
  bool valid;
};

#define MAX_PLANES 100
Plane planes[MAX_PLANES];
int planeCount = 0;    // all planes within the largest range, sorted by distance
int visibleCount = 0;  // prefix of planes[] inside the current range

enum Mode { MODE_ZOOM, MODE_SELECT };
Mode currentMode = MODE_ZOOM;

const int RANGE_PRESETS[] = {10, 25, 50, 100, 200}; // km
const int NUM_RANGES = 5;
int rangeIndex = 3;

int selectedIndex = 0;
String selectedIcao = "";
bool followNearest = true;

int infoPage = 0; // 0 flight, 1 details, 2 statistics

unsigned long pollIntervalMs = POLL_INTERVAL_MS;
unsigned long lastInputMs = 0;
unsigned long lastFetchOkMs = 0;

// ---------------------------------------------------------------------
// SETTINGS (stored in flash)
// ---------------------------------------------------------------------
Preferences prefs;
String cfgSsid, cfgPass, cfgClientId, cfgClientSecret, cfgOtaPass;
double cfgLat = 0, cfgLon = 0;

void loadConfig() {
  prefs.begin("radar", true);
  cfgSsid         = prefs.getString("ssid", "");
  cfgPass         = prefs.getString("pass", "");
  cfgClientId     = prefs.getString("cid", "");
  cfgClientSecret = prefs.getString("csec", "");
  cfgOtaPass      = prefs.getString("ota", "");
  cfgLat          = prefs.getDouble("lat", 0);
  cfgLon          = prefs.getDouble("lon", 0);
  int r           = prefs.getInt("range", 3);
  rangeIndex      = (r < 0 || r >= NUM_RANGES) ? 3 : r;
  prefs.end();
}

bool hasConfig() {
  return cfgSsid.length() > 0;
}

bool hasCoords() {
  return !(cfgLat == 0 && cfgLon == 0);
}

// ---------------------------------------------------------------------
// ENCODER
// ---------------------------------------------------------------------
volatile int encoderDelta = 0;
volatile uint8_t encoderStateHistory = 0;
volatile int8_t encoderAccum = 0;

const int8_t QUADRATURE_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

void IRAM_ATTR handleEncoder() {
  encoderStateHistory <<= 2;
  encoderStateHistory |= (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  encoderStateHistory &= 0x0F;

  int8_t change = QUADRATURE_TABLE[encoderStateHistory];
  if (change != 0) {
    encoderAccum += change;
    if (encoderAccum >= 4) {
      encoderDelta++;
      encoderAccum = 0;
    } else if (encoderAccum <= -4) {
      encoderDelta--;
      encoderAccum = 0;
    }
  }
}

bool btnWasDown = false;
bool longPressDone = false;
unsigned long btnDownMs = 0;
unsigned long btnEdgeMs = 0;

// Returns true if something changed that needs a redraw.
bool checkButton() {
  unsigned long now = millis();
  bool down = (digitalRead(ENC_SW) == LOW);
  bool changed = false;

  if (down != btnWasDown && now - btnEdgeMs > 25) {
    btnEdgeMs = now;
    btnWasDown = down;
    if (down) {
      btnDownMs = now;
      longPressDone = false;
    } else if (!longPressDone) {
      currentMode = (currentMode == MODE_ZOOM) ? MODE_SELECT : MODE_ZOOM;
      changed = true;
    }
    lastInputMs = now;
  }

  if (btnWasDown && !longPressDone && now - btnDownMs > LONG_PRESS_MS) {
    longPressDone = true;
    infoPage = (infoPage + 1) % 3;
    lastInputMs = now;
    changed = true;
  }
  return changed;
}

// ---------------------------------------------------------------------
// GEO
// ---------------------------------------------------------------------
double toRad(double deg) { return deg * M_PI / 180.0; }
double toDeg(double rad) { return rad * 180.0 / M_PI; }

float distanceKm(double lat1, double lon1, double lat2, double lon2) {
  double R = 6371.0;
  double dLat = toRad(lat2 - lat1);
  double dLon = toRad(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(toRad(lat1)) * cos(toRad(lat2)) *
             sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return (float)(R * c);
}

float bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  double y = sin(toRad(lon2 - lon1)) * cos(toRad(lat2));
  double x = cos(toRad(lat1)) * sin(toRad(lat2)) -
             sin(toRad(lat1)) * cos(toRad(lat2)) * cos(toRad(lon2 - lon1));
  double brng = toDeg(atan2(y, x));
  return (float)fmod((brng + 360.0), 360.0);
}

// Keeps the selection on the same aircraft across polls (matched by icao24).
void syncSelection() {
  if (planeCount == 0) {
    selectedIndex = 0;
    return;
  }
  if (followNearest) {
    selectedIndex = 0;
  } else {
    int found = -1;
    for (int i = 0; i < planeCount; i++) {
      if (planes[i].icao24 == selectedIcao) { found = i; break; }
    }
    selectedIndex = (found >= 0) ? found : 0;
  }
  if (selectedIndex >= visibleCount) selectedIndex = 0;
  selectedIcao = planes[selectedIndex].icao24;
}

void updateVisible() {
  int rangeKm = RANGE_PRESETS[rangeIndex];
  visibleCount = 0;
  while (visibleCount < planeCount && planes[visibleCount].distance_km <= rangeKm) {
    visibleCount++;
  }
  syncSelection();
}

// ---------------------------------------------------------------------
// AIRCRAFT TYPE LOOKUP
// ---------------------------------------------------------------------
String metaIcao24;
String metaType;
String metaManufacturer;
String metaOperator;
bool metaValid = false;
bool metaLookupFailed = false;

void lookupAircraftType(const String &icao24) {
  if (icao24.length() == 0) return;
  if (icao24 == metaIcao24 && (metaValid || metaLookupFailed)) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setReuse(false);
  http.begin(secureClient, "https://hexdb.io/api/v1/aircraft/" + icao24);
  http.setTimeout(5000);
  int code = http.GET();

  metaIcao24 = icao24;
  metaValid = false;
  metaLookupFailed = true;

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, payload)) {
      if (doc.containsKey("Manufacturer")) {
        metaManufacturer = doc["Manufacturer"].as<String>();
        metaType = doc["Type"].as<String>();
        metaOperator = doc["RegisteredOwners"] | "";
        metaValid = true;
        metaLookupFailed = false;
      }
    }
  }
  http.end();
}

// ---------------------------------------------------------------------
// OPENSKY TOKEN
// ---------------------------------------------------------------------
String openskyToken;
unsigned long tokenExpiresAtMs = 0;

String getOpenSkyToken() {
  if (cfgClientId.length() == 0 || cfgClientSecret.length() == 0) return "";
  if (openskyToken.length() > 0 && millis() < tokenExpiresAtMs) {
    return openskyToken;
  }
  if (WiFi.status() != WL_CONNECTED) return "";

  HTTPClient http;
  http.setReuse(false);
  http.begin(secureClient, "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=client_credentials&client_id=" + cfgClientId +
                "&client_secret=" + cfgClientSecret;

  int code = http.POST(body);
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, payload)) {
      openskyToken = doc["access_token"].as<String>();
      long expiresIn = doc["expires_in"] | 1800;
      if (expiresIn < 120) expiresIn = 120;
      tokenExpiresAtMs = millis() + (unsigned long)(expiresIn - 60) * 1000UL;
      Serial.println("OpenSky token refreshed OK");
    }
  } else {
    Serial.print("Token fetch failed, HTTP code: ");
    Serial.println(code);
    openskyToken = "";
  }
  http.end();
  return openskyToken;
}

// ---------------------------------------------------------------------
// OPENSKY FETCH (always largest range; zoom filters locally)
// ---------------------------------------------------------------------
int doStatesRequest(HTTPClient &http, const String &url, const String &token) {
  http.setReuse(false);
  http.useHTTP10(true);
  http.begin(secureClient, url);
  http.setTimeout(8000);
  if (token.length() > 0) {
    http.addHeader("Authorization", "Bearer " + token);
  }
  return http.GET();
}

void fetchPlanes() {
  if (WiFi.status() != WL_CONNECTED) return;

  int rangeKm = RANGE_PRESETS[NUM_RANGES - 1];
  double latPad = rangeKm / 111.0;
  double lonPad = rangeKm / (111.0 * cos(toRad(cfgLat)) + 0.0001);

  String url = "https://opensky-network.org/api/states/all?lamin=" + String(cfgLat - latPad, 4) +
               "&lomin=" + String(cfgLon - lonPad, 4) +
               "&lamax=" + String(cfgLat + latPad, 4) +
               "&lomax=" + String(cfgLon + lonPad, 4);

  String token = getOpenSkyToken();

  HTTPClient http;
  int code = doStatesRequest(http, url, token);
  Serial.print("States fetch HTTP code: ");
  Serial.println(code);
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());

  if (code == 429) {
    Serial.println("Rate limited, backing off");
    pollIntervalMs = BACKOFF_429_MS;
    http.end();
    return;
  }

  if (code != 200) {
    http.end();
    delay(2000);
    code = doStatesRequest(http, url, token);
    Serial.print("Retry HTTP code: ");
    Serial.println(code);
  }

  if (code == 200) {
    pollIntervalMs = POLL_INTERVAL_MS;

    // Filtered arrays are compacted:
    // 0 icao24, 1 callsign, 2 lon, 3 lat, 4 baro_alt, 5 on_ground,
    // 6 velocity, 7 track, 8 vertical_rate
    StaticJsonDocument<256> filter;
    const int keep[] = {0, 1, 5, 6, 7, 8, 9, 10, 11};
    for (int k : keep) filter["states"][0][k] = true;

    DynamicJsonDocument doc(40960);
    DeserializationError err = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));

    if (!err) {
      JsonArray states = doc["states"].as<JsonArray>();
      planeCount = 0;
      for (JsonArray s : states) {
        if (planeCount >= MAX_PLANES) break;
        if (s.size() < 8) continue;
        if (s[2].isNull() || s[3].isNull()) continue;
        if (!s[5].isNull() && s[5].as<bool>()) continue;

        Plane &p = planes[planeCount];
        p.icao24 = String(s[0].as<const char*>());
        const char *cs = s[1] | "";
        p.callsign = String(cs);
        p.callsign.trim();
        if (p.callsign.length() == 0) p.callsign = p.icao24;

        p.lon = s[2].as<double>();
        p.lat = s[3].as<double>();
        p.altitude_m = s[4].isNull() ? 0 : s[4].as<float>();
        p.on_ground = false;
        p.velocity_ms = s[6].isNull() ? 0 : s[6].as<float>();
        p.heading_deg = s[7].isNull() ? 0 : s[7].as<float>();
        p.vrate_ms = (s.size() > 8 && !s[8].isNull()) ? s[8].as<float>() : 0;

        p.distance_km = distanceKm(cfgLat, cfgLon, p.lat, p.lon);
        p.bearing_deg = bearingDeg(cfgLat, cfgLon, p.lat, p.lon);
        p.valid = true;

        if (p.distance_km > rangeKm) continue;
        planeCount++;
      }

      for (int i = 1; i < planeCount; i++) {
        Plane key = planes[i];
        int j = i - 1;
        while (j >= 0 && planes[j].distance_km > key.distance_km) {
          planes[j + 1] = planes[j];
          j--;
        }
        planes[j + 1] = key;
      }

      lastFetchOkMs = millis();
      updateVisible();
      Serial.print("Planes in range: ");
      Serial.println(planeCount);
    } else {
      Serial.print("JSON parse error: ");
      Serial.println(err.c_str());
    }
  }
  http.end();
}

// ---------------------------------------------------------------------
// DRAWING
// ---------------------------------------------------------------------
void drawRadar() {
  radar->fillScreen(COL_BG);

  int rangeKm = RANGE_PRESETS[rangeIndex];

  for (int i = 1; i <= 3; i++) {
    int r = (RADAR_MAX_R * i) / 3;
    radar->drawCircle(RADAR_CX, RADAR_CY, r, COL_RING);
  }

  radar->drawLine(RADAR_CX, RADAR_CY - RADAR_MAX_R, RADAR_CX, RADAR_CY + RADAR_MAX_R, COL_RING);
  radar->drawLine(RADAR_CX - RADAR_MAX_R, RADAR_CY, RADAR_CX + RADAR_MAX_R, RADAR_CY, COL_RING);

  radar->setTextColor(COL_TEXT);
  radar->setTextSize(1);
  radar->setCursor(RADAR_CX - 3, RADAR_CY - RADAR_MAX_R - 10); radar->print("N");
  radar->setCursor(RADAR_CX + RADAR_MAX_R + 2, RADAR_CY - 3);  radar->print("E");
  radar->setCursor(RADAR_CX - 3, RADAR_CY + RADAR_MAX_R + 2);  radar->print("S");
  radar->setCursor(RADAR_CX - RADAR_MAX_R - 10, RADAR_CY - 3); radar->print("W");

  radar->setCursor(RADAR_CX + 4, RADAR_CY - RADAR_MAX_R + 4);
  radar->print(rangeKm);
  radar->print("km");

  radar->fillTriangle(RADAR_CX, RADAR_CY - 6, RADAR_CX - 5, RADAR_CY + 5,
                       RADAR_CX + 5, RADAR_CY + 5, COL_HOME);

  for (int i = 0; i < visibleCount; i++) {
    Plane &p = planes[i];
    float r = (p.distance_km / rangeKm) * RADAR_MAX_R;
    if (r > RADAR_MAX_R) continue;
    float rad = toRad(p.bearing_deg);
    int x = RADAR_CX + (int)(r * sin(rad));
    int y = RADAR_CY - (int)(r * cos(rad));

    bool isSelected = (i == selectedIndex);
    uint16_t col = isSelected ? COL_SELECTED : COL_PLANE;

    radar->fillCircle(x, y, isSelected ? 4 : 3, col);
    if (isSelected) {
      radar->drawCircle(x, y, 7, col);
    }
  }

  radar->setCursor(RADAR_CX + 4, RADAR_CY + RADAR_MAX_R - 12);
  radar->setTextColor(COL_TEXT);
  radar->print(currentMode == MODE_ZOOM ? "ZOOM" : "SELECT");
}

String clip(const String &s, size_t n) {
  return s.length() > n ? s.substring(0, n) : s;
}

void drawPageMark() {
  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(110, 0);
  infoDisplay.print(infoPage + 1);
  infoDisplay.print("/3");
}

void drawDetails(Plane &p) {
  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(0, 0);
  infoDisplay.print(clip(p.callsign, 8));
  drawPageMark();

  bool have = (metaIcao24 == p.icao24 && metaValid);

  infoDisplay.setCursor(0, 14);
  if (have) {
    infoDisplay.print(metaOperator.length() ? clip(metaOperator, 21) : String("Betreiber: --"));
  } else if (metaIcao24 == p.icao24 && metaLookupFailed) {
    infoDisplay.print("Betreiber: unbekannt");
  } else {
    infoDisplay.print("Suche...");
  }

  infoDisplay.setCursor(0, 26);
  if (have) infoDisplay.print(clip(metaManufacturer + " " + metaType, 21));

  int vs = (int)(p.vrate_ms * 196.85f);
  infoDisplay.setCursor(0, 38);
  infoDisplay.print("V/S: ");
  if (vs > 0) infoDisplay.print("+");
  infoDisplay.print(vs);
  infoDisplay.print(" ft/min");

  infoDisplay.setCursor(0, 50);
  infoDisplay.print("ICAO: ");
  infoDisplay.print(p.icao24);
}

void drawStats() {
  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(0, 0);
  infoDisplay.print("IP ");
  infoDisplay.print(WiFi.localIP());
  drawPageMark();

  infoDisplay.setCursor(0, 14);
  infoDisplay.print("In ");
  infoDisplay.print(RANGE_PRESETS[rangeIndex]);
  infoDisplay.print(" km: ");
  infoDisplay.print(visibleCount);

  infoDisplay.setCursor(0, 26);
  infoDisplay.print("In ");
  infoDisplay.print(RANGE_PRESETS[NUM_RANGES - 1]);
  infoDisplay.print(" km: ");
  infoDisplay.print(planeCount);

  infoDisplay.setCursor(0, 38);
  if (planeCount > 0) {
    infoDisplay.print("Naechst: ");
    infoDisplay.print(clip(planes[0].callsign, 8));
  } else {
    infoDisplay.print("Kein Verkehr");
  }

  infoDisplay.setCursor(0, 50);
  infoDisplay.print("Update: ");
  if (lastFetchOkMs == 0) {
    infoDisplay.print("--");
  } else {
    infoDisplay.print((millis() - lastFetchOkMs) / 1000);
    infoDisplay.print(" s");
  }
}

void drawInfo() {
  infoDisplay.clearDisplay();
  infoDisplay.setTextColor(SSD1306_WHITE);

  if (infoPage == 2) {
    drawStats();
    infoDisplay.display();
    return;
  }

  if (visibleCount == 0) {
    infoDisplay.setTextSize(1);
    infoDisplay.setCursor(0, 0);
    infoDisplay.print("No traffic");
    infoDisplay.setCursor(0, 20);
    infoDisplay.print("Range: ");
    infoDisplay.print(RANGE_PRESETS[rangeIndex]);
    infoDisplay.print(" km");
    drawPageMark();
    infoDisplay.display();
    return;
  }

  int idx = (selectedIndex < visibleCount) ? selectedIndex : 0;
  Plane &p = planes[idx];

  if (infoPage == 1) {
    drawDetails(p);
    infoDisplay.display();
    return;
  }

  float altFt = p.altitude_m * 3.28084f;
  float speedKt = p.velocity_ms * 1.94384f;

  infoDisplay.setTextSize(2);
  infoDisplay.setCursor(0, 0);
  infoDisplay.print(clip(p.callsign, 8));

  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(0, 20);
  infoDisplay.print("Alt: ");
  infoDisplay.print((int)altFt);
  infoDisplay.print(" ft");

  infoDisplay.setCursor(0, 32);
  infoDisplay.print("Spd: ");
  infoDisplay.print((int)speedKt);
  infoDisplay.print(" kt  Hdg: ");
  infoDisplay.print((int)p.heading_deg);

  infoDisplay.setCursor(0, 44);
  infoDisplay.print("Dist: ");
  infoDisplay.print(p.distance_km, 1);
  infoDisplay.print(" km");

  infoDisplay.setCursor(0, 56);
  if (metaIcao24 == p.icao24 && metaValid) {
    infoDisplay.print(clip(metaManufacturer + " " + metaType, 21));
  } else if (metaIcao24 == p.icao24 && metaLookupFailed) {
    infoDisplay.print("Type: unknown");
  } else {
    infoDisplay.print("Type: looking up...");
  }

  drawPageMark();
  infoDisplay.display();
}

// ---------------------------------------------------------------------
// WEB SETUP (captive portal + settings page)
// ---------------------------------------------------------------------
WebServer server(80);
DNSServer dnsServer;
bool portalMode = false;
bool wifiScanned = false;
String netOptions;

String esc(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

void scanWifi() {
  int n = WiFi.scanNetworks();
  netOptions = "";
  String seen = "|";
  for (int i = 0; i < n && i < 25; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0 || seen.indexOf("|" + s + "|") >= 0) continue;
    seen += s + "|";
    netOptions += "<option value=\"" + esc(s) + "\">" + esc(s) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  wifiScanned = true;
}

String formPage() {
  String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Flugradar</title><style>"
    "body{font-family:sans-serif;max-width:480px;margin:auto;padding:16px}"
    "label{display:block;margin-top:14px;font-weight:600}"
    "input,select{width:100%;padding:9px;font-size:16px;box-sizing:border-box;margin-top:4px}"
    "button{margin-top:20px;padding:13px;width:100%;font-size:17px}"
    "small{color:#666;font-weight:400}</style></head><body>"
    "<h2>Flugradar</h2><form method=POST action=/save>"
    "<label>WLAN-Netzwerk</label>"
    "<select onchange=\"document.getElementById('s').value=this.value\">"
    "<option value=\"\">Netzwerk wählen ...</option>");
  h += netOptions;
  h += F("</select><input id=s name=ssid placeholder=\"oder Name eingeben\" value=\"");
  h += esc(cfgSsid);
  h += F("\"><small><a href=/rescan>Netzwerke neu suchen</a></small>"
         "<label>WLAN-Passwort</label><input name=pass type=password autocomplete=off placeholder=\"");
  h += cfgSsid.length() ? "(unverändert lassen)" : "";
  h += F("\"><label>Breitengrad <small>optional, z. B. 52.5200</small></label>"
         "<input name=lat inputmode=decimal value=\"");
  h += (cfgLat != 0 ? String(cfgLat, 6) : String(""));
  h += F("\"><label>Längengrad <small>optional, z. B. 13.4050</small></label>"
         "<input name=lon inputmode=decimal value=\"");
  h += (cfgLon != 0 ? String(cfgLon, 6) : String(""));
  h += F("\"><small>Koordinaten: in Google Maps den Standort lange antippen, die Zahlen erscheinen oben. "
         "Nur das WLAN ist Pflicht. Alles andere kann später unter flightradar.local ergänzt werden.</small>"
         "<label>OpenSky Client-ID <small>optional</small></label>"
         "<input name=cid autocapitalize=off value=\"");
  h += esc(cfgClientId);
  h += F("\"><label>OpenSky Client-Secret <small>optional</small></label>"
         "<input name=csec type=password autocomplete=off placeholder=\"");
  h += cfgClientSecret.length() ? "(unverändert lassen)" : "";
  h += F("\"><label>Start-Radius</label><select name=range>");
  for (int i = 0; i < NUM_RANGES; i++) {
    h += "<option value=" + String(i) + (i == rangeIndex ? " selected" : "") + ">" + String(RANGE_PRESETS[i]) + " km</option>";
  }
  h += F("</select><label>OTA-Passwort <small>für Firmware-Updates, mind. 6 Zeichen. Ohne Passwort sind Updates aus.</small></label>"
         "<input name=ota type=password autocomplete=off placeholder=\"");
  h += cfgOtaPass.length() ? "(unverändert lassen)" : "";
  h += F("\"><button>Speichern und neu starten</button></form>"
         "<p><a href=/update>Firmware-Update</a></p></body></html>");
  return h;
}

String msgPage(const String &msg, bool back) {
  String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Flugradar</title></head>"
    "<body style='font-family:sans-serif;max-width:480px;margin:auto;padding:16px'>"
    "<h2>Flugradar</h2><p>");
  h += msg;
  h += back ? "</p><p><a href=/>Zurück</a></p>" : "</p>";
  h += F("</body></html>");
  return h;
}

void handleRoot() {
  if (!wifiScanned) scanWifi();
  server.send(200, "text/html; charset=utf-8", formPage());
}

void handleRescan() {
  scanWifi();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSave() {
  String ssid = server.arg("ssid");
  ssid.trim();
  String pass = server.arg("pass");
  String lat = server.arg("lat");
  String lon = server.arg("lon");
  lat.trim(); lat.replace(',', '.');
  lon.trim(); lon.replace(',', '.');
  double la = lat.length() ? lat.toDouble() : cfgLat;
  double lo = lon.length() ? lon.toDouble() : cfgLon;

  if (ssid.length() == 0 || fabs(la) > 90 || fabs(lo) > 180) {
    server.send(400, "text/html; charset=utf-8",
                msgPage("Bitte WLAN-Name sowie Breiten- und Längengrad prüfen.", true));
    return;
  }

  if (pass.length() == 0 && ssid == cfgSsid) pass = cfgPass;
  String cid = server.arg("cid");
  cid.trim();
  String csec = server.arg("csec");
  csec.trim();
  if (csec.length() == 0 && cid == cfgClientId) csec = cfgClientSecret;
  int range = server.arg("range").toInt();
  if (range < 0 || range >= NUM_RANGES) range = 3;
  String ota = server.arg("ota");
  ota.trim();
  if (ota.length() > 0 && ota.length() < 6) {
    server.send(400, "text/html; charset=utf-8",
                msgPage("Das OTA-Passwort muss mindestens 6 Zeichen haben.", true));
    return;
  }
  if (ota.length() == 0) ota = cfgOtaPass;

  prefs.begin("radar", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("cid", cid);
  prefs.putString("csec", csec);
  prefs.putDouble("lat", la);
  prefs.putDouble("lon", lo);
  prefs.putInt("range", range);
  prefs.putString("ota", ota);
  prefs.end();

  server.send(200, "text/html; charset=utf-8",
              msgPage("Gespeichert. Das Gerät startet neu und verbindet sich mit dem WLAN.", false));
  delay(1500);
  ESP.restart();
}

bool otaActive = false;
bool updateOk = false;
int updLast = -1;

void showUpdateScreen(int pct) {
  if (pct == updLast) return;
  if (updLast < 0) {
    radar->fillScreen(COL_BG);
    radar->setTextColor(COL_TEXT);
    radar->setTextSize(2);
    radar->setCursor(84, 108);
    radar->print("UPDATE");
  }
  updLast = pct;
  infoDisplay.clearDisplay();
  infoDisplay.setTextColor(SSD1306_WHITE);
  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(0, 0);
  infoDisplay.print("Firmware-Update");
  infoDisplay.setTextSize(2);
  infoDisplay.setCursor(0, 24);
  infoDisplay.print(pct);
  infoDisplay.print(" %");
  infoDisplay.drawRect(0, 52, 128, 10, SSD1306_WHITE);
  infoDisplay.fillRect(2, 54, (124 * pct) / 100, 6, SSD1306_WHITE);
  infoDisplay.display();
}

bool otaAuthOk() {
  return cfgOtaPass.length() > 0 && server.authenticate("admin", cfgOtaPass.c_str());
}

void handleUpdatePage() {
  if (cfgOtaPass.length() == 0) {
    server.send(200, "text/html; charset=utf-8",
                msgPage("Updates sind ausgeschaltet. Setze auf der Einrichtungsseite ein OTA-Passwort.", true));
    return;
  }
  if (!server.authenticate("admin", cfgOtaPass.c_str())) {
    server.requestAuthentication();
    return;
  }
  String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Flugradar</title></head>"
    "<body style='font-family:sans-serif;max-width:480px;margin:auto;padding:16px'>"
    "<h2>Firmware-Update</h2>"
    "<form method=POST action=/update enctype=multipart/form-data>"
    "<p><input type=file name=firmware accept=.bin required></p>"
    "<p><button style='padding:12px;width:100%;font-size:17px'>Hochladen</button></p></form>"
    "<p><small>Das Gerät startet nach dem Update neu.</small></p>"
    "<p><a href=/>Zurück</a></p></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

void handleUpdateUpload() {
  if (!otaAuthOk()) return;
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    updateOk = false;
    showUpdateScreen(0);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      updateOk = true;
      showUpdateScreen(100);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleUpdateDone() {
  if (!otaAuthOk()) {
    server.requestAuthentication();
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/html; charset=utf-8",
              msgPage(updateOk ? "Update erfolgreich. Das Gerät startet neu." : "Update fehlgeschlagen.", !updateOk));
  delay(1000);
  if (updateOk) {
    ESP.restart();
  } else {
    updLast = -1;
    drawRadar();
    drawInfo();
  }
}

void handleNotFound() {
  if (portalMode) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void startServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/rescan", HTTP_GET, handleRescan);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  static const char *collect[] = {"Authorization"};
  server.collectHeaders(collect, 1);
  server.begin();
}

void showPortalScreens() {
  radar->fillScreen(COL_BG);
  radar->setTextColor(COL_TEXT);
  radar->setTextSize(2);
  radar->setCursor(84, 70);
  radar->print("SETUP");
  radar->setTextSize(1);
  radar->setCursor(66, 108);
  radar->print("Mit WLAN verbinden:");
  radar->setTextColor(COL_PLANE);
  radar->setTextSize(2);
  radar->setCursor(48, 128);
  radar->print("FlightRadar-");
  radar->setCursor(90, 148);
  radar->print("Setup");
  radar->setTextColor(COL_TEXT);
  radar->setTextSize(1);
  radar->setCursor(66, 182);
  radar->print("Seite oeffnet sich");
  radar->setCursor(72, 196);
  radar->print("oder 192.168.4.1");

  infoDisplay.clearDisplay();
  infoDisplay.setTextColor(SSD1306_WHITE);
  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(0, 0);  infoDisplay.print("EINRICHTUNG");
  infoDisplay.setCursor(0, 14); infoDisplay.print("1. WLAN verbinden:");
  infoDisplay.setCursor(0, 26); infoDisplay.print("FlightRadar-Setup");
  infoDisplay.setCursor(0, 40); infoDisplay.print("2. Seite oeffnet sich");
  infoDisplay.setCursor(0, 52); infoDisplay.print("oder 192.168.4.1");
  infoDisplay.display();
}

void runPortal(unsigned long timeoutMs) {
  portalMode = true;
  Serial.println("Starting setup portal");
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  delay(100);
  scanWifi();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_NAME);
  delay(300);
  dnsServer.start(53, "*", WiFi.softAPIP());
  startServer();
  showPortalScreens();

  unsigned long t0 = millis();
  while (timeoutMs == 0 || millis() - t0 < timeoutMs) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(2);
  }
  ESP.restart();
}

bool connectWifi() {
  infoDisplay.clearDisplay();
  infoDisplay.setTextColor(SSD1306_WHITE);
  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(0, 0);
  infoDisplay.print("Verbinde WLAN ...");
  infoDisplay.setCursor(0, 14);
  infoDisplay.print(clip(cfgSsid, 21));
  infoDisplay.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

void startStaServices() {
  portalMode = false;
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
  startServer();

  infoDisplay.clearDisplay();
  infoDisplay.setTextColor(SSD1306_WHITE);
  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(0, 0);  infoDisplay.print("WLAN verbunden");
  infoDisplay.setCursor(0, 14); infoDisplay.print(WiFi.localIP());
  infoDisplay.setCursor(0, 32); infoDisplay.print("Einstellungen:");
  infoDisplay.setCursor(0, 44); infoDisplay.print("flightradar.local");
  infoDisplay.display();
  delay(2500);
}

void showNeedCoords() {
  radar->fillScreen(COL_BG);
  radar->setTextColor(COL_TEXT);
  radar->setTextSize(2);
  radar->setCursor(36, 96);
  radar->print("Koordinaten");
  radar->setCursor(60, 120);
  radar->print("fehlen");
  infoDisplay.clearDisplay();
  infoDisplay.setTextColor(SSD1306_WHITE);
  infoDisplay.setTextSize(1);
  infoDisplay.setCursor(0, 0);  infoDisplay.print("Koordinaten fehlen");
  infoDisplay.setCursor(0, 16); infoDisplay.print("Im Browser oeffnen:");
  infoDisplay.setCursor(0, 28); infoDisplay.print("flightradar.local");
  infoDisplay.setCursor(0, 40); infoDisplay.print("oder ");
  infoDisplay.print(WiFi.localIP());
  infoDisplay.display();
}

void startOta() {
  if (cfgOtaPass.length() == 0) return;
  ArduinoOTA.setHostname(MDNS_NAME);
  ArduinoOTA.setPassword(cfgOtaPass.c_str());
  ArduinoOTA.onStart([]() { showUpdateScreen(0); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    showUpdateScreen(total ? (int)((unsigned long)done * 100 / total) : 0);
  });
  ArduinoOTA.onEnd([]() { showUpdateScreen(100); });
  ArduinoOTA.onError([](ota_error_t) {
    updLast = -1;
    drawRadar();
    drawInfo();
  });
  ArduinoOTA.begin();
  otaActive = true;
}

// ---------------------------------------------------------------------
// SETUP / LOOP
// ---------------------------------------------------------------------
unsigned long lastPoll = 0;
unsigned long lastInfoRedraw = 0;

void setup() {
  Serial.begin(115200);

  secureClient.setInsecure();

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), handleEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT), handleEncoder, CHANGE);

  Wire.begin(21, 22);
  if (!infoDisplay.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed");
  }
  infoDisplay.clearDisplay();
  infoDisplay.display();

  radar->begin();
  COL_BG       = radar->color565(0, 0, 0);
  COL_RING     = radar->color565(0, 90, 0);
  COL_TEXT     = radar->color565(200, 200, 200);
  COL_PLANE    = radar->color565(0, 255, 100);
  COL_SELECTED = radar->color565(255, 60, 60);
  COL_HOME     = radar->color565(60, 140, 255);
  radar->fillScreen(COL_BG);

  loadConfig();
  bool forceSetup = (digitalRead(ENC_SW) == LOW);

  if (!hasConfig()) {
    runPortal(0);
  } else if (forceSetup) {
    runPortal(PORTAL_TIMEOUT_MS);
  } else if (!connectWifi()) {
    Serial.println("WiFi FAILED to connect");
    runPortal(PORTAL_TIMEOUT_MS);
  }

  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
  startStaServices();
  startOta();

  lastPoll = millis();
  lastInputMs = millis();
  if (!hasCoords()) {
    showNeedCoords();
    return;
  }
  fetchPlanes();
  drawRadar();
  drawInfo();
}

void loop() {
  server.handleClient();
  if (otaActive) ArduinoOTA.handle();

  if (!hasCoords()) {
    delay(20);
    return;
  }

  bool needsRedraw = checkButton();

  noInterrupts();
  int delta = encoderDelta;
  encoderDelta = 0;
  interrupts();

  if (delta != 0) {
    lastInputMs = millis();
    if (currentMode == MODE_ZOOM) {
      rangeIndex += (delta > 0) ? 1 : -1;
      if (rangeIndex < 0) rangeIndex = 0;
      if (rangeIndex >= NUM_RANGES) rangeIndex = NUM_RANGES - 1;
      updateVisible();
    } else if (visibleCount > 0) {
      selectedIndex += (delta > 0) ? 1 : -1;
      if (selectedIndex < 0) selectedIndex = visibleCount - 1;
      if (selectedIndex >= visibleCount) selectedIndex = 0;
      selectedIcao = planes[selectedIndex].icao24;
      followNearest = false;
    }
    needsRedraw = true;
  }

  if (currentMode == MODE_SELECT && millis() - lastInputMs > SELECT_TIMEOUT_MS) {
    currentMode = MODE_ZOOM;
    followNearest = true;
    updateVisible();
    needsRedraw = true;
  }

  if (millis() - lastPoll > pollIntervalMs) {
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    fetchPlanes();
    lastPoll = millis();
    needsRedraw = true;
  }

  if (needsRedraw) {
    if (visibleCount > 0) {
      int idx = (selectedIndex < visibleCount) ? selectedIndex : 0;
      lookupAircraftType(planes[idx].icao24);
    }
    drawRadar();
    drawInfo();
    lastInfoRedraw = millis();
  } else if (infoPage == 2 && millis() - lastInfoRedraw > 1000) {
    drawInfo();
    lastInfoRedraw = millis();
  }

  delay(20);
}
