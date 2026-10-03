# Flight Radar

ESP32 desk flight radar. A round GC9A01 display shows aircraft around a fixed location, a small SSD1306 OLED shows details of the selected aircraft, and a rotary encoder controls zoom, selection and OLED pages.

Aircraft positions come from the OpenSky Network API, aircraft types from hexdb.io.

## Hardware

- ESP32 DevKit (38 pin)
- GC9A01 1.28" round display, 240x240 (SPI)
- SSD1306 0.96" OLED, 128x64 (I2C, address 0x3C)
- EC11 rotary encoder with push button

## Wiring

All parts run at 3.3 V.

| Part | Part pin | ESP32 pin |
|---|---|---|
| GC9A01 | VCC / GND | 3V3 / GND |
| | SCL (SCK) | GPIO18 |
| | SDA (MOSI) | GPIO23 |
| | CS | GPIO15 |
| | DC | GPIO2 |
| | RES | GPIO4 |
| | BLK | 3V3 |
| SSD1306 | VCC / GND | 3V3 / GND |
| | SDA | GPIO21 |
| | SCL | GPIO22 |
| EC11 | + / GND | 3V3 / GND |
| | CLK | GPIO25 |
| | DT | GPIO26 |
| | SW | GPIO27 |

GPIO2 and GPIO15 are strapping pins. If an upload fails, disconnect both wires briefly.

## Setup

1. Install the ESP32 board package and select **ESP32 Dev Module**.
2. Install the libraries Adafruit GFX Library, Adafruit SSD1306, GFX Library for Arduino and ArduinoJson.
3. Open `flight_radar/flight_radar.ino` and set `WIFI_SSID`, `WIFI_PASSWORD`, `HOME_LAT`, `HOME_LON`, `OPENSKY_CLIENT_ID` and `OPENSKY_CLIENT_SECRET`.
4. Upload and open the Serial Monitor at 115200 baud.

The OpenSky credentials are an OAuth2 API client created in your OpenSky account. Do not commit them.

## Controls

| Action | Effect |
|---|---|
| Turn (ZOOM) | Range: 10, 25, 50, 100 or 200 km |
| Short press | Switch between ZOOM and SELECT |
| Turn (SELECT) | Select an aircraft (red) |
| Long press (~1 s) | Next OLED page (1/3, 2/3, 3/3) |
| No input for 10 s (SELECT) | Back to ZOOM, nearest aircraft selected |

The selection stays on the same aircraft between updates. Data is polled every 30 s and backs off for 3 minutes on HTTP 429.

## Notes

- Some labels on OLED pages 2 and 3 are in German (Betreiber, Statistik, Naechst, Kein Verkehr).
- WiFi credentials are compiled into the sketch.

## Guides

- [Guide (English)](docs/Flight_Radar_Guide_EN.pdf)
- [Anleitung (Deutsch)](docs/Flugradar_Anleitung_DE.pdf)
