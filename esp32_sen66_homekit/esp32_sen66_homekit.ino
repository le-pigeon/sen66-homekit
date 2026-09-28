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
 *   Uses PM2.5 and CO2 conditions to indicate air quality.
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

// SEN66 uses the ESP32-S3's normal hardware I2C bus.
#define SEN66_SDA 1
#define SEN66_SCL 2

// OLED uses a separate software I2C bus.
// This keeps the OLED completely separate from the SEN66.
#define OLED_SDA 6
#define OLED_SCL 7

// SH1106 OLED I2C address.
#define OLED_ADDRESS 0x3C

// YD-ESP32-S3 onboard WS2812 RGB LED.
#define RGB_LED_PIN 48


// ================================================================
// TIMING
// ================================================================

// SEN66 is read once every 10 seconds.
//
// This gives us:
//   6 samples/minute
//   360 samples/hour
//   8640 samples/day
#define SENSOR_INTERVAL_MS 10000

// OLED display update interval.
#define OLED_UPDATE_INTERVAL_MS 250

// Automatically switch OLED page every 5 seconds.
#define PAGE_INTERVAL_MS 5000


// ================================================================
// HISTORY BUFFER SIZES
// ================================================================

// PM2.5 history for one hour.
//
// 360 samples × 10 seconds = 1 hour.
#define PM25_1H_SAMPLES 360

// PM2.5 history for 24 hours.
//
// 8640 samples × 10 seconds = 24 hours.
#define PM25_24H_SAMPLES 8640

// CO2 history for 5 minutes.
//
// 30 samples × 10 seconds = 5 minutes.
#define CO2_5M_SAMPLES 30


// ================================================================
// SENSOR OBJECT
// ================================================================

SensirionI2cSen66 sen66;


// ================================================================
// OLED
// ================================================================
//
// The OLED uses software I2C on GPIO6 and GPIO7.
//
// We intentionally do NOT use GPIO1/2 here because those pins
// are reserved for the SEN66 hardware I2C bus.
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
// These variables contain the most recently measured values.
//

float pm1 = 0.0;
float pm25 = 0.0;
float pm4 = 0.0;
float pm10 = 0.0;

float co2 = 0.0;

float temperature = 0.0;
float humidity = 0.0;

float vocIndex = 0.0;
float noxIndex = 0.0;


// Used to indicate whether the SEN66 currently has a valid reading.
bool sensorOK = false;


// ================================================================
// PM2.5 HISTORY
// ================================================================

// One-hour PM2.5 history.
//
// This is a circular/ring buffer. Once it is full,
// new readings overwrite the oldest readings.
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

// Stores the most recent 30 CO2 readings.
//
// Because we read the sensor every 10 seconds:
//
// 30 readings × 10 seconds = 300 seconds = 5 minutes
//
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

// Air quality service.
SpanCharacteristic *homeAirQuality;

// PM2.5.
SpanCharacteristic *homePM25;

// PM10.
SpanCharacteristic *homePM10;


// CO2 service.
SpanCharacteristic *homeCO2Detected;
SpanCharacteristic *homeCO2Level;


// Temperature.
SpanCharacteristic *homeTemperature;


// Humidity.
SpanCharacteristic *homeHumidity;


// ================================================================
// RGB LED
// ================================================================
//
// The YD-ESP32-S3 onboard LED is a WS2812 addressable RGB LED.
//
// Values are deliberately kept low because the LED is very bright.
//

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_LED_PIN, r, g, b);
}


// Turn the RGB LED off.
void rgbOff() {
  setRGB(0, 0, 0);
}


