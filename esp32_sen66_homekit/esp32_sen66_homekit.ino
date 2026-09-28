#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>

#include <HomeSpan.h>
#include <SensirionI2cSen66.h>
#include <U8g2lib.h>
#include <wifi_secrets.h>


// ============================================================
// PIN CONFIGURATION
// ============================================================

// SEN66
#define SEN66_SDA 1
#define SEN66_SCL 2

// SH1106 OLED
#define OLED_SDA 6
#define OLED_SCL 7

// Onboard WS2812 RGB LED
#define RGB_LED_PIN 48

// SEN66 default I2C address
#define SEN66_ADDRESS 0x6B

// OLED I2C address
#define OLED_ADDRESS 0x3C


// ============================================================
// TELNET
// ============================================================

WiFiServer terminalServer(23);
WiFiClient terminalClient;

String terminalCommand = "";

bool terminalAuthenticated = false;
uint8_t terminalLoginAttempts = 0;

const uint8_t MAX_TERMINAL_LOGIN_ATTEMPTS = 3;


// ============================================================
// SEN66
// ============================================================

SensirionI2cSen66 sen66;


// ============================================================
// OLED
// ============================================================
//
// Software I2C
//
// SCL = GPIO 7
// SDA = GPIO 6
//

U8G2_SH1106_128X64_NONAME_F_SW_I2C oled(
  U8G2_R0,
  OLED_SCL,
  OLED_SDA,
  U8X8_PIN_NONE
);


// ============================================================
// SENSOR DATA
// ============================================================

float pm1 = 0.0;
float pm25 = 0.0;
float pm4 = 0.0;
float pm10 = 0.0;

float temperature = 0.0;
float humidity = 0.0;

float vocIndex = 0.0;
float noxIndex = 0.0;

uint16_t co2 = 0;

bool sensorOK = false;


// ============================================================
// HOMESPAN CHARACTERISTICS
// ============================================================

Characteristic::AirQuality *homeAirQuality;

Characteristic::PM25Density *homePM25;
Characteristic::PM10Density *homePM10;

Characteristic::CarbonDioxideLevel *homeCO2;
Characteristic::CarbonDioxideDetected *homeCO2Detected;

Characteristic::CurrentTemperature *homeTemperature;
Characteristic::CurrentRelativeHumidity *homeHumidity;


// ============================================================
// TIMING
// ============================================================

unsigned long lastSensorRead = 0;
unsigned long lastOLEDUpdate = 0;
unsigned long lastPageChange = 0;
unsigned long lastHistorySample = 0;

const unsigned long SENSOR_INTERVAL = 1000;
const unsigned long OLED_INTERVAL = 250;
const unsigned long PAGE_INTERVAL = 5000;

// Store history every 10 seconds
const unsigned long HISTORY_INTERVAL = 10000;


// ============================================================
// OLED PAGES
// ============================================================

uint8_t oledPage = 0;

const uint8_t OLED_PAGE_COUNT = 4;


// ============================================================
// HISTORY BUFFERS
// ============================================================
//
// PM2.5:
// 360 samples x 10 seconds = 1 hour
// 8640 samples x 10 seconds = 24 hours
//
// CO2:
// 30 samples x 10 seconds = 5 minutes
//
// These are intentionally kept as float arrays.
// The memory requirement is still small for ESP32-S3.
//

#define PM25_HISTORY_1H 360
#define PM25_HISTORY_24H 8640
#define CO2_HISTORY_5M 30


float pm25History1H[PM25_HISTORY_1H];
float pm25History24H[PM25_HISTORY_24H];

uint16_t co2History5M[CO2_HISTORY_5M];

uint16_t pm25History1HIndex = 0;
uint16_t pm25History24HIndex = 0;
uint8_t co2HistoryIndex = 0;

uint16_t pm25History1HCount = 0;
uint16_t pm25History24HCount = 0;
uint8_t co2HistoryCount = 0;


// ============================================================
// ROLLING AVERAGES
// ============================================================

float pm25Average1H = 0.0;
float pm25Average24H = 0.0;

float co2Average5M = 0.0;


// ============================================================
// RGB LED
// ============================================================
//
// YD-ESP32-S3 onboard WS2812 RGB LED
// GPIO48
//
// Brightness deliberately kept low for indoor use.
//

void setRGB(uint8_t r, uint8_t g, uint8_t b) {

  neopixelWrite(
    RGB_LED_PIN,
    r,
    g,
    b
  );
}


// ============================================================
// CALCULATE PM2.5 1-HOUR AVERAGE
// ============================================================

void calculatePM25Average1H() {

  if (pm25History1HCount == 0) {

    pm25Average1H = 0.0;

    return;
  }


  double sum = 0.0;


  for (
    uint16_t i = 0;
    i < pm25History1HCount;
    i++
  ) {

    sum += pm25History1H[i];
  }


  pm25Average1H =
    sum / pm25History1HCount;
}


