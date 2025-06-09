#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>

// === DISPLAY ===
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// === GPS ===
#define GPS_RX 16
#define GPS_TX 17
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;

// === SD Card ===
#define SD_CS 5
fs::File logFile;

// === State ===
unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 10000; // 10 seconds

// === Wi-Fi ===
const char *AP_SSID = "ESP32-Logger";
const char *AP_PASS = "12345678";
AsyncWebServer server(80);

// === Display Helper ===
void displayMessage(const String &msg) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.println(msg);
  display.display();
}

void WiFiLogMode() {
  displayMessage("Starting WiFi AP...");
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();

  // === List Files ===
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<html><head><title>GPS Logs</title></head><body>";
    html += "<h2>GPS Log Files</h2><ul>";

    fs::File root = SD.open("/");
    fs::File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String fname = file.name();
        size_t size = file.size();
        html += "<li><a href=\"/download?file=" + fname + "\">" + fname + "</a> ";
        html += String(size) + " bytes ";
        html += "<a href=\"/delete?file=" + fname + "\">[Delete]</a></li>";
      }
      file = root.openNextFile();
    }
    html += "</ul></body></html>";
    request->send(200, "text/html", html);
  });

  // === Download ===
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
      String fname = request->getParam("file")->value();
      if (!fname.startsWith("/")) fname = "/" + fname;

      if (SD.exists(fname)) {
        Serial.println("Sending file: " + fname);
        request->send(SD, fname, "text/plain");
      } else {
        Serial.println("404 - File not found: " + fname);
        request->send(404, "text/plain", "File not found: " + fname);
      }
    } else {
      request->send(400, "text/plain", "Missing file parameter");
    }
  });

  // === Delete ===
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
      String fname = request->getParam("file")->value();
      SD.remove(fname);
      request->redirect("/");
    } else {
      request->send(400, "text/plain", "File parameter missing");
    }
  });

  server.begin();

  // Display network info
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi Access Point:");
  display.println(AP_SSID);
  display.println("");
  display.print("http://");
  display.println(ip);
  display.display();
  delay(5000);
}

void setup() {
  Serial.begin(115200);

  // Init GPS
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  // Init OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED init failed");
    while (true);
  }

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.cp437(true);   

  // Init SD Card
  if (!SD.begin(SD_CS)) {
    display.println("SD init failed");
    display.display();
    Serial.println("SD Card init failed");
    while (true);
  }

// Create or append to GPS log file
  logFile = SD.open("/gps_log_file.csv", FILE_APPEND);
  if (!logFile) {
    display.println("File error");
    display.display();
    Serial.println("Failed to open log file");
    while (true);
  }

  // Header if file is new
  if (logFile.size() == 0) {
    logFile.println("Timestamp,Latitude,Longitude,Satellites,HDOP");
    logFile.flush();
  }

  display.println("GPS Logger Ready");
  display.display();
  delay(2000);

  WiFiLogMode();  // Start web server
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
    int sats = gps.satellites.value();
    double hdop = gps.hdop.hdop();

    // Serial
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
    Serial.println(sats);

    // Display
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
  


    // Logging
    if (millis() - lastLogTime >= LOG_INTERVAL) {
      lastLogTime = millis();

      char buffer[128];     
      snprintf(buffer, sizeof(buffer), "%.6f,%.6f,%d,%.2f", lat, lng, sats, hdop);

      logFile.print(gps.date.year());
      logFile.print("-");
      if (gps.date.month() < 10) logFile.print(F("0"));   
      logFile.print(gps.date.month());
      logFile.print("-");
      if (gps.date.day() < 10) logFile.print(F("0"));   
      logFile.print(gps.date.day());
      logFile.print(" UTC");
      if (gps.time.hour() < 10) logFile.print(F("0"));  
      logFile.print(gps.time.hour());
      logFile.print(":");
      if (gps.time.minute() < 10) logFile.print(F("0")); 
      logFile.print(gps.time.minute());
      logFile.print(":");
      if (gps.time.second() < 10) logFile.print(F("0")); 
      logFile.print(gps.time.second());
      logFile.print(", ");

      logFile.println(buffer);
      logFile.flush();
      Serial.println(buffer);
    }

  } else {
    display.println("GPS Wait..");
    Serial.println("Waiting for GPS fix...");
  }

  display.display();
  delay(1000);
}
