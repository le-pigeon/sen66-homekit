#include <Arduino.h>
#include <Wire.h>

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

// SEN66 default I2C address
#define SEN66_ADDRESS 0x6B

// OLED I2C address
#define OLED_ADDRESS 0x3C


// ============================================================
// SEN66
// ============================================================

SensirionI2cSen66 sen66;


// ============================================================
// OLED
// ============================================================
//
// Important:
//
// U8g2's _2ND_HW_I2C constructor uses Arduino's Wire1 bus.
// Therefore we configure Wire1 with GPIO 6/7 below.
//
// SEN66 uses Wire  -> GPIO 1/2
// OLED  uses Wire1 -> GPIO 6/7
//

U8G2_SH1106_128X64_NONAME_F_2ND_HW_I2C oled(
  U8G2_R0,
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

const unsigned long SENSOR_INTERVAL = 1000;
const unsigned long OLED_INTERVAL = 250;
const unsigned long PAGE_INTERVAL = 5000;

uint8_t oledPage = 0;


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
//
// HomeKit has a general AirQuality characteristic.
//
// We derive it from PM2.5. This is only a coarse status
// indicator; the actual PM2.5 value is also exposed separately.
//

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

    return;
  }


  // ----------------------------------------------------------
  // Store new readings
  // ----------------------------------------------------------

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


  // ----------------------------------------------------------
  // HomeKit
  // ----------------------------------------------------------

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


  // CO2 detected status.
  //
  // This is NOT the actual CO2 measurement.
  // It is simply HomeKit's binary detected/not-detected
  // characteristic.

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


  // ----------------------------------------------------------
  // Serial output
  // ----------------------------------------------------------

  Serial.printf(
    "PM1 %.1f | PM2.5 %.1f | PM4 %.1f | PM10 %.1f | "
    "T %.1f C | RH %.1f %% | VOC %.0f | NOx %.0f | CO2 %u ppm\n",

    pm1,
    pm25,
    pm4,
    pm10,
    temperature,
    humidity,
    vocIndex,
    noxIndex,
    co2
  );
}


// ============================================================
// OLED PAGE 1
// ============================================================
//
// PM1
// PM2.5
// PM4
// PM10
// CO2
//

void drawOLEDPage1() {

  oled.setFont(u8g2_font_6x10_tf);

  oled.drawStr(
    0,
    9,
    "AIR QUALITY"
  );

  oled.drawHLine(
    0,
    12,
    128
  );


  oled.setCursor(0, 23);
  oled.printf(
    "PM1.0   %5.1f ug/m3",
    pm1
  );


  oled.setCursor(0, 33);
  oled.printf(
    "PM2.5   %5.1f ug/m3",
    pm25
  );


  oled.setCursor(0, 43);
  oled.printf(
    "PM4.0   %5.1f ug/m3",
    pm4
  );


  oled.setCursor(0, 53);
  oled.printf(
    "PM10    %5.1f ug/m3",
    pm10
  );


  oled.setCursor(0, 63);
  oled.printf(
    "CO2     %5u ppm",
    co2
  );
}


// ============================================================
// OLED PAGE 2
// ============================================================
//
// Temperature
// Humidity
// VOC Index
// NOx Index
//

void drawOLEDPage2() {

  oled.setFont(u8g2_font_6x10_tf);

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


  oled.setCursor(0, 25);
  oled.printf(
    "TEMP    %5.1f C",
    temperature
  );


  oled.setCursor(0, 37);
  oled.printf(
    "RH      %5.1f %%",
    humidity
  );


  oled.setCursor(0, 49);
  oled.printf(
    "VOC IDX %5.0f",
    vocIndex
  );


  oled.setCursor(0, 61);
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


  // Sensor hasn't produced a valid measurement yet.

  if (!sensorOK) {

    oled.setFont(u8g2_font_6x10_tf);

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


  if (oledPage == 0) {

    drawOLEDPage1();

  }
  else {

    drawOLEDPage2();
  }


  oled.sendBuffer();
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


  // ==========================================================
  // ACCESSORY INFORMATION
  // ==========================================================

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
  Serial.println("==============================");
  Serial.println(" SEN66 Air Quality Monitor");
  Serial.println(" ESP32-S3 + HomeSpan + OLED");
  Serial.println("==============================");


  // ==========================================================
  // SEN66 I2C
  // ==========================================================

  Serial.println(
    "Starting SEN66 I2C..."
  );


  // Wire = I2C bus 0
  // SEN66 = GPIO1 / GPIO2

  Wire.begin(
    SEN66_SDA,
    SEN66_SCL,
    100000
  );


  sen66.begin(
    Wire,
    SEN66_ADDRESS
  );


  // ----------------------------------------------------------
  // Reset SEN66
  // ----------------------------------------------------------

  int16_t error = sen66.deviceReset();


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


  // Sensirion's current example waits 1200 ms
  // after deviceReset().

  delay(1200);


  // ----------------------------------------------------------
  // Start continuous measurement
  // ----------------------------------------------------------

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


  // Give the sensor time to produce its first
  // measurement.

  delay(1100);


  // ==========================================================
  // OLED I2C
  // ==========================================================

  Serial.println(
    "Starting OLED I2C..."
  );


  // Wire1 = I2C bus 1
  // OLED = GPIO6 / GPIO7

  Wire1.begin(
    OLED_SDA,
    OLED_SCL,
    400000
  );


  oled.setI2CAddress(
    OLED_ADDRESS << 1
  );


  oled.begin();


  // ==========================================================
  // INITIAL OLED
  // ==========================================================

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


  // Force first sensor read soon.

  lastSensorRead = millis() - SENSOR_INTERVAL;
  lastOLEDUpdate = millis() - OLED_INTERVAL;
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // HomeSpan
  // ----------------------------------------------------------

  homeSpan.poll();


  unsigned long now = millis();


  // ----------------------------------------------------------
  // SENSOR
  // ----------------------------------------------------------

  if (
    now - lastSensorRead >= SENSOR_INTERVAL
  ) {

    lastSensorRead = now;

    readSEN66();
  }


  // ----------------------------------------------------------
  // PAGE CHANGE
  // ----------------------------------------------------------

  if (
    now - lastPageChange >= PAGE_INTERVAL
  ) {

    lastPageChange = now;

    oledPage++;

    if (oledPage > 1) {
      oledPage = 0;
    }
  }


  // ----------------------------------------------------------
  // OLED
  // ----------------------------------------------------------

  if (
    now - lastOLEDUpdate >= OLED_INTERVAL
  ) {

    lastOLEDUpdate = now;

    updateOLED();
  }
}