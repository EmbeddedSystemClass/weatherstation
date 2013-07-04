/*

  Outdoor weather station
  Copyright (C) 2013 by Xose Pérez <xose dot perez at gmail dot com>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <LowPower.h>
#include <DHT22.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <LLAPSerial.h>
#include <PCF8583.h>
#include <Magnitude.h>

// DHT22 connections:
// Connect pin 1 (on the left) of the sensor to 3.3V
// Connect pin 2 of the sensor to whatever your DHT_PIN is
// Connect pin 4 (on the right) of the sensor to GROUND
// Connect a 10K resistor from pin 2 (data) to pin 1 (power) of the sensor

// BMP085 and PCF8586 connections:
// Connect VCC of the sensor to 3.3V (NOT 5.0V!)
// Connect GND to Ground
// Connect SCL to i2c clock - on '168/'328 Arduino Uno/Duemilanove/etc thats Analog 5
// Connect SDA to i2c data - on '168/'328 Arduino Uno/Duemilanove/etc thats Analog 4
// EOC is not used, it signifies an end of conversion
// XCLR is a reset pin, also not used here

// ===========================================
// Configuration
// ===========================================

#define BAUD_RATE 9600

#define USE_NOAA

#define BATT_PIN 0
#define PANEL_PIN 1
#define RADIO_SLEEP_PIN 4
#define DHT_PIN 5
#define ANEMOMETER_ADDRESS 0xA0
#define RAIN_GAUGE_ADDRESS 0xA2
#define NOTIFICATION_PIN 13

#define DHT_TYPE DHT22
#define VOLTAGE_REFERENCE_VALUE 1100
#define VOLTAGE_REFERENCE_CODE INTERNAL
#define BATT_VOLTAGE_FACTOR 4.28 // 99.8KOhm + 327.4KOhm
#define PANEL_VOLTAGE_FACTOR 9.09 // 100.2kOhm + 811kOhm
#define RADIO_DELAY 100

// Every measure takes rougly 1.35 seconds (1.3 being the warmup bellow)
// Sending adds 1.23 seconds to that
// The total awake time for a 5 measures cicle is (1.35*5+1.23 ~= 8 seconds)
// Besides, the sleep code is not very precise...

#define SLEEP_INTERVAL SLEEP_4S
#define MEASURE_EVERY 14 // take one measure every minute
#define SEND_EVERY 70 // that's 14*5
#define WARMUP_DELAY 1300

//#define DEBUG

// ===========================================
// Globals
// ===========================================

DHT22 dht(DHT_PIN);
Adafruit_BMP085 bmp;
LLAPSerial LLAP(Serial);
PCF8583 anemometer(ANEMOMETER_ADDRESS);
PCF8583 rain_gauge(RAIN_GAUGE_ADDRESS);

boolean bmp_ready = false;
unsigned long interval = 0;

Magnitude dht22_temperature;
Magnitude dht22_humidity;
Magnitude bmp085_pressure;
Magnitude bmp085_temperature;
Magnitude battery_voltage;
Magnitude panel_voltage;
Magnitude anemometer_count;
Magnitude rain_gauge_count;

// ===========================================
// Methods
// ===========================================

/*
 * radioSleep
 * Sets the radio to sleep
 *
 * @return void
 */
void radioSleep() {
    delay(RADIO_DELAY);
    digitalWrite(RADIO_SLEEP_PIN, HIGH);
    digitalWrite(NOTIFICATION_PIN, LOW);
}

/*
 * radioWake
 * Wakes up the radio to sleep
 *
 * @return void
 */
void radioWake() {
    digitalWrite(NOTIFICATION_PIN, HIGH);
    digitalWrite(RADIO_SLEEP_PIN, LOW);
    delay(RADIO_DELAY);
}

/*
 * readVoltage
 * Reads the analog PIN and performs the conversion to get mV
 *
 * @param byte pin PIN number
 * @param float factor conversion factor based on voltage divider
 * @return long
 */