// ============================================================
// CALCULATE PM2.5 24-HOUR AVERAGE
// ============================================================

void calculatePM25Average24H() {

  if (pm25History24HCount == 0) {

    pm25Average24H = 0.0;

    return;
  }


  double sum = 0.0;


  for (
    uint16_t i = 0;
    i < pm25History24HCount;
    i++
  ) {

    sum += pm25History24H[i];
  }


  pm25Average24H =
    sum / pm25History24HCount;
}


// ============================================================
// CALCULATE CO2 5-MINUTE AVERAGE
// ============================================================

void calculateCO2Average5M() {

  if (co2HistoryCount == 0) {

    co2Average5M = 0.0;

    return;
  }


  uint32_t sum = 0;


  for (
    uint8_t i = 0;
    i < co2HistoryCount;
    i++
  ) {

    sum += co2History5M[i];
  }


  co2Average5M =
    (float)sum / co2HistoryCount;
}


// ============================================================
// STORE HISTORY SAMPLE
// ============================================================

void storeHistorySample() {

  if (!sensorOK) {
    return;
  }


  // ==========================================================
  // PM2.5 - 1 HOUR
  // ==========================================================

  pm25History1H[
    pm25History1HIndex
  ] = pm25;


  pm25History1HIndex++;

  if (
    pm25History1HIndex >=
    PM25_HISTORY_1H
  ) {

    pm25History1HIndex = 0;
  }


  if (
    pm25History1HCount <
    PM25_HISTORY_1H
  ) {

    pm25History1HCount++;
  }


  // ==========================================================
  // PM2.5 - 24 HOURS
  // ==========================================================

  pm25History24H[
    pm25History24HIndex
  ] = pm25;


  pm25History24HIndex++;

  if (
    pm25History24HIndex >=
    PM25_HISTORY_24H
  ) {

    pm25History24HIndex = 0;
  }


  if (
    pm25History24HCount <
    PM25_HISTORY_24H
  ) {

    pm25History24HCount++;
  }


  // ==========================================================
  // CO2 - 5 MINUTES
  // ==========================================================

  co2History5M[
    co2HistoryIndex
  ] = co2;


  co2HistoryIndex++;

  if (
    co2HistoryIndex >=
    CO2_HISTORY_5M
  ) {

    co2HistoryIndex = 0;
  }


  if (
    co2HistoryCount <
    CO2_HISTORY_5M
  ) {

    co2HistoryCount++;
  }


  // ==========================================================
  // UPDATE AVERAGES
  // ==========================================================

  calculatePM25Average1H();
  calculatePM25Average24H();
  calculateCO2Average5M();
}


// ============================================================
// GET 1-HOUR HISTORY VALUE IN CHRONOLOGICAL ORDER
// ============================================================
//
// Position 0 = oldest
// Position count-1 = newest
//

float getPM25History1H(uint16_t position) {

  if (
    position >= pm25History1HCount
  ) {

    return 0.0;
  }


  uint16_t oldestIndex;


  if (
    pm25History1HCount <
    PM25_HISTORY_1H
  ) {

    oldestIndex = 0;

  }
  else {

    oldestIndex =
      pm25History1HIndex;
  }


  uint16_t index =
    oldestIndex + position;


  if (
    index >=
    PM25_HISTORY_1H
  ) {

    index -= PM25_HISTORY_1H;
  }


  return pm25History1H[index];
}


// ============================================================
// UPDATE RGB LED
// ============================================================
//
// Uses the 1-hour PM2.5 average.
//
// CO2 is treated as a ventilation modifier rather than being
// added directly to the PM2.5 number.
//
// Logic:
//
// PM2.5 <= 12       -> green
// PM2.5 <= 35.4     -> yellow-green
// PM2.5 <= 55.4     -> yellow
// PM2.5 <= 125.4    -> orange
// PM2.5 > 125.4     -> red
//
// If PM2.5 is elevated AND CO2 is high, the warning can become
// stronger.
//
// If PM2.5 is low but CO2 is high, show amber to indicate
// ventilation rather than particulate pollution.
//

void updateRGBLED() {

  if (!sensorOK) {

    // Dim blue = sensor warming up / unavailable
    setRGB(0, 0, 2);

    return;
  }


  // During initial history collection, use current PM2.5
  // rather than an incomplete average.

  float pmForLED;


  if (
    pm25History1HCount >= 30
  ) {

    // At least 5 minutes of history
    pmForLED = pm25Average1H;

  }
  else {

    pmForLED = pm25;
  }


  bool highCO2 =
    co2Average5M >= 800.0;


  // ==========================================================
  // LOW PM2.5
  // ==========================================================

  if (
    pmForLED <= 12.0
  ) {

    if (highCO2) {

      // Airborne particles are low, but ventilation is poorer
      setRGB(2, 1, 0);

    }
    else {

      setRGB(0, 2, 0);
    }

    return;
  }


  // ==========================================================
  // GOOD / MODERATE
  // ==========================================================

  if (
    pmForLED <= 35.4
  ) {

    if (highCO2) {

      setRGB(2, 2, 0);

    }
    else {

      setRGB(1, 2, 0);
    }

    return;
  }


  // ==========================================================
  // FAIR
  // ==========================================================

  if (
    pmForLED <= 55.4
  ) {

    setRGB(2, 1, 0);

    return;
  }


  // ==========================================================
  // ELEVATED PM2.5
  // ==========================================================

  if (
    pmForLED <= 125.4
  ) {

    if (highCO2) {

      // PM2.5 elevated + poor ventilation
      setRGB(3, 0, 0);

    }
    else {

      setRGB(2, 0, 0);
    }

    return;
  }


  // ==========================================================
  // VERY HIGH PM2.5
  // ==========================================================

  setRGB(4, 0, 0);
}


