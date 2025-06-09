#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>

// OLED display config
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// GPS on Serial2
#define GPS_RX 16
#define GPS_TX 17
HardwareSerial gpsSerial(2);

// TinyGPS++ instance
TinyGPSPlus gps;

void setup() {
  Serial.begin(115200);

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.cp437(true);   
  Serial.println("Waiting for GPS...");
  display.println("Waiting for GPS...");
  display.display();
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  display.clearDisplay();
  display.setCursor(0, 0);

  if (gps.location.isUpdated()) {
    double lat = gps.location.lat();
    double lng = gps.location.lng();

    // Serial.println("GPS Lock");
    Serial.print(gps.date.year());
    Serial.print("-");
    if (gps.date.month() < 10) Serial.print(F("0"));   
    Serial.print(gps.date.month());
    Serial.print("-");
    if (gps.date.day() < 10) Serial.print(F("0"));   
    Serial.print(gps.date.day());
    Serial.print(" UTC");
    if (gps.time.hour() < 10) Serial.print(F("0"));  
    Serial.print(gps.time.hour());
    Serial.print(":");
    if (gps.time.minute() < 10) Serial.print(F("0")); 
    Serial.print(gps.time.minute());
    Serial.print(":");
    if (gps.time.second() < 10) Serial.print(F("0")); 
    Serial.print(gps.time.second());
    Serial.print(", ");
    Serial.print(lat, 6);
    Serial.print(", ");
    Serial.print(lng, 6);
    Serial.print(", Sat: ");
    Serial.println(gps.satellites.value());

    // display.println("GPS Lock");
    display.setTextSize(1);
    display.println("Latitude");
    display.setTextSize(2);
    if (lat>=0 && lat<100) display.print("  ");
    if (lat<0 && lat>-100) display.print(" ");
    display.println(lat, 5);
    display.setTextSize(1);
    display.println("");
    display.println("Longitude");
    display.setTextSize(2);
    if (lng>=0 && lng<100) display.print("  ");
    if (lng<0 && lng>-100) display.print(" ");
    display.println(lng, 5);
  } else {
    display.println("GPS Wait..");
    Serial.println("Waiting for GPS fix...");
  }

  display.display();
  delay(1000);
}