// ================================================================
// PM2.5 → HOMEKIT AIR QUALITY
// ================================================================
//
// HomeKit's AirQuality characteristic is a categorical value,
// rather than a PM2.5 concentration.
//
// We therefore convert the PM2.5 measurement into one of the
// HomeKit AirQuality categories.
//

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

  // Store the newest reading.
  pm25History1H[pm25History1HIndex] = value;

  // Move to the next position.
  pm25History1HIndex++;

  // Wrap around when we reach the end.
  if (pm25History1HIndex >= PM25_1H_SAMPLES) {
    pm25History1HIndex = 0;
  }

  // During startup, gradually increase the number of
  // valid readings until the buffer is full.
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
// This is a circular buffer.
//
// Example:
//
// Reading 1  → buffer[0]
// Reading 2  → buffer[1]
// ...
// Reading 30 → buffer[29]
// Reading 31 → buffer[0]  ← replaces oldest reading
//
// This means the buffer always contains the most recent
// 5 minutes of CO2 data once it is full.
//

void addCO2History(float value) {

  // Store the newest CO2 reading.
  co2History5M[co2History5MIndex] = value;

  // Move to the next position.
  co2History5MIndex++;

  // Wrap back to the beginning.
  if (co2History5MIndex >= CO2_5M_SAMPLES) {
    co2History5MIndex = 0;
  }

  // Increase the valid sample count during startup.
  //
  // This is important because the array starts at zero.
  // We do NOT want those initial zeroes included in
  // the average.
  if (co2History5MCount < CO2_5M_SAMPLES) {
    co2History5MCount++;
  }
}


// ================================================================
// CALCULATE CO2 5-MINUTE AVERAGE
// ================================================================
//
// During startup this function only averages the readings that
// actually exist.
//
// For example:
//
//   1 reading  → average of 1
//   6 readings → average of 6
//   15 readings → average of 15
//   30 readings → full 5-minute average
//
// Once 30 readings have been collected, the buffer always
// represents the latest 5 minutes.
//

float getCO2Average5M() {

  // No readings yet.
  if (co2History5MCount == 0) {
    return 0.0;
  }

  float sum = 0.0;

  // Only include valid readings.
  for (int i = 0; i < co2History5MCount; i++) {
    sum += co2History5M[i];
  }

  return sum / co2History5MCount;
}


// ================================================================
// UPDATE ALL HISTORY AND AVERAGES
// ================================================================

void storeHistorySample() {

  // Add the current PM2.5 measurement to both history buffers.
  addPM25History1H(pm25);
  addPM25History24H(pm25);

  // Add the current CO2 measurement to the 5-minute buffer.
  addCO2History(co2);

  // Recalculate the averages.
  //
  // This is simple and easy to understand.
  // The ESP32-S3 has enough processing power for this.
  pm25Average1H = getPM25Average1H();
  pm25Average24H = getPM25Average24H();
  co2Average5M = getCO2Average5M();
}


// ================================================================
// RGB STATUS
// ================================================================
//
// The RGB LED uses the 1-hour PM2.5 average rather than a single
// instantaneous reading. This prevents the LED from changing
// colour because of one short-lived particle spike.
//
// CO2 can also be used as a ventilation indicator.
//