// ============================================================
// SEN66 ERROR HANDLING
// ============================================================

void printSEN66Error(int16_t error) {

  char errorMessage[64];

  errorToString(
    error,
    errorMessage,
    sizeof(errorMessage)
  );

  Serial.print("SEN66 error: ");
  Serial.println(errorMessage);
}


// ============================================================
// UPDATE HOMESPAN AIR QUALITY STATUS
// ============================================================

void updateAirQuality() {

  if (!homeAirQuality || !sensorOK) {
    return;
  }


  if (pm25 <= 12.0) {

    homeAirQuality->setVal(
      Characteristic::AirQuality::EXCELLENT
    );

  }
  else if (pm25 <= 35.4) {

    homeAirQuality->setVal(
      Characteristic::AirQuality::GOOD
    );

  }
  else if (pm25 <= 55.4) {

    homeAirQuality->setVal(
      Characteristic::AirQuality::FAIR
    );

  }
  else if (pm25 <= 150.4) {

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


// ============================================================
// READ SEN66
// ============================================================

void readSEN66() {

  float newPM1 = 0.0;
  float newPM25 = 0.0;
  float newPM4 = 0.0;
  float newPM10 = 0.0;

  float newHumidity = 0.0;
  float newTemperature = 0.0;

  float newVOC = 0.0;
  float newNOx = 0.0;

  uint16_t newCO2 = 0;


  int16_t error = sen66.readMeasuredValues(
    newPM1,
    newPM25,
    newPM4,
    newPM10,
    newHumidity,
    newTemperature,
    newVOC,
    newNOx,
    newCO2
  );


  if (error != 0) {

    sensorOK = false;

    printSEN66Error(error);

    updateRGBLED();

    return;
  }


  pm1 = newPM1;
  pm25 = newPM25;
  pm4 = newPM4;
  pm10 = newPM10;

  humidity = newHumidity;
  temperature = newTemperature;

  vocIndex = newVOC;
  noxIndex = newNOx;

  co2 = newCO2;

  sensorOK = true;


  // ==========================================================
  // HISTORY
  // ==========================================================

  unsigned long now = millis();


  if (
    now - lastHistorySample >=
    HISTORY_INTERVAL
  ) {

    lastHistorySample = now;

    storeHistorySample();
  }


  // ==========================================================
  // HOMESPAN
  // ==========================================================

  if (homePM25) {
    homePM25->setVal(pm25);
  }

  if (homePM10) {
    homePM10->setVal(pm10);
  }

  if (homeCO2) {
    homeCO2->setVal((float)co2);
  }

  if (homeTemperature) {
    homeTemperature->setVal(temperature);
  }

  if (homeHumidity) {
    homeHumidity->setVal(humidity);
  }


  if (homeCO2Detected) {

    if (co2 >= 2000) {

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


  updateAirQuality();

  updateRGBLED();


  // ==========================================================
  // SERIAL OUTPUT
  // ==========================================================

  Serial.printf(
    "PM1 %.1f | PM2.5 %.1f | PM4 %.1f | PM10 %.1f | "
    "T %.1f C | RH %.1f %% | VOC %.0f | NOx %.0f | CO2 %u | "
    "PM25 1H %.1f | PM25 24H %.1f | CO2 5M %.0f\n",

    pm1,
    pm25,
    pm4,
    pm10,
    temperature,
    humidity,
    vocIndex,
    noxIndex,
    co2,
    pm25Average1H,
    pm25Average24H,
    co2Average5M
  );
}


// ============================================================
// DRAW GRAPH
// ============================================================
//
// Draws the last hour of PM2.5 history.
//
// Graph area:
// X = 2..127
// Y = 25..61
//
// Vertical scale is automatically determined from the data,
// with a minimum range of 60 ug/m3 so normal indoor readings
// remain visually useful.
//

void drawPM25Graph() {

  const int graphLeft = 2;
  const int graphRight = 127;
  const int graphTop = 27;
  const int graphBottom = 61;

  oled.drawFrame(
    graphLeft,
    graphTop,
    graphRight - graphLeft + 1,
    graphBottom - graphTop + 1
  );


  if (
    pm25History1HCount < 2
  ) {

    oled.setFont(
      u8g2_font_5x8_tf
    );

    oled.drawStr(
      30,
      48,
      "COLLECTING..."
    );

    return;
  }


  // ==========================================================
  // Find maximum value
  // ==========================================================

  float maxValue = 60.0;


  for (
    uint16_t i = 0;
    i < pm25History1HCount;
    i++
  ) {

    float value =
      getPM25History1H(i);


    if (
      value > maxValue
    ) {

      maxValue = value;
    }
  }


  // Round scale upwards
  if (maxValue <= 60.0) {
    maxValue = 60.0;
  }
  else if (maxValue <= 100.0) {
    maxValue = 100.0;
  }
  else if (maxValue <= 150.0) {
    maxValue = 150.0;
  }
  else if (maxValue <= 250.0) {
    maxValue = 250.0;
  }
  else {
    maxValue =
      ceil(maxValue / 50.0) * 50.0;
  }


  // ==========================================================
  // Draw reference line at 35.4 ug/m3
  // ==========================================================

  if (
    maxValue > 35.4
  ) {

    int referenceY =
      graphBottom -
      (int)(
        (35.4 / maxValue) *
        (graphBottom - graphTop - 1)
      );


    // Dotted reference line
    for (
      int x = graphLeft + 2;
      x < graphRight;
      x += 4
    ) {

      oled.drawPixel(
        x,
        referenceY
      );
    }
  }


  // ==========================================================
  // Draw graph
  // ==========================================================

  uint16_t count =
    pm25History1HCount;


  for (
    uint16_t i = 1;
    i < count;
    i++
  ) {

    float value1 =
      getPM25History1H(i - 1);

    float value2 =
      getPM25History1H(i);


    int x1 =
      graphLeft + 1 +
      (
        (long)(i - 1) *
        (graphRight - graphLeft - 2)
        /
        (count - 1)
      );


    int x2 =
      graphLeft + 1 +
      (
        (long)i *
        (graphRight - graphLeft - 2)
        /
        (count - 1)
      );


    int y1 =
      graphBottom - 1 -
      (int)(
        (value1 / maxValue) *
        (graphBottom - graphTop - 2)
      );


    int y2 =
      graphBottom - 1 -
      (int)(
        (value2 / maxValue) *
        (graphBottom - graphTop - 2)
      );


    if (y1 < graphTop + 1) {
      y1 = graphTop + 1;
    }

    if (y2 < graphTop + 1) {
      y2 = graphTop + 1;
    }

    if (y1 > graphBottom - 1) {
      y1 = graphBottom - 1;
    }

    if (y2 > graphBottom - 1) {
      y2 = graphBottom - 1;
    }


    oled.drawLine(
      x1,
      y1,
      x2,
      y2
    );
  }
}


// ============================================================
// DRAW CO2 GRAPH
// ============================================================

void drawCO2Graph() {

  const int graphLeft = 2;
  const int graphRight = 127;
  const int graphTop = 27;
  const int graphBottom = 61;

  oled.drawFrame(
    graphLeft,
    graphTop,
    graphRight - graphLeft + 1,
    graphBottom - graphTop + 1
  );


  if (
    co2HistoryCount < 2
  ) {

    oled.setFont(
      u8g2_font_5x8_tf
    );

    oled.drawStr(
      30,
      48,
      "COLLECTING..."
    );

    return;
  }


  // ==========================================================
  // Determine graph scale
  // ==========================================================

  float maxValue = 1200.0;


  for (
    uint8_t i = 0;
    i < co2HistoryCount;
    i++
  ) {

    if (
      co2History5M[i] >
      maxValue
    ) {

      maxValue =
        co2History5M[i];
    }
  }


  if (
    maxValue <= 1200.0
  ) {

    maxValue = 1200.0;

  }
  else {

    maxValue =
      ceil(maxValue / 200.0) * 200.0;
  }


  // ==========================================================
  // Draw 800 ppm reference line
  // ==========================================================

  if (
    maxValue > 800.0
  ) {

    int referenceY =
      graphBottom -
      (int)(
        (800.0 / maxValue) *
        (graphBottom - graphTop - 1)
      );


    for (
      int x = graphLeft + 2;
      x < graphRight;
      x += 4
    ) {

      oled.drawPixel(
        x,
        referenceY
      );
    }
  }


  // ==========================================================
  // Draw graph
  // ==========================================================

  for (
    uint8_t i = 1;
    i < co2HistoryCount;
    i++
  ) {

    float value1 =
      co2History5M[i - 1];

    float value2 =
      co2History5M[i];


    int x1 =
      graphLeft + 1 +
      (
        (long)(i - 1) *
        (graphRight - graphLeft - 2)
        /
        (co2HistoryCount - 1)
      );


    int x2 =
      graphLeft + 1 +
      (
        (long)i *
        (graphRight - graphLeft - 2)
        /
        (co2HistoryCount - 1)
      );


    int y1 =
      graphBottom - 1 -
      (int)(
        (value1 / maxValue) *
        (graphBottom - graphTop - 2)
      );


    int y2 =
      graphBottom - 1 -
      (int)(
        (value2 / maxValue) *
        (graphBottom - graphTop - 2)
      );


    if (y1 < graphTop + 1) {
      y1 = graphTop + 1;
    }

    if (y2 < graphTop + 1) {
      y2 = graphTop + 1;
    }

    if (y1 > graphBottom - 1) {
      y1 = graphBottom - 1;
    }

    if (y2 > graphBottom - 1) {
      y2 = graphBottom - 1;
    }


    oled.drawLine(
      x1,
      y1,
      x2,
      y2
    );
  }
}


// ============================================================
// OLED PAGE 1
// ============================================================
//
// PM2.5 graph
// Current PM2.5
// 1-hour average
// 24-hour average
//

void drawOLEDPage1() {

  oled.setFont(
    u8g2_font_6x10_tf
  );


  oled.setCursor(
    0,
    9
  );

  oled.printf(
    "PM2.5 %5.1f",
    pm25
  );


  oled.setCursor(
    73,
    9
  );

  oled.printf(
    "1H %5.1f",
    pm25Average1H
  );


  oled.setCursor(
    0,
    20
  );

  oled.printf(
    "24H AVG %5.1f ug/m3",
    pm25Average24H
  );


  drawPM25Graph();
}


// ============================================================
// OLED PAGE 2
// ============================================================
//
// CO2 graph
// Current CO2
// 5-minute average
//

void drawOLEDPage2() {

  oled.setFont(
    u8g2_font_6x10_tf
  );


  oled.setCursor(
    0,
    9
  );

  oled.printf(
    "CO2 %4u ppm",
    co2
  );


  oled.setCursor(
    72,
    9
  );

  oled.printf(
    "5M %4.0f",
    co2Average5M
  );


  oled.setCursor(
    0,
    20
  );

  if (
    co2Average5M >= 800.0
  ) {

    oled.drawStr(
      0,
      20,
      "VENTILATION HIGH"
    );

  }
  else {

    oled.drawStr(
      0,
      20,
      "VENTILATION OK"
    );
  }


  drawCO2Graph();
}


// ============================================================
// OLED PAGE 3
// ============================================================
//
// Particle measurements
//

void drawOLEDPage3() {

  oled.setFont(
    u8g2_font_6x10_tf
  );


  oled.drawStr(
    0,
    9,
    "PARTICLES"
  );


  oled.drawHLine(
    0,
    12,
    128
  );


  oled.setCursor(
    0,
    23
  );

  oled.printf(
    "PM1.0   %5.1f ug/m3",
    pm1
  );


  oled.setCursor(
    0,
    33
  );

  oled.printf(
    "PM2.5   %5.1f ug/m3",
    pm25
  );


  oled.setCursor(
    0,
    43
  );

  oled.printf(
    "PM4.0   %5.1f ug/m3",
    pm4
  );


  oled.setCursor(
    0,
    53
  );

  oled.printf(
    "PM10    %5.1f ug/m3",
    pm10
  );


  oled.setCursor(
    0,
    63
  );

  oled.printf(
    "CO2     %5u ppm",
    co2
  );
}


// ============================================================
// OLED PAGE 4
// ============================================================
//
// Environmental measurements
//

void drawOLEDPage4() {

  oled.setFont(
    u8g2_font_6x10_tf
  );


  oled.drawStr(
    0,
    9,
    "ENVIRONMENT"
  );


  oled.drawHLine(
    0,
    12,
    128
  );


  oled.setCursor(
    0,
    25
  );

  oled.printf(
    "TEMP    %5.1f C",
    temperature
  );


  oled.setCursor(
    0,
    37
  );

  oled.printf(
    "RH      %5.1f %%",
    humidity
  );


  oled.setCursor(
    0,
    49
  );

  oled.printf(
    "VOC IDX %5.0f",
    vocIndex
  );


  oled.setCursor(
    0,
    61
  );

  oled.printf(
    "NOx IDX %5.0f",
    noxIndex
  );
}


// ============================================================
// OLED UPDATE
// ============================================================

void updateOLED() {

  oled.clearBuffer();


  if (!sensorOK) {

    oled.setFont(
      u8g2_font_6x10_tf
    );

    oled.drawStr(
      0,
      20,
      "SEN66"
    );

    oled.drawStr(
      0,
      38,
      "WARMING UP..."
    );

    oled.drawStr(
      0,
      56,
      "Please wait"
    );

    oled.sendBuffer();

    return;
  }


  switch (oledPage) {

    case 0:
      drawOLEDPage1();
      break;

    case 1:
      drawOLEDPage2();
      break;

    case 2:
      drawOLEDPage3();
      break;

    case 3:
      drawOLEDPage4();
      break;
  }


  oled.sendBuffer();
}


// ============================================================
// TELNET HELP
// ============================================================

void terminalHelp() {

  terminalClient.println();
  terminalClient.println("SEN66 Air Quality Monitor");
  terminalClient.println("-------------------------");
  terminalClient.println("Available commands:");
  terminalClient.println();
  terminalClient.println("  help     - Show this help");
  terminalClient.println("  status   - Show sensor summary");
  terminalClient.println("  sensor   - Show all SEN66 readings");
  terminalClient.println("  wifi     - Show WiFi information");
  terminalClient.println("  memory   - Show ESP32 memory");
  terminalClient.println("  uptime   - Show device uptime");
  terminalClient.println("  restart  - Restart ESP32");
  terminalClient.println();
}


// ============================================================
// TELNET STATUS
// ============================================================

void terminalStatus() {

  terminalClient.println();

  terminalClient.println("SEN66 STATUS");
  terminalClient.println("------------");

  terminalClient.print("Sensor: ");

  if (sensorOK) {
    terminalClient.println("OK");
  }
  else {
    terminalClient.println("NOT READY");
  }

  terminalClient.print("PM2.5: ");
  terminalClient.print(pm25, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("PM2.5 1H AVG: ");
  terminalClient.print(pm25Average1H, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("PM2.5 24H AVG: ");
  terminalClient.print(pm25Average24H, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("CO2: ");
  terminalClient.print(co2);
  terminalClient.println(" ppm");

  terminalClient.print("CO2 5M AVG: ");
  terminalClient.print(co2Average5M, 0);
  terminalClient.println(" ppm");

  terminalClient.print("Temperature: ");
  terminalClient.print(temperature, 1);
  terminalClient.println(" C");

  terminalClient.print("Humidity: ");
  terminalClient.print(humidity, 1);
  terminalClient.println(" %");

  terminalClient.println();
}


// ============================================================
// TELNET SENSOR
// ============================================================

void terminalSensor() {

  terminalClient.println();

  terminalClient.println("SEN66 MEASUREMENTS");
  terminalClient.println("------------------");

  terminalClient.print("PM1.0:       ");
  terminalClient.print(pm1, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("PM2.5:       ");
  terminalClient.print(pm25, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("PM4.0:       ");
  terminalClient.print(pm4, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("PM10:        ");
  terminalClient.print(pm10, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("Temperature: ");
  terminalClient.print(temperature, 1);
  terminalClient.println(" C");

  terminalClient.print("Humidity:    ");
  terminalClient.print(humidity, 1);
  terminalClient.println(" %");

  terminalClient.print("VOC Index:   ");
  terminalClient.println(vocIndex, 0);

  terminalClient.print("NOx Index:   ");
  terminalClient.println(noxIndex, 0);

  terminalClient.print("CO2:         ");
  terminalClient.print(co2);
  terminalClient.println(" ppm");

  terminalClient.println();

  terminalClient.print("PM2.5 1H AVG:  ");
  terminalClient.print(pm25Average1H, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("PM2.5 24H AVG: ");
  terminalClient.print(pm25Average24H, 1);
  terminalClient.println(" ug/m3");

  terminalClient.print("CO2 5M AVG:    ");
  terminalClient.print(co2Average5M, 0);
  terminalClient.println(" ppm");

  terminalClient.println();
}


// ============================================================
// TELNET WIFI
// ============================================================

void terminalWifi() {

  terminalClient.println();

  terminalClient.println("WIFI");
  terminalClient.println("----");

  terminalClient.print("SSID: ");
  terminalClient.println(WiFi.SSID());

  terminalClient.print("IP: ");
  terminalClient.println(WiFi.localIP());

  terminalClient.print("RSSI: ");
  terminalClient.print(WiFi.RSSI());
  terminalClient.println(" dBm");

  terminalClient.print("MAC: ");
  terminalClient.println(WiFi.macAddress());

  terminalClient.println();
}


// ============================================================
// TELNET MEMORY
// ============================================================

void terminalMemory() {

  terminalClient.println();

  terminalClient.println("MEMORY");
  terminalClient.println("------");

  terminalClient.print("Free heap: ");
  terminalClient.print(ESP.getFreeHeap());
  terminalClient.println(" bytes");

  terminalClient.print("Minimum free heap: ");
  terminalClient.print(ESP.getMinFreeHeap());
  terminalClient.println(" bytes");

  terminalClient.print("Free PSRAM: ");
  terminalClient.print(ESP.getFreePsram());
  terminalClient.println(" bytes");

  terminalClient.print("Free internal heap: ");
  terminalClient.print(
    ESP.getFreeHeap()
  );
  terminalClient.println(" bytes");

  terminalClient.println();

  terminalClient.print(
    "PM2.5 24H history memory: "
  );

  terminalClient.print(
    sizeof(pm25History24H)
  );

  terminalClient.println(
    " bytes"
  );

  terminalClient.println();
}


// ============================================================
// TELNET UPTIME
// ============================================================

void terminalUptime() {

  unsigned long totalSeconds =
    millis() / 1000;

  unsigned long days =
    totalSeconds / 86400;

  totalSeconds %= 86400;

  unsigned long hours =
    totalSeconds / 3600;

  totalSeconds %= 3600;

  unsigned long minutes =
    totalSeconds / 60;

  unsigned long seconds =
    totalSeconds % 60;


  terminalClient.println();

  terminalClient.println("UPTIME");
  terminalClient.println("------");

  terminalClient.printf(
    "%lu days, %lu hours, %lu minutes, %lu seconds\n",
    days,
    hours,
    minutes,
    seconds
  );

  terminalClient.println();
}


// ============================================================
// TELNET COMMAND PROCESSING
// ============================================================

void processTerminalCommand(String command) {

  command.trim();
  command.toLowerCase();


  if (command.length() == 0) {
    return;
  }


  if (command == "help") {

    terminalHelp();

  }
  else if (command == "status") {

    terminalStatus();

  }
  else if (command == "sensor") {

    terminalSensor();

  }
  else if (command == "wifi") {

    terminalWifi();

  }
  else if (command == "memory") {

    terminalMemory();

  }
  else if (command == "uptime") {

    terminalUptime();

  }
  else if (command == "restart") {

    terminalClient.println();
    terminalClient.println("Restarting ESP32...");
    terminalClient.println();

    delay(500);

    ESP.restart();

  }
  else {

    terminalClient.println();
    terminalClient.print("Unknown command: ");
    terminalClient.println(command);
    terminalClient.println("Type 'help' for available commands.");
    terminalClient.println();
  }
}


// ============================================================
// TELNET SERVER
// ============================================================

void handleTerminal() {

  // ==========================================================
  // New connection
  // ==========================================================

  if (
    terminalServer.hasClient()
  ) {

    WiFiClient newClient =
      terminalServer.available();


    // Only allow one terminal connection
    if (
      terminalClient &&
      terminalClient.connected()
    ) {

      newClient.println(
        "Terminal already in use."
      );

      newClient.stop();

    }
    else {

      terminalClient =
        newClient;

      terminalAuthenticated = false;

      terminalLoginAttempts = 0;

      terminalCommand = "";


      terminalClient.println();
      terminalClient.println(
        "================================"
      );
      terminalClient.println(
        " SEN66 Air Quality Monitor"
      );
      terminalClient.println(
        " Wireless Terminal"
      );
      terminalClient.println(
        "================================"
      );

      terminalClient.println();
      terminalClient.println(
        "Password:"
      );
    }
  }


  // ==========================================================
  // Existing connection
  // ==========================================================

  if (
    !terminalClient ||
    !terminalClient.connected()
  ) {

    return;
  }


  while (
    terminalClient.available()
  ) {

    char c =
      terminalClient.read();


    // ========================================================
    // Authentication
    // ========================================================

    if (!terminalAuthenticated) {

      if (
        c == '\n' ||
        c == '\r'
      ) {

        if (
          terminalCommand.length() == 0
        ) {

          continue;
        }


        if (
          terminalCommand ==
          TERMINAL_PASSWORD
        ) {

          terminalAuthenticated = true;

          terminalCommand = "";

          terminalClient.println();
          terminalClient.println(
            "Authentication successful."
          );

          terminalClient.println(
            "Type 'help' for commands."
          );

          terminalClient.println();

          terminalClient.print(">");

        }
        else {

          terminalLoginAttempts++;

          terminalCommand = "";


          if (
            terminalLoginAttempts >=
            MAX_TERMINAL_LOGIN_ATTEMPTS
          ) {

            terminalClient.println();
            terminalClient.println(
              "Too many failed attempts."
            );

            terminalClient.println(
              "Connection closed."
            );

            delay(200);

            terminalClient.stop();

            return;
          }


          terminalClient.println();
          terminalClient.println(
            "Incorrect password."
          );

          terminalClient.println();

          terminalClient.print(
            "Password:"
          );
        }

      }
      else {

        // Do not echo password characters
        if (
          c != '\b' &&
          c != 127
        ) {

          terminalCommand += c;
        }
      }

      continue;
    }


    // ========================================================
    // Authenticated command input
    // ========================================================

    if (
      c == '\n' ||
      c == '\r'
    ) {

      if (
        terminalCommand.length() > 0
      ) {

        processTerminalCommand(
          terminalCommand
        );

        terminalCommand = "";
      }


      if (
        terminalClient &&
        terminalClient.connected()
      ) {

        terminalClient.print(">");
      }

    }
    else if (
      c == '\b' ||
      c == 127
    ) {

      if (
        terminalCommand.length() > 0
      ) {

        terminalCommand.remove(
          terminalCommand.length() - 1
        );
      }

    }
    else {

      // Prevent command string from growing indefinitely
      if (
        terminalCommand.length() < 100
      ) {

        terminalCommand += c;
      }
    }
  }
}


// ============================================================
// HOMESPAN SETUP
// ============================================================

void setupHomeSpan() {

  homeSpan.setWifiCredentials(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  homeSpan.begin(
    Category::Sensors,
    "SEN66 Air Quality"
  );


  new SpanAccessory();

    new Service::AccessoryInformation();

      new Characteristic::Identify();

      new Characteristic::Name(
        "SEN66 Air Quality"
      );

      new Characteristic::Manufacturer(
        "Sensirion"
      );

      new Characteristic::Model(
        "SEN66-ESP32S3"
      );

      new Characteristic::SerialNumber(
        "SEN66-001"
      );

      new Characteristic::FirmwareRevision(
        "1.0.0"
      );


  // ==========================================================
  // AIR QUALITY
  // ==========================================================

  new Service::AirQualitySensor();

    homeAirQuality =
      new Characteristic::AirQuality(
        Characteristic::AirQuality::EXCELLENT
      );

    homePM25 =
      new Characteristic::PM25Density();

    homePM10 =
      new Characteristic::PM10Density();


  // ==========================================================
  // CO2
  // ==========================================================

  new Service::CarbonDioxideSensor();

    homeCO2Detected =
      new Characteristic::CarbonDioxideDetected(
        Characteristic::CarbonDioxideDetected::NORMAL
      );

    homeCO2 =
      new Characteristic::CarbonDioxideLevel();


  // ==========================================================
  // TEMPERATURE
  // ==========================================================

  new Service::TemperatureSensor();

    homeTemperature =
      new Characteristic::CurrentTemperature();


  // ==========================================================
  // HUMIDITY
  // ==========================================================

  new Service::HumiditySensor();

    homeHumidity =
      new Characteristic::CurrentRelativeHumidity();
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(500);


  Serial.println();

  Serial.println(
    "=============================="
  );

  Serial.println(
    " SEN66 Air Quality Monitor"
  );

  Serial.println(
    " ESP32-S3 + HomeSpan + OLED"
  );

  Serial.println(
    " RGB Status LED"
  );

  Serial.println(
    " Wireless Terminal"
  );

  Serial.println(
    "=============================="
  );


  // ==========================================================
  // RGB LED
  // ==========================================================

  // Dim blue while starting
  setRGB(0, 0, 2);


  // ==========================================================
  // SEN66 I2C
  // ==========================================================

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
    SEN66_ADDRESS
  );


  int16_t error =
    sen66.deviceReset();


  if (error != 0) {

    Serial.println(
      "SEN66 reset failed."
    );

    printSEN66Error(error);

  }
  else {

    Serial.println(
      "SEN66 reset OK."
    );
  }


  delay(1200);


  error =
    sen66.startContinuousMeasurement();


  if (error != 0) {

    Serial.println(
      "Failed to start SEN66 measurement."
    );

    printSEN66Error(error);

  }
  else {

    Serial.println(
      "SEN66 measurement started."
    );
  }


  delay(1100);


  // ==========================================================
  // OLED
  // ==========================================================

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
    0,
    20,
    "SEN66 AIR QUALITY"
  );


  oled.drawStr(
    0,
    38,
    "Starting..."
  );


  oled.sendBuffer();


  // ==========================================================
  // HOMESPAN
  // ==========================================================

  setupHomeSpan();


  Serial.println(
    "HomeSpan started."
  );


  Serial.println(
    "Ready for Apple Home pairing."
  );


  // ==========================================================
  // TELNET
  // ==========================================================

  terminalServer.begin();

  terminalServer.setNoDelay(true);


  Serial.println(
    "Wireless terminal started."
  );

  Serial.println(
    "Telnet port: 23"
  );


  // ==========================================================
  // INITIAL TIMERS
  // ==========================================================

  lastSensorRead =
    millis() - SENSOR_INTERVAL;

  lastOLEDUpdate =
    millis() - OLED_INTERVAL;

  lastPageChange =
    millis();

  lastHistorySample =
    millis() - HISTORY_INTERVAL;
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ==========================================================
  // HOMESPAN
  // ==========================================================

  homeSpan.poll();


  // ==========================================================
  // TELNET
  // ==========================================================

  handleTerminal();


  unsigned long now =
    millis();


  // ==========================================================
  // SENSOR
  // ==========================================================

  if (
    now - lastSensorRead >=
    SENSOR_INTERVAL
  ) {

    lastSensorRead = now;

    readSEN66();
  }


  // ==========================================================
  // OLED PAGE CHANGE
  // ==========================================================

  if (
    now - lastPageChange >=
    PAGE_INTERVAL
  ) {

    lastPageChange = now;

    oledPage++;


    if (
      oledPage >=
      OLED_PAGE_COUNT
    ) {

      oledPage = 0;
    }
  }


  // ==========================================================
  // OLED UPDATE
  // ==========================================================

  if (
    now - lastOLEDUpdate >=
    OLED_INTERVAL
  ) {

    lastOLEDUpdate = now;

    updateOLED();
  }
}