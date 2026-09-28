/*
 * ================================================================
 * SEN66 Air Quality Monitor
 * ESP32-S3 + Sensirion SEN66 + SH1106 OLED
 * HomeSpan → Apple HomeKit
 * ================================================================
 *
 * SEN66 measurements:
 *   - PM1.0
 *   - PM2.5
 *   - PM4.0
 *   - PM10
 *   - CO2
 *   - Temperature
 *   - Relative Humidity
 *   - VOC Index
 *   - NOx Index
 *
 * OLED:
 *   Page 1 - PM2.5 graph
 *   Page 2 - CO2 graph
 *   Page 3 - Particle measurements
 *   Page 4 - Environment measurements
 *
 * HomeKit:
 *   - Air Quality
 *   - PM2.5
 *   - PM10
 *   - CO2
 *   - Temperature
 *   - Humidity
 *
 * RGB LED:
 *   Uses PM2.5 conditions to indicate air quality.
 *
 * Telnet:
 *   Port 23
 *   Provides simple diagnostic commands.
 *
 * ================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HomeSpan.h>
#include <SensirionI2cSen66.h>
#include <U8g2lib.h>

#include "wifi_secrets.h"


// ================================================================
// PIN CONFIGURATION
// ================================================================

// SEN66 uses the ESP32-S3 hardware I2C bus.
#define SEN66_SDA 1
#define SEN66_SCL 2

// OLED uses separate software I2C.
#define OLED_SDA 6
#define OLED_SCL 7

// SH1106 OLED I2C address.
#define OLED_ADDRESS 0x3C

// YD-ESP32-S3 onboard WS2812 RGB LED.
#define RGB_LED_PIN 48


// ================================================================
// TIMING
// ================================================================

// SEN66 read interval.
//
// 10 seconds:
//   6 samples/minute
//   360 samples/hour
//   8640 samples/day
#define SENSOR_INTERVAL_MS 10000

// OLED refresh interval.
#define OLED_UPDATE_INTERVAL_MS 250

// Automatically change OLED page every 5 seconds.
#define PAGE_INTERVAL_MS 7000


// ================================================================
// HISTORY BUFFER SIZES
// ================================================================

// PM2.5 history for one hour.
#define PM25_1H_SAMPLES 360

// PM2.5 history for 24 hours.
#define PM25_24H_SAMPLES 8640

// CO2 history for five minutes.
//
// 30 samples × 10 seconds = 300 seconds = 5 minutes.
#define CO2_5M_SAMPLES 30


// ================================================================
// SENSOR OBJECT
// ================================================================

SensirionI2cSen66 sen66;


// ================================================================
// OLED
// ================================================================
//
// Software I2C on GPIO6 and GPIO7.
//
// GPIO1/2 remain dedicated to the SEN66 hardware I2C bus.
//

U8G2_SH1106_128X64_NONAME_F_SW_I2C oled(
  U8G2_R0,
  OLED_SCL,
  OLED_SDA,
  U8X8_PIN_NONE
);


// ================================================================
// SENSOR VALUES
// ================================================================
//
// These are the latest sensor readings.
//

float pm1 = 0.0;
float pm25 = 0.0;
float pm4 = 0.0;
float pm10 = 0.0;

// Keep the project-level CO2 variable as float.
// The SEN66 library itself receives a separate uint16_t
// temporary variable called co2Raw.
float co2 = 0.0;

float temperature = 0.0;
float humidity = 0.0;

float vocIndex = 0.0;
float noxIndex = 0.0;


// Indicates whether the SEN66 currently has a valid reading.
bool sensorOK = false;


// ================================================================
// PM2.5 HISTORY
// ================================================================

// One-hour PM2.5 history.
float pm25History1H[PM25_1H_SAMPLES];

int pm25History1HIndex = 0;
int pm25History1HCount = 0;


// 24-hour PM2.5 history.
float pm25History24H[PM25_24H_SAMPLES];

int pm25History24HIndex = 0;
int pm25History24HCount = 0;


// ================================================================
// CO2 5-MINUTE HISTORY
// ================================================================

// Circular buffer containing the latest 30 CO2 readings.
//
// Once full, each new reading overwrites the oldest reading.
float co2History5M[CO2_5M_SAMPLES];

int co2History5MIndex = 0;
int co2History5MCount = 0;


// ================================================================
// AVERAGES
// ================================================================

float pm25Average1H = 0.0;
float pm25Average24H = 0.0;
float co2Average5M = 0.0;


// ================================================================
// OLED PAGE
// ================================================================

int currentPage = 0;

unsigned long lastSensorRead = 0;
unsigned long lastOLEDUpdate = 0;
unsigned long lastPageChange = 0;


// ================================================================
// TELNET
// ================================================================

WiFiServer telnetServer(23);

WiFiClient telnetClient;

bool telnetAuthenticated = false;

int telnetLoginAttempts = 0;

String telnetInput = "";


// ================================================================
// HOMESPAN / HOMEKIT CHARACTERISTICS
// ================================================================

SpanCharacteristic *homeAirQuality;

SpanCharacteristic *homePM25;
SpanCharacteristic *homePM10;

SpanCharacteristic *homeCO2Detected;
SpanCharacteristic *homeCO2Level;

SpanCharacteristic *homeTemperature;

SpanCharacteristic *homeHumidity;


// ================================================================
// RGB LED
// ================================================================

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_LED_PIN, r, g, b);
}


void rgbOff() {
  setRGB(0, 0, 0);
}


// ================================================================
// PM2.5 → HOMEKIT AIR QUALITY
// ================================================================

void updateHomeKitAirQuality(float value) {

  if (!homeAirQuality) {
    return;
  }

  if (value <= 12.0) {

    homeAirQuality->setVal(
      Characteristic::AirQuality::EXCELLENT
    );

  }
  else if (value <= 35.4) {

    homeAirQuality->setVal(
      Characteristic::AirQuality::GOOD
    );

  }
  else if (value <= 55.4) {

    homeAirQuality->setVal(
      Characteristic::AirQuality::FAIR
    );

  }
  else if (value <= 150.4) {

    homeAirQuality->setVal(
      Characteristic::AirQuality::INFERIOR
    );

  }
  else {

    homeAirQuality->setVal(
      Characteristic::AirQuality::POOR
    );
  }
}


// ================================================================
// ADD PM2.5 TO 1-HOUR HISTORY
// ================================================================

void addPM25History1H(float value) {

  pm25History1H[pm25History1HIndex] = value;

  pm25History1HIndex++;

  if (pm25History1HIndex >= PM25_1H_SAMPLES) {
    pm25History1HIndex = 0;
  }

  if (pm25History1HCount < PM25_1H_SAMPLES) {
    pm25History1HCount++;
  }
}


// ================================================================
// ADD PM2.5 TO 24-HOUR HISTORY
// ================================================================

void addPM25History24H(float value) {

  pm25History24H[pm25History24HIndex] = value;

  pm25History24HIndex++;

  if (pm25History24HIndex >= PM25_24H_SAMPLES) {
    pm25History24HIndex = 0;
  }

  if (pm25History24HCount < PM25_24H_SAMPLES) {
    pm25History24HCount++;
  }
}


// ================================================================
// CALCULATE PM2.5 1-HOUR AVERAGE
// ================================================================

float getPM25Average1H() {

  if (pm25History1HCount == 0) {
    return 0.0;
  }

  float sum = 0.0;

  for (int i = 0; i < pm25History1HCount; i++) {
    sum += pm25History1H[i];
  }

  return sum / pm25History1HCount;
}


// ================================================================
// CALCULATE PM2.5 24-HOUR AVERAGE
// ================================================================

float getPM25Average24H() {

  if (pm25History24HCount == 0) {
    return 0.0;
  }

  float sum = 0.0;

  for (int i = 0; i < pm25History24HCount; i++) {
    sum += pm25History24H[i];
  }

  return sum / pm25History24HCount;
}


// ================================================================
// ADD CO2 TO 5-MINUTE HISTORY
// ================================================================
//
// Circular buffer.
//
// Reading 1  -> buffer[0]
// Reading 2  -> buffer[1]
// ...
// Reading 30 -> buffer[29]
// Reading 31 -> buffer[0], replacing reading 1
//
// co2History5MCount tells us how many valid readings exist.
//
// co2History5MIndex tells us where the NEXT reading will go.
//

void addCO2History(float value) {

  // Store newest reading.
  co2History5M[co2History5MIndex] = value;

  // Advance write position.
  co2History5MIndex++;

  // Wrap around.
  if (co2History5MIndex >= CO2_5M_SAMPLES) {
    co2History5MIndex = 0;
  }

  // Increase valid count until buffer is full.
  if (co2History5MCount < CO2_5M_SAMPLES) {
    co2History5MCount++;
  }
}


// ================================================================
// CALCULATE CO2 5-MINUTE AVERAGE
// ================================================================

float getCO2Average5M() {

  if (co2History5MCount == 0) {
    return 0.0;
  }

  float sum = 0.0;

  for (int i = 0; i < co2History5MCount; i++) {
    sum += co2History5M[i];
  }

  return sum / co2History5MCount;
}


// ================================================================
// UPDATE HISTORY AND AVERAGES
// ================================================================

void storeHistorySample() {

  addPM25History1H(pm25);

  addPM25History24H(pm25);

  addCO2History(co2);

  pm25Average1H = getPM25Average1H();

  pm25Average24H = getPM25Average24H();

  co2Average5M = getCO2Average5M();
}


// ================================================================
// RGB STATUS
// ================================================================

void updateRGBStatus() {

  // During startup, show very dim blue.
  if (pm25History1HCount < 30) {

    setRGB(0, 0, 2);

    return;
  }


  float value = pm25Average1H;


  if (value <= 12.0) {

    setRGB(0, 2, 0);

  }
  else if (value <= 35.4) {

    setRGB(2, 2, 0);

  }
  else if (value <= 55.4) {

    setRGB(2, 1, 0);

  }
  else if (value <= 150.4) {

    setRGB(3, 0, 0);

  }
  else {

    setRGB(5, 0, 0);
  }
}


// ================================================================
// READ SEN66
// ================================================================

void readSEN66() {

  uint16_t error = 0;

  char errorMessage[256];


  // ------------------------------------------------------------
  // Temporary variables used by the SEN66 library.
  // ------------------------------------------------------------

  float massConcentrationPm1p0;
  float massConcentrationPm2p5;
  float massConcentrationPm4p0;
  float massConcentrationPm10p0;

  float ambientHumidity;
  float ambientTemperature;

  float vocIndexValue;
  float noxIndexValue;

  // IMPORTANT:
  //
  // Sensirion's SEN66 Arduino library expects the CO2 output
  // parameter to be uint16_t.
  //
  // We therefore use co2Raw here rather than the global float
  // variable called co2.
  uint16_t co2Raw = 0;


  // ------------------------------------------------------------
  // Read the latest measurement from the SEN66.
  // ------------------------------------------------------------

  error = sen66.readMeasuredValues(

    massConcentrationPm1p0,

    massConcentrationPm2p5,

    massConcentrationPm4p0,

    massConcentrationPm10p0,

    ambientHumidity,

    ambientTemperature,

    vocIndexValue,

    noxIndexValue,

    co2Raw
  );


  // ------------------------------------------------------------
  // Check for SEN66 error.
  // ------------------------------------------------------------

  if (error != 0) {

    sensorOK = false;

    errorToString(
      error,
      errorMessage,
      sizeof(errorMessage)
    );

    Serial.print("SEN66 read error: ");
    Serial.println(errorMessage);

    return;
  }


  // ------------------------------------------------------------
  // Store sensor values in global variables.
  // ------------------------------------------------------------

  pm1 = massConcentrationPm1p0;

  pm25 = massConcentrationPm2p5;

  pm4 = massConcentrationPm4p0;

  pm10 = massConcentrationPm10p0;

  humidity = ambientHumidity;

  temperature = ambientTemperature;

  vocIndex = vocIndexValue;

  noxIndex = noxIndexValue;

  // Convert the SEN66 uint16_t CO2 result into our global float.
  co2 = (float)co2Raw;


  // Valid sensor reading.
  sensorOK = true;


  // ------------------------------------------------------------
  // Store in history buffers.
  // ------------------------------------------------------------

  storeHistorySample();


  // ------------------------------------------------------------
  // Update HomeKit instantaneous values.
  // ------------------------------------------------------------

  if (homePM25) {
    homePM25->setVal(pm25);
  }

  if (homePM10) {
    homePM10->setVal(pm10);
  }

  if (homeCO2Level) {
    homeCO2Level->setVal(co2);
  }

  if (homeTemperature) {
    homeTemperature->setVal(temperature);
  }

  if (homeHumidity) {
    homeHumidity->setVal(humidity);
  }


  // Update categorical HomeKit air quality.
  updateHomeKitAirQuality(pm25);


  // ------------------------------------------------------------
  // HomeKit CO2 abnormal status.
  // ------------------------------------------------------------

  if (homeCO2Detected) {

    if (co2 >= 2000.0) {

      homeCO2Detected->setVal(
        Characteristic::CarbonDioxideDetected::ABNORMAL
      );

    }
    else {

      homeCO2Detected->setVal(
        Characteristic::CarbonDioxideDetected::NORMAL
      );
    }
  }


  // ------------------------------------------------------------
  // Update RGB LED.
  // ------------------------------------------------------------

  updateRGBStatus();


  // ------------------------------------------------------------
  // Serial diagnostics.
  // ------------------------------------------------------------

  Serial.println();
  Serial.println("----- SEN66 -----");

  Serial.print("PM1.0:       ");
  Serial.print(pm1);
  Serial.println(" ug/m3");

  Serial.print("PM2.5:       ");
  Serial.print(pm25);
  Serial.println(" ug/m3");

  Serial.print("PM4.0:       ");
  Serial.print(pm4);
  Serial.println(" ug/m3");

  Serial.print("PM10:        ");
  Serial.print(pm10);
  Serial.println(" ug/m3");

  Serial.print("CO2:         ");
  Serial.print(co2);
  Serial.println(" ppm");

  Serial.print("CO2 5M AVG:  ");

  if (co2History5MCount >= CO2_5M_SAMPLES) {

    Serial.print(co2Average5M);
    Serial.println(" ppm");

  }
  else {

    Serial.print("warming up (");
    Serial.print(co2History5MCount);
    Serial.print("/");
    Serial.print(CO2_5M_SAMPLES);
    Serial.println(")");
  }

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" C");

  Serial.print("Humidity:    ");
  Serial.print(humidity);
  Serial.println(" %");

  Serial.print("VOC Index:   ");
  Serial.println(vocIndex);

  Serial.print("NOx Index:   ");
  Serial.println(noxIndex);

  Serial.println("-----------------");
}


// ================================================================
// DRAW GRAPH
// ================================================================

void drawGraph(
  float *values,
  int count,
  int maxSamples,
  float maxValue,
  int x,
  int y,
  int width,
  int height
) {

  oled.drawFrame(
    x,
    y,
    width,
    height
  );


  if (count < 2) {
    return;
  }


  int samplesToDraw =
    min(count, maxSamples);


  if (samplesToDraw < 2) {
    return;
  }


  int startIndex =
    count - samplesToDraw;


  for (int i = 1; i < samplesToDraw; i++) {

    float previousValue =
      values[startIndex + i - 1];

    float currentValue =
      values[startIndex + i];


    previousValue =
      constrain(
        previousValue,
        0.0,
        maxValue
      );

    currentValue =
      constrain(
        currentValue,
        0.0,
        maxValue
      );


    int x1 =
      x +
      ((i - 1) * (width - 2)) /
      (samplesToDraw - 1);

    int x2 =
      x +
      (i * (width - 2)) /
      (samplesToDraw - 1);


    int y1 =
      y +
      height -
      2 -
      (int)(
        (previousValue / maxValue) *
        (height - 3)
      );

    int y2 =
      y +
      height -
      2 -
      (int)(
        (currentValue / maxValue) *
        (height - 3)
      );


    oled.drawLine(
      x1,
      y1,
      x2,
      y2
    );
  }
}


// ================================================================
// GET CHRONOLOGICAL PM2.5 HISTORY
// ================================================================

float getPM25History1H(int chronologicalIndex) {

  if (pm25History1HCount == 0) {
    return 0.0;
  }


  int oldestIndex;


  if (pm25History1HCount < PM25_1H_SAMPLES) {

    oldestIndex = 0;

  }
  else {

    oldestIndex = pm25History1HIndex;
  }


  int actualIndex =
    (
      oldestIndex +
      chronologicalIndex
    ) % PM25_1H_SAMPLES;


  return pm25History1H[actualIndex];
}


// ================================================================
// DRAW PM2.5 GRAPH
// ================================================================

void drawPM25Graph() {

  const int graphX = 0;
  const int graphY = 30;

  const int graphWidth = 128;
  const int graphHeight = 34;


  oled.drawFrame(
    graphX,
    graphY,
    graphWidth,
    graphHeight
  );


  // PM2.5 reference at 35.4 ug/m3.
  float reference = 35.4;


  float graphMax = 100.0;


  if (pm25Average1H > 80.0) {

    graphMax =
      ceil(
        pm25Average1H / 20.0
      ) * 20.0;
  }


  int referenceY =
    graphY +
    graphHeight -
    2 -
    (int)(
      (reference / graphMax) *
      (graphHeight - 3)
    );


  if (
    referenceY > graphY &&
    referenceY < graphY + graphHeight
  ) {

    for (
      int x = graphX + 2;
      x < graphX + graphWidth - 2;
      x += 4
    ) {

      oled.drawPixel(
        x,
        referenceY
      );
    }
  }


  // Copy chronological history into display buffer.
  float displayValues[128];

  int displayCount =
    min(
      pm25History1HCount,
      128
    );


  for (int i = 0; i < displayCount; i++) {

    int sourceIndex =
      (
        i *
        pm25History1HCount
      ) /
      displayCount;


    displayValues[i] =
      getPM25History1H(
        sourceIndex
      );
  }


  drawGraph(
    displayValues,
    displayCount,
    displayCount,
    graphMax,
    graphX,
    graphY,
    graphWidth,
    graphHeight
  );
}


// ================================================================
// DRAW CO2 GRAPH
// ================================================================

void drawCO2Graph() {

  const int graphX = 0;
  const int graphY = 30;

  const int graphWidth = 128;
  const int graphHeight = 34;


  float graphMax = 2000.0;


  if (co2 > graphMax) {

    graphMax =
      ceil(
        co2 / 500.0
      ) * 500.0;
  }


  if (co2Average5M > graphMax) {

    graphMax =
      ceil(
        co2Average5M / 500.0
      ) * 500.0;
  }


  oled.drawFrame(
    graphX,
    graphY,
    graphWidth,
    graphHeight
  );


  // 800 ppm reference.
  float reference = 800.0;


  int referenceY =
    graphY +
    graphHeight -
    2 -
    (int)(
      (reference / graphMax) *
      (graphHeight - 3)
    );


  if (
    referenceY > graphY &&
    referenceY < graphY + graphHeight
  ) {

    for (
      int x = graphX + 2;
      x < graphX + graphWidth - 2;
      x += 4
    ) {

      oled.drawPixel(
        x,
        referenceY
      );
    }
  }


  // Copy chronological CO2 history into display buffer.
  float displayValues[128];

  int displayCount =
    min(
      co2History5MCount,
      128
    );


  for (int i = 0; i < displayCount; i++) {

    int sourceIndex =
      (
        i *
        co2History5MCount
      ) /
      displayCount;


    // Determine oldest reading.
    int oldestIndex;


    if (
      co2History5MCount <
      CO2_5M_SAMPLES
    ) {

      oldestIndex = 0;

    }
    else {

      oldestIndex =
        co2History5MIndex;
    }


    // Convert chronological index to actual buffer index.
    int actualIndex =
      (
        oldestIndex +
        sourceIndex
      ) % CO2_5M_SAMPLES;


    displayValues[i] =
      co2History5M[actualIndex];
  }


  drawGraph(
    displayValues,
    displayCount,
    displayCount,
    graphMax,
    graphX,
    graphY,
    graphWidth,
    graphHeight
  );
}


// ================================================================
// OLED PAGE 1 — PM2.5 GRAPH
// ================================================================

void drawPagePM25() {

  oled.clearBuffer();


  oled.setFont(
    u8g2_font_6x10_tf
  );

  oled.drawStr(
    0,
    9,
    "PM2.5"
  );


  oled.setFont(
    u8g2_font_7x13B_tf
  );

  char buffer[32];


  snprintf(
    buffer,
    sizeof(buffer),
    "%.1f ug/m3",
    pm25
  );


  oled.drawStr(
    55,
    10,
    buffer
  );


  oled.setFont(
    u8g2_font_5x8_tf
  );


  snprintf(
    buffer,
    sizeof(buffer),
    "1H %.1f",
    pm25Average1H
  );


  oled.drawStr(
    0,
    21,
    buffer
  );


  snprintf(
    buffer,
    sizeof(buffer),
    "24H %.1f",
    pm25Average24H
  );


  oled.drawStr(
    45,
    21,
    buffer
  );


  snprintf(
    buffer,
    sizeof(buffer),
    "%d",
    pm25History1HCount
  );


  oled.drawStr(
    105,
    21,
    buffer
  );


  drawPM25Graph();


  oled.sendBuffer();
}


// ================================================================
// OLED PAGE 2 — CO2 GRAPH
// ================================================================

void drawPageCO2() {

  oled.clearBuffer();


  oled.setFont(
    u8g2_font_6x10_tf
  );


  oled.drawStr(
    0,
    9,
    "CO2"
  );


  oled.setFont(
    u8g2_font_7x13B_tf
  );


  char buffer[32];


  snprintf(
    buffer,
    sizeof(buffer),
    "%.0f ppm",
    co2
  );


  oled.drawStr(
    55,
    10,
    buffer
  );


  oled.setFont(
    u8g2_font_5x8_tf
  );


  if (
    co2History5MCount >=
    CO2_5M_SAMPLES
  ) {

    snprintf(
      buffer,
      sizeof(buffer),
      "5M %.0f",
      co2Average5M
    );


    oled.drawStr(
      0,
      21,
      buffer
    );


    if (co2Average5M >= 1500.0) {

      oled.drawStr(
        62,
        21,
        "VENT HIGH"
      );

    }
    else {

      oled.drawStr(
        62,
        21,
        "VENT OK"
      );
    }

  }
  else {

    oled.drawStr(
      0,
      21,
      "5M --"
    );


    oled.drawStr(
      62,
      21,
      "WARMING"
    );
  }


  drawCO2Graph();


  oled.sendBuffer();
}


// ================================================================
// OLED PAGE 3 — PARTICLES
// ================================================================

void drawPageParticles() {

  oled.clearBuffer();

  // Title
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawUTF8(0, 9, "PARTICLES (µg/m3)");

  // Horizontal divider below title
  oled.drawHLine(0, 11, 128);

  // Labels
  oled.setFont(u8g2_font_5x8_tf);

  oled.drawStr(2, 21, "PM1.0");
  oled.drawStr(66, 21, "PM4.0");

  // Values
  oled.setFont(u8g2_font_6x10_tf);

  oled.setCursor(2, 32);
  oled.print(pm1, 1);

  oled.setCursor(66, 32);
  oled.print(pm4, 1);

  // Divider between rows
  oled.drawHLine(0, 36, 128);

  // Bottom labels
  oled.setFont(u8g2_font_5x8_tf);

  oled.drawStr(2, 46, "PM2.5");
  oled.drawStr(66, 46, "PM10");

  // Bottom values
  oled.setFont(u8g2_font_6x10_tf);

  oled.setCursor(2, 58);
  oled.print(pm25, 1);

  oled.setCursor(66, 58);
  oled.print(pm10, 1);

  oled.sendBuffer();
}


// ================================================================
// OLED PAGE 4 — ENVIRONMENT
// ================================================================

void drawPageEnvironment() {

  oled.clearBuffer();

  // Title
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "ENVIRONMENT");

  // Temperature
  oled.setFont(u8g2_font_7x14_tf);
  oled.setCursor(0, 26);
  oled.print("TEMP ");
  oled.print(temperature, 1);
  oled.print(" C");

  // Humidity
  oled.setCursor(0, 42);
  oled.print("RH   ");
  oled.print(humidity, 1);
  oled.print(" %");

  // VOC
  oled.setCursor(0, 58);
  oled.print("VOC  ");
  oled.print(vocIndex, 0);

  // NOx
  oled.setCursor(75, 58);
  oled.print("NOx ");
  oled.print(noxIndex, 0);

  oled.sendBuffer();
}


// ================================================================
// UPDATE OLED
// ================================================================

void updateOLED() {

  if (!sensorOK) {

    oled.clearBuffer();


    oled.setFont(
      u8g2_font_6x10_tf
    );


    oled.drawStr(
      20,
      28,
      "SEN66"
    );


    oled.drawStr(
      20,
      42,
      "WARMING..."
    );


    oled.sendBuffer();

    return;
  }


  switch (currentPage) {

    case 0:
      drawPagePM25();
      break;

    case 1:
      drawPageCO2();
      break;

    case 2:
      drawPageParticles();
      break;

    case 3:
      drawPageEnvironment();
      break;
  }
}


// ================================================================
// TELNET HELP
// ================================================================

void telnetPrintHelp() {

  telnetClient.println();

  telnetClient.println(
    "SEN66 Air Quality Monitor"
  );

  telnetClient.println(
    "--------------------------------"
  );

  telnetClient.println(
    "help       - Show this help"
  );

  telnetClient.println(
    "status     - Show system status"
  );

  telnetClient.println(
    "sensor     - Show sensor readings"
  );

  telnetClient.println(
    "wifi       - Show WiFi information"
  );

  telnetClient.println(
    "memory     - Show memory usage"
  );

  telnetClient.println(
    "uptime     - Show uptime"
  );

  telnetClient.println(
    "co2history - Show CO2 buffer status"
  );

  telnetClient.println(
    "restart    - Restart ESP32"
  );

  telnetClient.println(
    "--------------------------------"
  );

  telnetClient.println();
}


// ================================================================
// TELNET STATUS
// ================================================================

void telnetPrintStatus() {

  telnetClient.println();

  telnetClient.println("STATUS");
  telnetClient.println("------");


  telnetClient.print("Sensor: ");

  if (sensorOK) {
    telnetClient.println("OK");
  }
  else {
    telnetClient.println("ERROR");
  }


  telnetClient.print("PM2.5: ");
  telnetClient.print(pm25);
  telnetClient.println(" ug/m3");


  telnetClient.print("PM2.5 1H AVG: ");
  telnetClient.print(pm25Average1H);
  telnetClient.println(" ug/m3");


  telnetClient.print("PM2.5 24H AVG: ");
  telnetClient.print(pm25Average24H);
  telnetClient.println(" ug/m3");


  telnetClient.print("CO2: ");
  telnetClient.print(co2);
  telnetClient.println(" ppm");


  if (
    co2History5MCount >=
    CO2_5M_SAMPLES
  ) {

    telnetClient.print(
      "CO2 5M AVG: "
    );

    telnetClient.print(
      co2Average5M
    );

    telnetClient.println(
      " ppm"
    );

  }
  else {

    telnetClient.println(
      "CO2 5M AVG: warming up"
    );
  }


  telnetClient.println();
}


// ================================================================
// TELNET SENSOR
// ================================================================

void telnetPrintSensor() {

  telnetClient.println();

  telnetClient.println("SENSOR");
  telnetClient.println("------");


  telnetClient.print("PM1.0: ");
  telnetClient.print(pm1);
  telnetClient.println(" ug/m3");


  telnetClient.print("PM2.5: ");
  telnetClient.print(pm25);
  telnetClient.println(" ug/m3");


  telnetClient.print("PM4.0: ");
  telnetClient.print(pm4);
  telnetClient.println(" ug/m3");


  telnetClient.print("PM10: ");
  telnetClient.print(pm10);
  telnetClient.println(" ug/m3");


  telnetClient.print("CO2: ");
  telnetClient.print(co2);
  telnetClient.println(" ppm");


  telnetClient.print("Temperature: ");
  telnetClient.print(temperature);
  telnetClient.println(" C");


  telnetClient.print("Humidity: ");
  telnetClient.print(humidity);
  telnetClient.println(" %");


  telnetClient.print("VOC Index: ");
  telnetClient.println(vocIndex);


  telnetClient.print("NOx Index: ");
  telnetClient.println(noxIndex);


  telnetClient.println();
}


// ================================================================
// TELNET WIFI
// ================================================================

void telnetPrintWifi() {

  telnetClient.println();

  telnetClient.println("WIFI");
  telnetClient.println("----");


  telnetClient.print("SSID: ");
  telnetClient.println(
    WiFi.SSID()
  );


  telnetClient.print("IP: ");
  telnetClient.println(
    WiFi.localIP()
  );


  telnetClient.print("RSSI: ");
  telnetClient.print(
    WiFi.RSSI()
  );

  telnetClient.println(
    " dBm"
  );


  telnetClient.println();
}


// ================================================================
// TELNET MEMORY
// ================================================================

void telnetPrintMemory() {

  telnetClient.println();

  telnetClient.println("MEMORY");
  telnetClient.println("------");


  telnetClient.print(
    "Free heap: "
  );

  telnetClient.print(
    ESP.getFreeHeap()
  );

  telnetClient.println(
    " bytes"
  );


  telnetClient.print(
    "Free PSRAM: "
  );

  telnetClient.print(
    ESP.getFreePsram()
  );

  telnetClient.println(
    " bytes"
  );


  telnetClient.println();
}


// ================================================================
// TELNET UPTIME
// ================================================================

void telnetPrintUptime() {

  unsigned long seconds =
    millis() / 1000;


  unsigned long minutes =
    seconds / 60;


  seconds %= 60;


  unsigned long hours =
    minutes / 60;


  minutes %= 60;


  telnetClient.print(
    "Uptime: "
  );


  telnetClient.print(
    hours
  );

  telnetClient.print(
    "h "
  );


  telnetClient.print(
    minutes
  );

  telnetClient.print(
    "m "
  );


  telnetClient.print(
    seconds
  );

  telnetClient.println(
    "s"
  );
}


// ================================================================
// TELNET CO2 HISTORY
// ================================================================
//
// Shows the state of the 30-entry circular CO2 buffer.
//
// Count:
//   Number of valid readings currently stored.
//
// Next index:
//   The array position where the NEXT reading will be written.
//
// Once Count reaches 30, Next index is also the position
// containing the oldest reading that will be overwritten next.
//

void telnetPrintCO2History() {

  telnetClient.println();

  telnetClient.println(
    "CO2 5-MIN HISTORY"
  );

  telnetClient.println(
    "------------------"
  );


  telnetClient.print(
    "Count: "
  );

  telnetClient.print(
    co2History5MCount
  );

  telnetClient.print(
    " / "
  );

  telnetClient.println(
    CO2_5M_SAMPLES
  );


  telnetClient.print(
    "Next index: "
  );

  telnetClient.println(
    co2History5MIndex
  );


  if (
    co2History5MCount >=
    CO2_5M_SAMPLES
  ) {

    telnetClient.println(
      "Buffer: FULL"
    );

    telnetClient.print(
      "Oldest slot: "
    );

    telnetClient.println(
      co2History5MIndex
    );

  }
  else {

    telnetClient.println(
      "Buffer: WARMING UP"
    );
  }


  telnetClient.print(
    "5M average: "
  );

  telnetClient.print(
    co2Average5M
  );

  telnetClient.println(
    " ppm"
  );


  telnetClient.println();
}


// ================================================================
// PROCESS TELNET COMMAND
// ================================================================

void processTelnetCommand(
  String command
) {

  command.trim();

  command.toLowerCase();


  if (command == "help") {

    telnetPrintHelp();

  }

  else if (command == "status") {

    telnetPrintStatus();

  }

  else if (command == "sensor") {

    telnetPrintSensor();

  }

  else if (command == "wifi") {

    telnetPrintWifi();

  }

  else if (command == "memory") {

    telnetPrintMemory();

  }

  else if (command == "uptime") {

    telnetPrintUptime();

  }

  else if (command == "co2history") {

    telnetPrintCO2History();

  }

  else if (command == "restart") {

    telnetClient.println(
      "Restarting..."
    );

    delay(500);

    ESP.restart();

  }

  else if (command.length() > 0) {

    telnetClient.println(
      "Unknown command. Type 'help'."
    );
  }
}


// ================================================================
// HANDLE TELNET
// ================================================================

void handleTelnet() {

  // Check for a new client if we don't currently
  // have an authenticated/connected client.
  if (
    !telnetClient ||
    !telnetClient.connected()
  ) {

    telnetAuthenticated = false;

    telnetLoginAttempts = 0;


    WiFiClient newClient =
      telnetServer.available();


    if (newClient) {

      telnetClient = newClient;


      telnetClient.println();

      telnetClient.println(
        "SEN66 Air Quality Monitor"
      );

      telnetClient.println(
        "Password:"
      );


      telnetInput = "";
    }


    return;
  }


  while (telnetClient.available()) {

    char c =
      telnetClient.read();


    // ------------------------------------------------------------
    // Password entry
    // ------------------------------------------------------------

    if (!telnetAuthenticated) {

      if (
        c == '\n' ||
        c == '\r'
      ) {

        String password =
          telnetInput;


        telnetInput = "";


        if (
          password ==
          TERMINAL_PASSWORD
        ) {

          telnetAuthenticated = true;


          telnetClient.println();

          telnetClient.println(
            "Login successful."
          );


          telnetPrintHelp();

        }
        else {

          telnetLoginAttempts++;


          telnetClient.println(
            "Incorrect password."
          );


          if (
            telnetLoginAttempts >= 3
          ) {

            telnetClient.println(
              "Too many attempts."
            );


            telnetClient.stop();

          }
          else {

            telnetClient.println(
              "Password:"
            );
          }
        }

      }
      else {

        if (
          c >= 32 &&
          c <= 126
        ) {

          telnetInput += c;
        }
      }


      continue;
    }


    // ------------------------------------------------------------
    // Authenticated command entry
    // ------------------------------------------------------------

    if (
      c == '\n' ||
      c == '\r'
    ) {

      processTelnetCommand(
        telnetInput
      );


      telnetInput = "";

    }
    else {

      if (
        c >= 32 &&
        c <= 126
      ) {

        telnetInput += c;

        telnetClient.print(c);
      }
    }
  }
}


// ================================================================
// HOMESPAN AIR QUALITY SERVICE
// ================================================================

struct AirQualityService :
  Service::AirQualitySensor {

  AirQualityService() {

    homeAirQuality =
      new Characteristic::AirQuality();


    homePM25 =
      new Characteristic::PM25Density();


    homePM10 =
      new Characteristic::PM10Density();
  }


  boolean update() override {

    return true;
  }
};


// ================================================================
// HOMESPAN CO2 SERVICE
// ================================================================

struct CO2Service :
  Service::CarbonDioxideSensor {

  CO2Service() {

    homeCO2Level =
      new Characteristic::CarbonDioxideLevel();


    homeCO2Detected =
      new Characteristic::CarbonDioxideDetected();
  }


  boolean update() override {

    return true;
  }
};


// ================================================================
// HOMESPAN TEMPERATURE SERVICE
// ================================================================

struct TemperatureService :
  Service::TemperatureSensor {

  TemperatureService() {

    homeTemperature =
      new Characteristic::CurrentTemperature();
  }


  boolean update() override {

    return true;
  }
};


// ================================================================
// HOMESPAN HUMIDITY SERVICE
// ================================================================

struct HumidityService :
  Service::HumiditySensor {

  HumidityService() {

    homeHumidity =
      new Characteristic::CurrentRelativeHumidity();
  }


  boolean update() override {

    return true;
  }
};


// ================================================================
// SETUP
// ================================================================

void setup() {

  Serial.begin(115200);

  delay(1000);


  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    " SEN66 Air Quality Monitor"
  );

  Serial.println(
    "================================"
  );


  // ------------------------------------------------------------
  // RGB LED
  // ------------------------------------------------------------

  rgbOff();


  // ------------------------------------------------------------
  // SEN66 I2C
  // ------------------------------------------------------------

  Serial.println(
    "Starting SEN66 I2C..."
  );


  Wire.begin(
    SEN66_SDA,
    SEN66_SCL,
    100000
  );


  sen66.begin(
    Wire,
    0x6B
  );


  // ------------------------------------------------------------
  // SEN66 INITIALIZATION
  // ------------------------------------------------------------

  Serial.println(
    "Initializing SEN66..."
  );


  uint16_t error = 0;

  char errorMessage[256];


  // Stop any previous measurement.
  error =
    sen66.stopMeasurement();


  if (error != 0) {

    errorToString(
      error,
      errorMessage,
      sizeof(errorMessage)
    );


    Serial.print(
      "SEN66 stopMeasurement error: "
    );


    Serial.println(
      errorMessage
    );
  }


  delay(100);


  // Start continuous measurement.
  error =
    sen66.startContinuousMeasurement();


  if (error != 0) {

    errorToString(
      error,
      errorMessage,
      sizeof(errorMessage)
    );


    Serial.print(
      "SEN66 startMeasurement error: "
    );


    Serial.println(
      errorMessage
    );

  }
  else {

    Serial.println(
      "SEN66 measurement started."
    );
  }


  // ------------------------------------------------------------
  // OLED
  // ------------------------------------------------------------

  Serial.println(
    "Starting OLED..."
  );


  oled.setI2CAddress(
    OLED_ADDRESS << 1
  );


  oled.begin();


  oled.clearBuffer();


  oled.setFont(
    u8g2_font_6x10_tf
  );


  oled.drawStr(
    20,
    28,
    "SEN66"
  );


  oled.drawStr(
    20,
    43,
    "Starting..."
  );


  oled.sendBuffer();


  // ------------------------------------------------------------
  // WIFI
  // ------------------------------------------------------------

  Serial.println(
    "Connecting to WiFi..."
  );


  WiFi.mode(
    WIFI_STA
  );


  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  while (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    delay(500);

    Serial.print(".");
  }


  Serial.println();

  Serial.println(
    "WiFi Connected!"
  );


  Serial.print(
    "IP address: "
  );


  Serial.println(
    WiFi.localIP()
  );


  Serial.print(
    "RSSI: "
  );


  Serial.print(
    WiFi.RSSI()
  );


  Serial.println(
    " dBm"
  );


  // ------------------------------------------------------------
  // TELNET
  // ------------------------------------------------------------

  telnetServer.begin();

  telnetServer.setNoDelay(true);


  Serial.println(
    "Telnet server started on port 23."
  );


  // ------------------------------------------------------------
  // HOMESPAN
  // ------------------------------------------------------------

  Serial.println(
    "Starting HomeSpan..."
  );


  homeSpan.begin(
    Category::Sensors,
    "SEN66 Air Quality"
  );


  // ------------------------------------------------------------
  // HOMESPAN ACCESSORY
  // ------------------------------------------------------------

  new SpanAccessory();


  new Service::AccessoryInformation();


  new Characteristic::Name(
    "SEN66 Air Quality"
  );


  new Characteristic::Manufacturer(
    "Sensirion"
  );


  new Characteristic::Model(
    "SEN66 + ESP32-S3"
  );


  new Characteristic::SerialNumber(
    "SEN66-ESP32"
  );


  new Characteristic::FirmwareRevision(
    "1.0"
  );


  // ------------------------------------------------------------
  // HOMEKIT AIR QUALITY
  // ------------------------------------------------------------

  new AirQualityService();


  // ------------------------------------------------------------
  // HOMEKIT CO2
  // ------------------------------------------------------------

  new CO2Service();


  // ------------------------------------------------------------
  // HOMEKIT TEMPERATURE
  // ------------------------------------------------------------

  new TemperatureService();


  // ------------------------------------------------------------
  // HOMEKIT HUMIDITY
  // ------------------------------------------------------------

  new HumidityService();


  Serial.println();

  Serial.println(
    "HomeSpan started."
  );


  Serial.println(
    "Pair the accessory with Apple Home."
  );


  // ------------------------------------------------------------
  // INITIAL TIMERS
  // ------------------------------------------------------------

  // Force an immediate first sensor read.
  lastSensorRead =
    millis() -
    SENSOR_INTERVAL_MS;


  lastOLEDUpdate =
    millis();


  lastPageChange =
    millis();
}


// ================================================================
// MAIN LOOP
// ================================================================

void loop() {

  // ------------------------------------------------------------
  // HOMEKIT
  // ------------------------------------------------------------

  homeSpan.poll();


  // ------------------------------------------------------------
  // TELNET
  // ------------------------------------------------------------

  handleTelnet();


  // ------------------------------------------------------------
  // SENSOR READING
  // ------------------------------------------------------------

  unsigned long now =
    millis();


  if (
    now -
    lastSensorRead >=
    SENSOR_INTERVAL_MS
  ) {

    lastSensorRead =
      now;


    readSEN66();
  }


  // ------------------------------------------------------------
  // OLED PAGE SWITCHING
  // ------------------------------------------------------------

  if (
    now -
    lastPageChange >=
    PAGE_INTERVAL_MS
  ) {

    lastPageChange =
      now;


    currentPage++;


    if (
      currentPage >= 4
    ) {

      currentPage = 0;
    }
  }


  // ------------------------------------------------------------
  // OLED UPDATE
  // ------------------------------------------------------------

  if (
    now -
    lastOLEDUpdate >=
    OLED_UPDATE_INTERVAL_MS
  ) {

    lastOLEDUpdate =
      now;


    updateOLED();
  }
}