long readVoltage(byte pin, float factor) {
    int reading = analogRead(pin);
    #ifdef DEBUG
        Serial.print("Voltage reading for PIN ");
        Serial.print(pin);
        Serial.print(": ");
        Serial.println(reading);
        delay(20);
    #endif
    return (long) map(reading, 0, 1023, 0, VOLTAGE_REFERENCE_VALUE) * factor;
}

#ifdef USE_NOAA

/*
 * dewPoint
 * Calculates dew point
 *
 * reference: http://wahiduddin.net/calc/density_algorithms.htm 
 *
 * @param temperature float temperature in celsius
 * @param humidity float humidity in %
 * @return float
 */
float dewPoint(float temperature, float humidity) {
    float A0= 373.15/(273.15 + temperature);
    float SUM = -7.90298 * (A0-1);
    SUM += 5.02808 * log10(A0);
    SUM += -1.3816e-7 * (pow(10, (11.344*(1-1/A0)))-1) ;
    SUM += 8.1328e-3 * (pow(10,(-3.49149*(A0-1)))-1) ;
    SUM += log10(1013.246);
    float VP = pow(10, SUM-3) * humidity;
    float T = log(VP/0.61078); // temp var
    return (241.88 * T) / (17.558 - T);
}

#else

/*
 * dewPoint
 * Calculates dew point
 * delta max = 0.6544 wrt dewPoint()
 * 5x faster than dewPoint()
 * reference: http://en.wikipedia.org/wiki/Dew_point
 *
 * @param temperature float temperature in celsius
 * @param humidity float humidity in %
 * @return float
 */
float dewPoint(float temperature, float humidity) {
    float a = 17.271;
    float b = 237.7;
    float temp = (a * temperature) / (b + temperature) + log(humidity/100);
    float Td = (b * temp) / (a - temp);
    return Td;
}

#endif

/*
 * readAnemometer
 * 
 * Reads anemometer counter
 */
void readAnemometer() {

    static unsigned long previous = 0;
    unsigned long current = anemometer.getCount();
    #ifdef DEBUG
        Serial.print("Anemometer count: ");
        Serial.println(current);
        delay(20);
    #endif

    float delta = current - previous;
    if (delta < 0) delta += 1000000.0;
    anemometer_count.store(delta);
    #ifdef DEBUG
        Serial.print("Anemometer delta: ");
        Serial.println(delta);
        delay(20);
    #endif

    previous = current;

}

/*
 * readRainGauge
 * 
 * Reads rain count gauge counter
 */
void readRainGauge() {

    static unsigned long previous = 0;
    unsigned long current = rain_gauge.getCount();
    #ifdef DEBUG
        Serial.print("Rain gauge count: ");
        Serial.println(current);
        delay(20);
    #endif

    float delta = current - previous;
    if (delta < 0) delta += 1000000.0;
    rain_gauge_count.store(delta);
    #ifdef DEBUG
        Serial.print("Rain gauge delta: ");
        Serial.println(delta);
        delay(20);
    #endif

    previous = current;

}

/*
 * readDHT22
 * Reads humidity and temperature from DHT22
 *
 * @return void
 */
void readDHT22() {

    // Allowing the DHT22 to warm up
    delay(WARMUP_DELAY);

    DHT22_ERROR_t errorCode = dht.readData();
    if (errorCode == DHT_ERROR_NONE) {
        dht22_temperature.store(dht.getTemperatureC());
        #ifdef DEBUG
            Serial.print("DHT22 temperature: ");
            Serial.println(dht.getTemperatureC());
            delay(20);
        #endif
        dht22_humidity.store(dht.getHumidity());
        #ifdef DEBUG
            Serial.print("DHT22 humidity: ");
            Serial.println(dht.getHumidity());
            delay(20);
        #endif
    }

}

/*
 * readBMP085
 * Reads pressure and temperature from BMP085
 *
 * @return void
 */
void readBMP085() {

    if (bmp_ready) {
        bmp085_temperature.store(bmp.readTemperature());
        #ifdef DEBUG
            Serial.print("BMP085 temperature: ");
            Serial.println(bmp.readTemperature());
            delay(20);
        #endif
        bmp085_pressure.store(bmp.readPressure());
        #ifdef DEBUG
            Serial.print("BMP085 pressure: ");
            Serial.println(bmp.readPressure());
            delay(20);
        #endif
    }

}