void updateRGBStatus() {

  // If we don't have enough PM2.5 data yet, keep the LED dim.
  if (pm25History1HCount < 30) {

    // Very dim blue during startup.
    setRGB(0, 0, 2);

    return;
  }


  // Use the 1-hour PM2.5 average for the main colour.
  float value = pm25Average1H;


  // Excellent / very low PM2.5.
  if (value <= 12.0) {

    setRGB(0, 2, 0);

  }

  // Good.
  else if (value <= 35.4) {

    setRGB(2, 2, 0);

  }

  // Fair.
  else if (value <= 55.4) {

    setRGB(2, 1, 0);

  }

  // Inferior.
  else if (value <= 150.4) {

    setRGB(3, 0, 0);

  }

  // Poor.
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


  // Temporary variables used by the SEN66 library.
  float massConcentrationPm1p0;
  float massConcentrationPm2p5;
  float massConcentrationPm4p0;
  float massConcentrationPm10p0;

  float ambientHumidity;
  float ambientTemperature;

  float vocIndexValue;
  float noxIndexValue;

  float co2Value;


  // Read the latest measurement from the SEN66.
  error = sen66.readMeasuredValues(
    massConcentrationPm1p0,
    massConcentrationPm2p5,
    massConcentrationPm4p0,
    massConcentrationPm10p0,
    ambientHumidity,
    ambientTemperature,
    vocIndexValue,
    noxIndexValue,
    co2Value
  );


  // Check whether the SEN66 returned an error.
  if (error != 0) {

    sensorOK = false;

    errorToString(error, errorMessage, sizeof(errorMessage));

    Serial.print("SEN66 read error: ");
    Serial.println(errorMessage);

    return;
  }


  // Store the sensor values in our global variables.
  pm1 = massConcentrationPm1p0;
  pm25 = massConcentrationPm2p5;
  pm4 = massConcentrationPm4p0;
  pm10 = massConcentrationPm10p0;

  humidity = ambientHumidity;
  temperature = ambientTemperature;

  vocIndex = vocIndexValue;
  noxIndex = noxIndexValue;

  co2 = co2Value;


  // The sensor returned a valid reading.
  sensorOK = true;


  // Store the reading in our history buffers.
  storeHistorySample();


  // Update HomeKit with the newest instantaneous readings.
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


  // Update HomeKit's categorical air-quality value.
  updateHomeKitAirQuality(pm25);


  // HomeKit's CarbonDioxideDetected characteristic is
  // separate from the numerical CO2 level.
  //
  // "NORMAL" appears in Apple Home as "No".
  //
  // This does NOT mean the sensor failed to detect CO2.
  //
  // It means CO2 is below our abnormal threshold.
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


  // Update the RGB status LED.
  updateRGBStatus();


  // Print sensor values to Serial for debugging.
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
    Serial.println("warming up");
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
//
// Draws a simple line graph on the OLED.
//
// The graph receives an array of values and maps them onto the
// available OLED area.
//
// This is not intended to be a scientific plotting system.
// It is simply a compact way of seeing how the sensor has changed
// over time.
//

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

  // Draw the graph border.
  oled.drawFrame(x, y, width, height);


  // Need at least two points to draw a line.
  if (count < 2) {
    return;
  }


  // Determine how many actual samples we want to display.
  int samplesToDraw = min(count, maxSamples);

  if (samplesToDraw < 2) {
    return;
  }


  // Determine where the first sample should start.
  int startIndex = count - samplesToDraw;


  // Draw each line segment.
  for (int i = 1; i < samplesToDraw; i++) {

    float previousValue = values[startIndex + i - 1];
    float currentValue = values[startIndex + i];


    // Limit values to the graph's range.
    previousValue = constrain(
      previousValue,
      0.0,
      maxValue
    );

    currentValue = constrain(
      currentValue,
      0.0,
      maxValue
    );


    // Convert the sample number to an X coordinate.
    int x1 = x + ((i - 1) * (width - 2)) / (samplesToDraw - 1);
    int x2 = x + (i * (width - 2)) / (samplesToDraw - 1);


    // Convert the sensor value to a Y coordinate.
    int y1 = y + height - 2 -
             (int)((previousValue / maxValue) * (height - 3));

    int y2 = y + height - 2 -
             (int)((currentValue / maxValue) * (height - 3));


    // Draw the line between the two points.
    oled.drawLine(x1, y1, x2, y2);
  }
}


// ================================================================
// GET CHRONOLOGICAL PM2.5 HISTORY
// ================================================================
//
// The circular buffer does not necessarily start with the oldest
// sample at index 0.
//
// This helper returns a sample in chronological order so that
// the graph can draw:
//
// oldest ------------------------ newest
//
// rather than:
//
// random buffer order
//

