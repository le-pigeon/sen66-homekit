# SEN66 Air Quality Monitor

A DIY indoor air-quality monitor built around an **ESP32-S3** and **Sensirion SEN66**, with a local **SH1106 OLED display**, **Apple HomeKit integration**, RGB status indication, historical measurements, and Telnet diagnostics.

The project is designed to make use of as much of the SEN66's environmental data as practical while keeping the HomeKit interface limited to measurements with appropriate native HomeKit representations.

---

## Features

* Sensirion SEN66 environmental sensing
* PM1.0, PM2.5, PM4.0 and PM10 measurements
* CO₂ measurement
* Temperature and relative humidity
* VOC Index and NOx Index
* Apple HomeKit integration using HomeSpan
* 128×64 SH1106 OLED interface
* PM2.5 1-hour and 24-hour history
* CO₂ 5-minute history
* CO₂ 5-minute average
* PM2.5-based RGB status indication
* Telnet diagnostics
* Live sensor monitoring through Telnet
* Wi-Fi connectivity
* Measurements updated locally without requiring Home Assistant

---

## Hardware

| Component       | Description            |
| --------------- | ---------------------- |
| Microcontroller | YD-ESP32-S3            |
| Sensor          | Sensirion SEN66        |
| Display         | 128×64 SH1106 I²C OLED |
| HomeKit         | HomeSpan               |
| RGB LED         | Onboard WS2812         |
| Connectivity    | Wi-Fi                  |
| Diagnostics     | Telnet                 |

### GPIO Connections

The SEN66 and OLED use separate I²C buses.

| Device          | Signal | ESP32-S3 GPIO |
| --------------- | ------ | ------------: |
| SEN66           | SDA    |        GPIO 1 |
| SEN66           | SCL    |        GPIO 2 |
| OLED            | SDA    |        GPIO 6 |
| OLED            | SCL    |        GPIO 7 |
| Onboard RGB LED | Data   |       GPIO 48 |

#### SEN66

```text
SEN66       ESP32-S3
--------------------
SDA    ->   GPIO 1
SCL    ->   GPIO 2
3.3V   ->   3.3V
GND    ->   GND
```

#### OLED

```text
SH1106      ESP32-S3
--------------------
SDA    ->   GPIO 6
SCL    ->   GPIO 7
VCC    ->   3.3V
GND    ->   GND
```

The OLED uses software I²C so that the SEN66 can remain on the ESP32's hardware I²C interface.

---

## SEN66 Measurements

The SEN66 provides the following measurements used by this project:

| Measurement       | OLED | Apple Home |
| ----------------- | :--: | :--------: |
| PM1.0             |   ✓  |      —     |
| PM2.5             |   ✓  |      ✓     |
| PM4.0             |   ✓  |      —     |
| PM10              |   ✓  |      ✓     |
| CO₂               |   ✓  |      ✓     |
| Temperature       |   ✓  |      ✓     |
| Relative Humidity |   ✓  |      ✓     |
| VOC Index         |   ✓  |      —     |
| NOx Index         |   ✓  |      —     |

Not every SEN66 measurement has an appropriate native HomeKit representation.

PM1.0 and PM4.0 are therefore retained locally, while VOC and NOx are displayed as indices rather than being represented as concentration measurements.

---

## Apple HomeKit

