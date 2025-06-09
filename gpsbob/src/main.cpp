#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <SoftwareSerial.h>
#include <WiFi.h>
#include <WebServer.h>
#include <qrcode.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3D
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

RTC_DS1307 rtc;
SoftwareSerial gpsSerial(16, 17); // RX, TX to GPS
#define SD_CS 5

#define REED_SLEEP  32
#define REED_LOG    33
#define REED_WIFI   25

WebServer server(80);
const char *ssid = "GPSLogger";
const char *password = "12345678";

bool gpsFix = false;

String readGPS() {
  String line = "";
  while (gpsSerial.available()) {
    char c = gpsSerial.read();
    if (c == '\n') break;
    line += c;
  }
  if (line.startsWith("$GPGGA")) {
    gpsFix = line.indexOf(",0,") == -1;
    return line;
  }
  gpsFix = false;
  return "NO_GPS";
}

void startLoggingMode() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Logging in 30s...");
  display.display();
  Serial.println("Logging in 30s...");
  delay(1000);

  File file;
  char filename[32];
  DateTime now = rtc.now();

  snprintf(filename, sizeof(filename), "/GPS_%02d%02d%02d.csv", now.year(), now.month(), now.day());
  file = SD.open(filename, FILE_APPEND);
  if (!file) {
    Serial.println("File error");
    display.println("File error");
    display.display();
    return;
  }
  Serial.println("Logging...");
  display.println("Logging...");
  display.display();

  for (int i = 0; i < 10; ++i) {
    String gpsData = readGPS();
    display.clearDisplay();
    display.setCursor(0, 0);
    if (gpsFix) {
      display.println("GPS Fix");
      file.println(gpsData);
      Serial.println("GPS Fix");
      Serial.println(gpsData);
    } else {
      display.println("No Fix");
      Serial.println("No Fix");
      file.println("No Fix");
    }
    display.display();
    delay(5000);
  }
  file.close();
}

void startWiFiMode() {
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  String url = "http://" + IP.toString();

  Serial.println(url);

  display.clearDisplay();
//   display.print("GPSLogger");
  display.setCursor(0, 0);
  display.println(url);
  Serial.println(ssid);
  Serial.println(password);
  display.display();

  server.on("/", HTTP_GET, []() {
    String html = "<h1>GPS Logs</h1><ul>";
    File root = SD.open("/");
    while (File file = root.openNextFile()) {
      String fname = file.name();
      html += "<li><a href='/download?file=" + fname + "'>" + fname + "</a> ";
      html += "<a href='/delete?file=" + fname + "'>(del)</a></li>";
      file.close();
    }
    html += "</ul>";
    server.send(200, "text/html", html);
  });

  server.on("/download", HTTP_GET, []() {
    if (!server.hasArg("file")) return server.send(400, "text/plain", "No file specified");
    File file = SD.open("/" + server.arg("file"));
    if (!file) return server.send(404, "text/plain", "File not found");
    server.streamFile(file, "text/plain");
    file.close();
  });

  server.on("/delete", HTTP_GET, []() {
    if (!server.hasArg("file")) return server.send(400, "text/plain", "No file specified");
    SD.remove("/" + server.arg("file"));
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.begin();
}

void setup() {
  pinMode(REED_SLEEP, INPUT_PULLUP);
  pinMode(REED_LOG, INPUT_PULLUP);
  pinMode(REED_WIFI, INPUT_PULLUP);

  if (digitalRead(REED_SLEEP) == LOW) {
    esp_deep_sleep_start();
  }

  Serial.begin(115200);
  gpsSerial.begin(9600);

  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.clearDisplay();

  display.println("Display ON");
  display.display();

  if (!rtc.begin()) {
    Serial.println("RTC Error");
    display.display();
  }

  if (!SD.begin(SD_CS)) {
    Serial.println("SD fail");
    display.display();
  }

  if (digitalRead(REED_LOG) == LOW) {
    startLoggingMode();
  }

  if (digitalRead(REED_WIFI) == LOW) {
    startWiFiMode();
  }
}

void loop() {
  server.handleClient();
}