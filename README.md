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
2. Install the libraries *Adafruit GFX Library*, *Adafruit SSD1306*, *GFX Library for Arduino* (by Moon On Our Nation) and *ArduinoJson*.
3. Upload `flight_radar/flight_radar.ino`. Nothing needs to be edited in the sketch.
4. On first start the device opens the WiFi **FlightRadar-Setup** (no password). Connect with a phone or PC. The setup page opens automatically, otherwise open `http://192.168.4.1`.
5. Select the WiFi network, enter the password, latitude and longitude, and optionally the OpenSky client ID and secret and an OTA password. Save. The device restarts and connects.

The OpenSky credentials are an OAuth2 API client from your OpenSky account. Without them, requests are sent anonymously with lower rate limits.

To change settings later, hold the encoder while powering on, or open `http://flightradar.local/` in the home network. Settings are stored in flash. If the saved WiFi cannot be reached, the setup portal opens and the device restarts after 10 minutes to try again.

## Updates (OTA)

Set an OTA password (at least 6 characters) on the setup page. Without a password, OTA is disabled. The first OTA-capable firmware must be flashed over USB.

- Arduino IDE: select the network port `flightradar` under *Tools > Port*, upload as usual and enter the OTA password.
- Browser: open `http://flightradar.local/update`, log in as `admin` with the OTA password and upload the `.bin` exported with *Sketch > Export Compiled Binary*.

If the sketch does not fit the update partition, select *Tools > Partition Scheme > Minimal SPIFFS (1.9MB APP with OTA)* and flash once over USB.

## Controls

| Action | Effect |
|---|---|
| Turn (ZOOM) | Range: 10, 25, 50, 100 or 200 km |
| Short press | Switch between ZOOM and SELECT |
| Turn (SELECT) | Select an aircraft (red) |
| Long press (~1 s) | Next OLED page (1/3, 2/3, 3/3) |
| No input for 10 s (SELECT) | Back to ZOOM, nearest aircraft selected |
| Hold while powering on | Opens the setup portal |

The selection stays on the same aircraft between updates. Data is polled every 30 s and backs off for 3 minutes on HTTP 429.

## Notes

- The setup page and some OLED labels are in German.

## Guides

- [Guide (English)](docs/Flight_Radar_Guide_EN.pdf)
- [Anleitung (Deutsch)](docs/Flugradar_Anleitung_DE.pdf)