HomeKit integration is provided directly by the ESP32 using [HomeSpan](https://github.com/HomeSpan/HomeSpan).

The project exposes:

#### Air Quality Sensor

* PM2.5 density
* PM10 density
* Air Quality classification

#### Carbon Dioxide Sensor

* CO₂ concentration
* Carbon dioxide detection status

#### Temperature Sensor

* Temperature

#### Humidity Sensor

* Relative humidity

The ESP32 connects directly to Apple HomeKit without requiring Home Assistant as an intermediary.

---

## PM2.5 Air Quality Classification

The project converts PM2.5 concentration into HomeKit's `AirQuality` characteristic using PM2.5 concentration breakpoints derived from the **U.S. Environmental Protection Agency (EPA) Air Quality Index (AQI)**.

The thresholds currently implemented are based on the **historical U.S. EPA PM2.5 AQI breakpoints**:

| PM2.5 (µg/m³) | HomeKit Air Quality |
| ------------: | ------------------- |
|        ≤ 12.0 | Excellent           |
|     12.1–35.4 | Good                |
|     35.5–55.4 | Fair                |
|    55.5–150.4 | Inferior            |
|       >150.4  | Poor                |

These thresholds are used by the firmware to determine the HomeKit `AirQuality` category. **HomeKit itself does not calculate this category from the PM2.5 value**; the ESP32 firmware performs the classification and sends the resulting category to HomeKit.

> **Note:** The EPA updated its PM2.5 AQI breakpoints in 2024. The current breakpoints differ from those implemented here, including a lower first breakpoint of 9.0 µg/m³ and an upper breakpoint of 125.4 µg/m³ for the fourth category. The values above are retained because they are the thresholds currently implemented by this project.
>
> **Source:** U.S. Environmental Protection Agency, *Technical Assistance Document for the Reporting of Daily Air Quality – the Air Quality Index (AQI)*.
> [EPA AQI Technical Assistance Document](https://document.airnow.gov/technical-assistance-document-for-the-reporting-of-daily-air-quailty.pdf)

---

## CO₂

CO₂ is exposed to HomeKit in two ways:

* Actual CO₂ concentration in ppm
* `CarbonDioxideDetected`

The firmware currently treats CO₂ concentrations below 2000 ppm as normal and concentrations at or above 2000 ppm as abnormal.

For example:

```text
CO₂ Level:     534 ppm
CO₂ Detected:  Normal
```

The `Detected` characteristic indicates whether the measured CO₂ level has crossed the firmware's abnormal threshold. It does not mean whether the sensor is capable of detecting CO₂.

---

## OLED Interface

The SH1106 OLED automatically cycles through four pages.

The page duration is controlled by:

```cpp
#define PAGE_INTERVAL_MS 5000
```

The default is **5 seconds per page**.

## Page 1 — PM2.5

Displays:

* Current PM2.5
* 1-hour average
* 24-hour average
* 1-hour PM2.5 graph

The graph uses the individual 10-second PM2.5 samples rather than 5-minute averages.

---

### Page 2 — CO₂

Displays:

* Current CO₂
* 5-minute average
* 5-minute CO₂ graph
* Ventilation status indicator

The project currently uses the 5-minute average to determine the display status:

```text
< 800 ppm   → VENT OK
≥ 800 ppm   → VENT HIGH
```

This is a project-defined indicator and is not intended to represent a formal ventilation certification.

---

### Page 3 — Particles

Displays all four particle-size measurements:

```text
PM1.0
PM2.5
PM4.0
PM10
```

Values are shown in:

```text
µg/m³
```

The page uses a two-column layout to make better use of the 128×64 display.

---

### Page 4 — Environment

Displays:

* Temperature
* Relative humidity
* VOC Index
* NOx Index

VOC and NOx are deliberately labelled as indices rather than concentrations.

---

## VOC Index and NOx Index

The SEN66 does not directly report VOC or NOx concentration in ppm through these values.

Instead, it provides:

* VOC Index
* NOx Index

These are relative indices generated by the sensor's gas-processing algorithms.

They are useful for observing changes in the environment but should not be interpreted as:

```text
VOC Index = VOC concentration
NOx Index = NO₂ concentration
```

For this reason, the project keeps these measurements on the local OLED rather than exposing them as concentration values in HomeKit.

---

## Measurement History

The ESP32 stores recent measurements in RAM.

### PM2.5

#### 1-hour history

```text
360 samples
×
10 seconds
=
1 hour
```

#### 24-hour history

```text
8640 samples
×
10 seconds
=
24 hours
```

The firmware calculates:

* 1-hour PM2.5 average
* 24-hour PM2.5 average

The 24-hour average is a simple arithmetic average of the stored PM2.5 measurements. It is **not an official air-quality index or regulatory metric**.

### CO₂

The CO₂ history contains:

```text
30 samples
×
10 seconds
=
5 minutes
```

The firmware calculates a rolling 5-minute CO₂ average.

Once the buffer is full, new measurements overwrite the oldest measurements.

#### Startup behaviour

The averages only use measurements that have actually been collected.

For example:

```text
1 reading   → average of 1
6 readings  → average of 6
30 readings → full 5-minute average
```

This prevents the initial zero-filled buffer from artificially lowering the average.

#### Data persistence

Historical data is currently stored in RAM.

**All historical data is lost when the ESP32 restarts or loses power.**

The firmware does not continuously write the history to flash.

---

## Sensor Sampling and Display Timing

The main timing values are defined near the top of the sketch:

```cpp
#define SENSOR_INTERVAL_MS 10000
#define OLED_UPDATE_INTERVAL_MS 250
#define PAGE_INTERVAL_MS 5000
```

| Setting                   |    Default | Purpose                  |
| ------------------------- | ---------: | ------------------------ |
| `SENSOR_INTERVAL_MS`      | 10 seconds | SEN66 measurement update |
| `OLED_UPDATE_INTERVAL_MS` |     250 ms | OLED redraw interval     |
| `PAGE_INTERVAL_MS`        |  5 seconds | Automatic page switching |

The OLED can therefore redraw much more frequently than the sensor is sampled.

---

## RGB Status LED

The onboard WS2812 RGB LED is controlled through GPIO 48.

The firmware uses low brightness levels to keep the indicator unobtrusive.

The RGB status is primarily based on PM2.5 conditions and startup/sensor states.

The LED can be controlled using:

```cpp
void setRGB(uint8_t r, uint8_t g, uint8_t b)
```

For example:

```cpp
setRGB(0, 2, 0);
```

sets a low-brightness green indication.

---

## Telnet Diagnostics

The ESP32 runs a Telnet server on:

```text
TCP port 23
```

The device's IP address can be used to connect:

```bash
telnet <ESP32-IP> 23
```

On systems without a Telnet client:

```bash
nc <ESP32-IP> 23
```

The Telnet server requires the password defined in `wifi_secrets.h`.

### Telnet Commands

| Command | Description |
|---|---|
| `help` | Lists available commands. |
| `status` | Shows device and sensor status. |
| `sensor` | Shows current SEN66 measurements. |
| `wifi` | Shows Wi-Fi information. |
| `memory` | Shows ESP32 memory usage. |
| `uptime` | Shows device uptime. |
| `co2history` | Shows the 5-minute CO₂ history buffer status. |
| `restart` | Restarts the ESP32. |

The `sensor` command also supports live monitoring, refreshing the readings every second.

For `co2history`, the buffer stores up to **30 readings**. Once full, the `Next index` identifies the entry that will be overwritten by the next measurement.

---

## Telnet Security

Telnet is an unencrypted protocol.

This project is intended for use on a trusted local network.

**Do not expose TCP port 23 directly to the public internet or configure port forwarding for the Telnet server.**

---

## Software

The project is developed using the Arduino framework.

### Main Libraries

* ESP32 Arduino Core
* SensirionI2cSen66
* U8g2
* HomeSpan

### Arduino IDE Configuration

The current board configuration is:

```text
Board:
ESP32S3 Dev Module

Flash Size:
16MB (128Mb)

PSRAM:
OPI PSRAM

Partition Scheme:
16M Flash (3MB APP/9.9MB FATFS)

USB CDC On Boot:
Enabled

Upload Speed:
921600
```

---

## Wi-Fi Configuration

Wi-Fi credentials are stored separately in:

```text
wifi_secrets.h
```

Example:

```cpp
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"
#define TERMINAL_PASSWORD "your-telnet-password"
```

The credentials file should not be committed to source control.

Add it to `.gitignore`:

```gitignore
wifi_secrets.h
.DS_Store
```

---

## Project Structure

The main firmware is contained in the Arduino `.ino` sketch.

The firmware is organized into several functional areas:

```text
Setup
│
├── Wi-Fi
├── SEN66
├── OLED
├── HomeKit
└── Telnet

Main Loop
│
├── HomeKit polling
├── Telnet handling
├── Sensor sampling
├── History storage
├── OLED page switching
└── OLED updates

Display
│
├── PM2.5
├── CO₂
├── Particles
└── Environment
```

---

## Known Limitations

* Historical data is lost after reboot.
* VOC and NOx values are indices rather than direct gas concentrations.
* PM1.0 and PM4.0 do not currently have dedicated HomeKit characteristics in this project.
* Telnet communication is unencrypted.
* The OLED is limited to 128×64 pixels.
* The CO₂ ventilation indicator is a project-defined threshold rather than a formal assessment.
* HomeKit presentation is ultimately determined by Apple's Home app and HomeKit characteristics rather than being fully customizable by the firmware.

---

