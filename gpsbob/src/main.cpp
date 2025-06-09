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
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
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

    Serial.println("GPS Fix Acquired");
    Serial.print("Lat: ");
    Serial.println(gps.location.lat(), 6);
    Serial.print("Lng: ");
    Serial.println(gps.location.lng(), 6);

    display.println("GPS Fix Acquired");
    display.print("Lat: ");
    display.println(gps.location.lat(), 6);
    display.print("Lng: ");
    display.println(gps.location.lng(), 6);
  } else {
    display.println("Waiting for GPS fix...");
    Serial.println("Waiting for GPS fix...");
  }

  display.display();
  delay(1000);
}