/*
 * readVoltages
 * Reads voltages for battery and solar panel
 *
 * @return void
 */
void readVoltages() {

    battery_voltage.store((float) readVoltage(BATT_PIN, BATT_VOLTAGE_FACTOR));
    panel_voltage.store((float) readVoltage(PANEL_PIN, PANEL_VOLTAGE_FACTOR));

}

/*
 * resetAll
 * Resets all averages
 *
 * @return void
 */
void resetAll() {
    dht22_humidity.reset();
    dht22_temperature.reset();
    bmp085_temperature.reset();
    bmp085_pressure.reset();
    battery_voltage.reset();
    panel_voltage.reset();
    anemometer_count.reset();
    rain_gauge_count.reset();
}

/*
 * sendAll
 * Gets averages and sends all data through UART link
 *
 * @return void
 */
void sendAll() {

    float tmp1 = dht22_temperature.average();
    float humi = dht22_humidity.average();
    float dewp = dewPoint(tmp1, humi);
    float tmp2 = bmp085_temperature.average();
    float pres = bmp085_pressure.average() / 100.0; // in hPa
    float bat1 = battery_voltage.average();
    float bat2 = panel_voltage.average();
    float wind = anemometer_count.average() * 5 / 16; // 1c/s = 2.5km/h, the buckets are 4 seconds wide, and it counts double!!
    float wndx = anemometer_count.maximum() * 5 / 16;
    float rain = rain_gauge_count.sum() * 3 / 20; // 0.3mm per count, the counter counts double!!

    radioWake();

    LLAP.sendMessage("T1", tmp1, 2);
    delay(RADIO_DELAY);
    LLAP.sendMessage("T2", tmp2, 2);
    delay(RADIO_DELAY);
    LLAP.sendMessage("HM", humi, 2);
    delay(RADIO_DELAY);
    LLAP.sendMessage("PS", pres, 1);
    delay(RADIO_DELAY);
    LLAP.sendMessage("DP", dewp, 2);
    delay(RADIO_DELAY);
    LLAP.sendMessage("WD", wind, 1);
    delay(RADIO_DELAY);
    LLAP.sendMessage("WX", wndx, 1);
    delay(RADIO_DELAY);
    LLAP.sendMessage("RN", rain, 1);
    delay(RADIO_DELAY);
    LLAP.sendMessage("B1", bat1, 0);
    delay(RADIO_DELAY);
    LLAP.sendMessage("B2", bat2, 0);

    radioSleep();

}

/*
 * setup
 *
 * @return void
 */
void setup() {

    // Using the ADC against internal 1V1 reference for battery monitoring
    analogReference(VOLTAGE_REFERENCE_CODE);

    // Initialize UART, LLAP, PCF8583 and BMP085
    Serial.begin(BAUD_RATE);
    LLAP.setLocalID("AB");
    anemometer.setMode(MODE_EVENT_COUNTER);
    anemometer.setCount(0);
    rain_gauge.setMode(MODE_EVENT_COUNTER);
    rain_gauge.setCount(0);
    bmp_ready = (bool) bmp.begin();

    // Link radio
    pinMode(RADIO_SLEEP_PIN, OUTPUT);
    pinMode(NOTIFICATION_PIN, OUTPUT);
    radioWake();
    delay(2000);
    LLAP.sendMessage("ST", "1");
    radioSleep();

}

/*
 * loop
 *
 * @return void
 */
void loop() {

    // Always take a wind measure
    readAnemometer();

    ++interval;

    // Now, every MEASURE_EVERY intervals (1 minute) take the rest of measures
    if (interval % MEASURE_EVERY == 0) {
        readDHT22();
        readBMP085();
        readVoltages();
        readRainGauge();
    }

    // And every SEND_EVERY intervals (5 minutes) send messages
    if (interval % SEND_EVERY == 0) {
        sendAll();
        resetAll();
        interval = 0;
    }

    LowPower.powerDown(SLEEP_INTERVAL, ADC_OFF, BOD_OFF);

}