float getPM25History1H(int chronologicalIndex) {

  if (pm25History1HCount == 0) {
    return 0.0;
  }


  // Find the oldest valid sample.
  int oldestIndex;

  if (pm25History1HCount < PM25_1H_SAMPLES) {

    oldestIndex = 0;

  }
  else {

    oldestIndex = pm25History1HIndex;
  }


  int actualIndex =
    (oldestIndex + chronologicalIndex) % PM25_1H_SAMPLES;


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


  // Draw the graph border.
  oled.drawFrame(
    graphX,
    graphY,
    graphWidth,
    graphHeight
  );


  // Draw a rough 35.4 ug/m3 reference line.
  //
  // This corresponds to the upper boundary of the "Good"
  // PM2.5 range used in this project.
  //
  // It is only a visual reference, not an official HomeKit line.
  float reference = 35.4;


  float graphMax = 100.0;

  if (pm25Average1H > 80.0) {
    graphMax = ceil(pm25Average1H / 20.0) * 20.0;
  }


  int referenceY =
    graphY + graphHeight - 2 -
    (int)((reference / graphMax) * (graphHeight - 3));


  if (
    referenceY > graphY &&
    referenceY < graphY + graphHeight
  ) {

    // Draw a dotted reference line.
    for (
      int x = graphX + 2;
      x < graphX + graphWidth - 2;
      x += 4
    ) {
      oled.drawPixel(x, referenceY);
    }
  }


  // Draw the actual PM2.5 history.
  //
  // We temporarily copy the chronological values into a small
  // display buffer.
  //
  // The display can only physically show 128 pixels across,
  // so there is no benefit in drawing all 360 points separately.
  float displayValues[128];

  int displayCount =
    min(pm25History1HCount, 128);


  for (int i = 0; i < displayCount; i++) {

    int sourceIndex =
      (i * pm25History1HCount) / displayCount;

    displayValues[i] =
      getPM25History1H(sourceIndex);
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
// CO2 GRAPH
// ================================================================
//
// The CO2 graph covers the same 5-minute period as the rolling
// average.
//
// Each point represents approximately one 10-second reading.
//

void drawCO2Graph() {

  const int graphX = 0;
  const int graphY = 30;

  const int graphWidth = 128;
  const int graphHeight = 34;


  // CO2 graph range.
  //
  // Start at 2000 ppm so normal indoor values occupy a useful
  // portion of the graph.
  float graphMax = 2000.0;


  // If CO2 exceeds the normal graph range, expand the graph.
  if (co2 > graphMax) {
    graphMax = ceil(co2 / 500.0) * 500.0;
  }


  if (co2Average5M > graphMax) {
    graphMax =
      ceil(co2Average5M / 500.0) * 500.0;
  }


  // Draw graph border.
  oled.drawFrame(
    graphX,
    graphY,
    graphWidth,
    graphHeight
  );


  // Draw an 800 ppm reference line.
  //
  // This is the ventilation reference used by this project.
  float reference = 800.0;


  int referenceY =
    graphY + graphHeight - 2 -
    (int)((reference / graphMax) * (graphHeight - 3));


  if (
    referenceY > graphY &&
    referenceY < graphY + graphHeight
  ) {

    // Dotted reference line.
    for (
      int x = graphX + 2;
      x < graphX + graphWidth - 2;
      x += 4
    ) {
      oled.drawPixel(x, referenceY);
    }
  }


  // Draw the CO2 history.
  float displayValues[128];

  int displayCount =
    min(co2History5MCount, 128);


  for (int i = 0; i < displayCount; i++) {

    int sourceIndex =
      (i * co2History5MCount) / displayCount;

    // The CO2 history buffer is circular, so convert
    // chronological position into actual array position.
    int oldestIndex;

    if (co2History5MCount < CO2_5M_SAMPLES) {

      oldestIndex = 0;

    }
    else {

      oldestIndex = co2History5MIndex;
    }


    int actualIndex =
      (oldestIndex + sourceIndex) % CO2_5M_SAMPLES;


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


  // Page title.
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "PM2.5");


  // Current PM2.5.
  oled.setFont(u8g2_font_7x13B_tf);

  char buffer[32];

  snprintf(
    buffer,
    sizeof(buffer),
    "%.1f ug/m3",
    pm25
  );

  oled.drawStr(55, 10, buffer);


  // 1-hour average.
  oled.setFont(u8g2_font_5x8_tf);

  snprintf(
    buffer,
    sizeof(buffer),
    "1H %.1f",
    pm25Average1H
  );

  oled.drawStr(0, 21, buffer);


  // 24-hour average.
  snprintf(
    buffer,
    sizeof(buffer),
    "24H %.1f",
    pm25Average24H
  );

  oled.drawStr(45, 21, buffer);


  // Number of readings collected.
  snprintf(
    buffer,
    sizeof(buffer),
    "%d",
    pm25History1HCount
  );

  oled.drawStr(105, 21, buffer);


  // Draw the graph.
  drawPM25Graph();


  oled.sendBuffer();
}


// ================================================================
// OLED PAGE 2 — CO2 GRAPH
// ================================================================

void drawPageCO2() {

  oled.clearBuffer();


  oled.setFont(u8g2_font_6x10_tf);

  oled.drawStr(0, 9, "CO2");


  // Current CO2.
  oled.setFont(u8g2_font_7x13B_tf);

  char buffer[32];

  snprintf(
    buffer,
    sizeof(buffer),
    "%.0f ppm",
    co2
  );

  oled.drawStr(55, 10, buffer);


  oled.setFont(u8g2_font_5x8_tf);


  // Only display the 5-minute average once we actually
  // have 5 minutes of data.
  if (co2History5MCount >= CO2_5M_SAMPLES) {

    snprintf(
      buffer,
      sizeof(buffer),
      "5M %.0f",
      co2Average5M
    );

    oled.drawStr(0, 21, buffer);


    // Ventilation indication.
    //
    // This is a project-specific indication based on the
    // 5-minute CO2 average.
    //
    // It should be interpreted as:
    //
    //   <800 ppm  → OK
    //   >=800 ppm → elevated
    //
    // It is NOT a formal ventilation certification.
    if (co2Average5M >= 800.0) {

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

    // We don't have a complete 5-minute window yet.
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


  // Draw CO2 graph.
  drawCO2Graph();


  oled.sendBuffer();
}


// ================================================================
// OLED PAGE 3 — PARTICLES
// ================================================================

void drawPageParticles() {

  oled.clearBuffer();

  oled.setFont(u8g2_font_6x10_tf);

  oled.drawStr(0, 9, "PARTICLES");


  char buffer[32];

  oled.setFont(u8g2_font_6x10_tf);


  // PM1.0.
  snprintf(
    buffer,
    sizeof(buffer),
    "PM1   %.1f",
    pm1
  );

  oled.drawStr(0, 22, buffer);


  // PM2.5.
  snprintf(
    buffer,
    sizeof(buffer),
    "PM2.5 %.1f",
    pm25
  );

  oled.drawStr(64, 22, buffer);


  // PM4.0.
  snprintf(
    buffer,
    sizeof(buffer),
    "PM4   %.1f",
    pm4
  );

  oled.drawStr(0, 35, buffer);


  // PM10.
  snprintf(
    buffer,
    sizeof(buffer),
    "PM10  %.1f",
    pm10
  );

  oled.drawStr(64, 35, buffer);


  // CO2.
  snprintf(
    buffer,
    sizeof(buffer),
    "CO2   %.0f",
    co2
  );

  oled.drawStr(0, 48, buffer);


  // Sensor status.
  if (sensorOK) {

    oled.drawStr(
      64,
      48,
      "SENSOR OK"
    );

  }
  else {

    oled.drawStr(
      64,
      48,
      "ERROR"
    );
  }


  oled.sendBuffer();
}


// ================================================================
// OLED PAGE 4 — ENVIRONMENT
// ================================================================

void drawPageEnvironment() {

  oled.clearBuffer();

  oled.setFont(u8g2_font_6x10_tf);

  oled.drawStr(
    0,
    9,
    "ENVIRONMENT"
  );


  char buffer[32];


  // Temperature.
  snprintf(
    buffer,
    sizeof(buffer),
    "TEMP %.1f C",
    temperature
  );

  oled.drawStr(0, 22, buffer);


  // Relative humidity.
  snprintf(
    buffer,
    sizeof(buffer),
    "RH   %.1f%%",
    humidity
  );

  oled.drawStr(64, 22, buffer);


  // VOC index.
  snprintf(
    buffer,
    sizeof(buffer),
    "VOC  %.0f",
    vocIndex
  );

  oled.drawStr(0, 38, buffer);


  // NOx index.
  snprintf(
    buffer,
    sizeof(buffer),
    "NOx  %.0f",
    noxIndex
  );

  oled.drawStr(64, 38, buffer);


  // Explain that VOC/NOx are indices.
  oled.setFont(u8g2_font_5x8_tf);

  oled.drawStr(
    0,
    53,
    "VOC/NOx = index"
  );


  oled.sendBuffer();
}


// ================================================================
// UPDATE OLED
// ================================================================

void updateOLED() {

  // If the sensor hasn't produced a valid reading yet,
  // show a simple startup message.
  if (!sensorOK) {

    oled.clearBuffer();

    oled.setFont(u8g2_font_6x10_tf);

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


  // Select the appropriate page.
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
  telnetClient.println("SEN66 Air Quality Monitor");
  telnetClient.println("--------------------------------");
  telnetClient.println("help     - Show this help");
  telnetClient.println("status   - Show system status");
  telnetClient.println("sensor   - Show sensor readings");
  telnetClient.println("wifi     - Show WiFi information");
  telnetClient.println("memory   - Show memory usage");
  telnetClient.println("uptime   - Show uptime");
  telnetClient.println("restart  - Restart ESP32");
  telnetClient.println("--------------------------------");
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


  if (co2History5MCount >= CO2_5M_SAMPLES) {

    telnetClient.print("CO2 5M AVG: ");
    telnetClient.print(co2Average5M);
    telnetClient.println(" ppm");

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
  telnetClient.println(WiFi.SSID());

  telnetClient.print("IP: ");
  telnetClient.println(WiFi.localIP());

  telnetClient.print("RSSI: ");
  telnetClient.print(WiFi.RSSI());
  telnetClient.println(" dBm");

  telnetClient.println();
}


// ================================================================
// TELNET MEMORY
// ================================================================

void telnetPrintMemory() {

  telnetClient.println();

  telnetClient.println("MEMORY");
  telnetClient.println("------");

  telnetClient.print("Free heap: ");
  telnetClient.print(ESP.getFreeHeap());
  telnetClient.println(" bytes");

  telnetClient.print("Free PSRAM: ");
  telnetClient.print(ESP.getFreePsram());
  telnetClient.println(" bytes");

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


  telnetClient.print("Uptime: ");

  telnetClient.print(hours);
  telnetClient.print("h ");

  telnetClient.print(minutes);
  telnetClient.print("m ");

  telnetClient.print(seconds);
  telnetClient.println("s");
}


// ================================================================
// PROCESS TELNET COMMAND
// ================================================================

void processTelnetCommand(String command) {

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

  else if (command == "restart") {

    telnetClient.println("Restarting...");

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

  // If we don't currently have a client, check whether someone
  // is trying to connect.
  if (!telnetClient || !telnetClient.connected()) {

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


  // Process incoming characters.
  while (telnetClient.available()) {

    char c = telnetClient.read();


    // Don't echo password characters.
    if (!telnetAuthenticated) {

      if (c == '\n' || c == '\r') {

        String password =
          telnetInput;

        telnetInput = "";


        if (password == TERMINAL_PASSWORD) {

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


          if (telnetLoginAttempts >= 3) {

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

        // Only accept printable characters.
        if (c >= 32 && c <= 126) {
          telnetInput += c;
        }
      }

      continue;
    }


    // Once authenticated, echo normal commands.
    if (c == '\n' || c == '\r') {

      processTelnetCommand(telnetInput);

      telnetInput = "";

    }
    else {

      if (c >= 32 && c <= 126) {

        telnetInput += c;

        telnetClient.print(c);
      }
    }
  }
}


// ================================================================
// HOMESPAN AIR QUALITY SERVICE
// ================================================================

struct AirQualityService : Service::AirQualitySensor {

  AirQualityService() {

    // HomeKit's categorical air quality value.
    homeAirQuality =
      new Characteristic::AirQuality();

    // PM2.5 density.
    homePM25 =
      new Characteristic::PM2_5Density();

    // PM10 density.
    homePM10 =
      new Characteristic::PM10Density();
  }


  boolean update() override {

    // Sensor values are updated from readSEN66().
    // Returning true tells HomeSpan the service is valid.
    return true;
  }
};


// ================================================================
// HOMESPAN CO2 SERVICE
// ================================================================

struct CO2Service : Service::CarbonDioxideSensor {

  CO2Service() {

    // Numerical CO2 concentration.
    homeCO2Level =
      new Characteristic::CarbonDioxideLevel();

    // Whether CO2 is considered abnormal.
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

struct TemperatureService : Service::TemperatureSensor {

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

struct HumidityService : Service::HumiditySensor {

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
  Serial.println("================================");
  Serial.println(" SEN66 Air Quality Monitor");
  Serial.println("================================");


  // ------------------------------------------------------------
  // RGB LED
  // ------------------------------------------------------------

  // Start with the LED off.
  rgbOff();


  // ------------------------------------------------------------
  // SEN66 I2C
  // ------------------------------------------------------------

  Serial.println("Starting SEN66 I2C...");

  // Hardware I2C for SEN66.
  Wire.begin(
    SEN66_SDA,
    SEN66_SCL,
    100000
  );


  // Give the SEN66 library the I2C bus.
  sen66.begin(Wire, 0x6B);


  // ------------------------------------------------------------
  // SEN66 INITIALIZATION
  // ------------------------------------------------------------

  Serial.println("Initializing SEN66...");

  uint16_t error = 0;

  char errorMessage[256];


  // Stop any previous measurement.
  error = sen66.stopMeasurement();

  if (error != 0) {

    errorToString(
      error,
      errorMessage,
      sizeof(errorMessage)
    );

    Serial.print(
      "SEN66 stopMeasurement error: "
    );

    Serial.println(errorMessage);
  }


  delay(100);


  // Start continuous measurement.
  error = sen66.startContinuousMeasurement();

  if (error != 0) {

    errorToString(
      error,
      errorMessage,
      sizeof(errorMessage)
    );

    Serial.print(
      "SEN66 startMeasurement error: "
    );

    Serial.println(errorMessage);

  }
  else {

    Serial.println(
      "SEN66 measurement started."
    );
  }


  // ------------------------------------------------------------
  // OLED
  // ------------------------------------------------------------

  Serial.println("Starting OLED...");


  // Set the OLED I2C address.
  //
  // U8g2 expects the 8-bit address, hence << 1.
  oled.setI2CAddress(
    OLED_ADDRESS << 1
  );


  oled.begin();


  // Clear the display.
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

  Serial.println("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");
  }


  Serial.println();
  Serial.println("WiFi Connected!");

  Serial.print("IP address: ");
  Serial.println(
    WiFi.localIP()
  );

  Serial.print("RSSI: ");
  Serial.print(
    WiFi.RSSI()
  );

  Serial.println(" dBm");


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

  Serial.println("Starting HomeSpan...");


  // Start HomeSpan.
  //
  // This creates the Apple HomeKit accessory.
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

  lastSensorRead =
    millis() - SENSOR_INTERVAL_MS;

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

  // HomeSpan needs to run continuously so it can communicate
  // with Apple HomeKit.
  homeSpan.poll();


  // ------------------------------------------------------------
  // TELNET
  // ------------------------------------------------------------

  handleTelnet();


  // ------------------------------------------------------------
  // SENSOR READING
  // ------------------------------------------------------------

  unsigned long now = millis();


  if (
    now - lastSensorRead >=
    SENSOR_INTERVAL_MS
  ) {

    lastSensorRead = now;

    readSEN66();
  }


  // ------------------------------------------------------------
  // OLED PAGE SWITCHING
  // ------------------------------------------------------------

  if (
    now - lastPageChange >=
    PAGE_INTERVAL_MS
  ) {

    lastPageChange = now;

    currentPage++;

    if (currentPage >= 4) {
      currentPage = 0;
    }
  }


  // ------------------------------------------------------------
  // OLED UPDATE
  // ------------------------------------------------------------

  if (
    now - lastOLEDUpdate >=
    OLED_UPDATE_INTERVAL_MS
  ) {

    lastOLEDUpdate = now;

    updateOLED();
  }
}