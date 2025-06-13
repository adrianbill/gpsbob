#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>
// #include "esp_sleep.h"

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

// === Logging SD Card ===
#define SD_CS 5
fs::File csvFile;
fs::File gpxFile;
bool gpxHeaderWritten = false;
bool csvHeaderWritten = false;
int timezoneOffsetHours = 0;            // Default UTC
int LOG_INTERVAL = 30000;               // default 30 seconds
int LIVE_INTERVAL = 5000;               // default 5 seconds

// === State ===
String currentDateStr = "";
String lastTimestamp = "";
double lastLat = 0.0;
double lastLng = 0.0;
unsigned long lastLogTime = 0;
unsigned long lastLiveTime = 0;


// === Wi-Fi ===
AsyncWebServer server(80);
String wifiSSID = "GPS_BOB";            // Default SSID
String wifiPass = "12345678";           // Default password
bool wifiStarted = false;

// === Sleep & Modes ===
#define BUTTON_PIN 0
enum Mode {INFO_MODE, LIVE_MODE, LOG_MODE, WIFI_MODE};
uint32_t buttonPressTime = 0;
const uint16_t LONG_PRESS_MS = 3000; // Long press length, 3 seconds
const uint16_t DEBOUNCE_MS = 50;
bool sleepEnabled = false;
bool buttonWasPressed = false;
Mode currentMode = INFO_MODE;           //Default Start_Mode
// String Mode_Name = "Live Mode";         

// === Utilities ===
void loadConfig() {
  if (!SD.exists("/config.txt")) {
    Serial.println("No config.txt found, using defaults");
    return;
  }

  fs::File config = SD.open("/config.txt", FILE_READ);
  if (!config) {
    Serial.println("Failed to open config.txt");
    return;
  }

  while (config.available()) {
    String line = config.readStringUntil('\n');
    line.trim();

    if (line.startsWith("timezone=")) {
      String val = line.substring(9);
      timezoneOffsetHours = val.toInt();
      Serial.print("Loaded timezone offset: ");
      Serial.println(timezoneOffsetHours);
    }
    else if (line.startsWith("ssid=")) {
      wifiSSID = line.substring(5);
      wifiSSID.trim();
      Serial.print("Loaded SSID: ");
      Serial.println(wifiSSID);
    }
    else if (line.startsWith("password=")) {
      wifiPass = line.substring(9);
      wifiPass.trim();
      if (wifiPass.length() < 8) {
        Serial.println("Password too short, using default");
        wifiPass = "12345678";
      } else {
        Serial.print("Loaded password: ");
        Serial.println(wifiPass);
      }
    } 
    else if (line.startsWith("log_interval=")) {
      String val = line.substring(13);
      int interval = val.toInt() * 1000;
      if (interval >= 1000) LOG_INTERVAL = interval;
      Serial.print("Loaded log interval: ");
      Serial.print(LOG_INTERVAL / 1000);
      Serial.println(" seconds");
    }
    else if (line.startsWith("live_interval=")) {
      String val = line.substring(13);
      int interval = val.toInt() * 1000;
      if (interval >= 1000) LIVE_INTERVAL = interval;
      Serial.print("Loaded live interval: ");
      Serial.print(LIVE_INTERVAL / 1000);
      Serial.println(" seconds");
    }
  }

  config.close();
}

// === Date & Time conversion ===
String toISO8601(TinyGPSDate date, TinyGPSTime time) {
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           date.year(), date.month(), date.day(),
           time.hour(), time.minute(), time.second());
  return String(buf);
}

String toISO8601Local(TinyGPSDate date, TinyGPSTime time, int offsetHours) {
  int year = date.year();
  int month = date.month();
  int day = date.day();
  int hour = time.hour();
  int minute = time.minute();
  int second = time.second();
  
  int new_hour = hour + offsetHours;

  int new_day = day;
  int new_month = month;
  int new_year = year;
  
  if (new_hour < 0)
  {
    new_day = day - 1;
    new_hour = 24 + new_hour;
  }

  if (new_day < 0)
  {
    int days = 0;

    new_month = month - 1;
    
    if (new_month < 0)
    {
      new_month = 12 + new_month;
      new_year = year - 1;
    }
    
    switch (new_month)
    {
    case 1:
      days = 31;
      break;
    case 2:
      if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) days = 29; //leap year
      else days = 28;
      break;
    case 3:
      days = 31;
      break;
    case 4:
      days = 30;
      break;
    case 5:
      days = 31;
      break;
    case 6:
      days = 30;
      break;
    case 7:
      days = 31;
      break;
    case 8:
      days = 31;
      break;  
    case 9:
      days = 30;
      break;
    case 10:
      days = 31;
      break;
    case 11:
      days = 30;
      break;
    case 12:
      days = 31;
      break;
    }
    new_day = days + new_day;

  }

  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           new_year, new_month, new_day, new_hour, minute, second);
  return String(buf);
}

String gpsDateStamp(TinyGPSDate date) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%04d%02d%02d", date.year(), date.month(), date.day());
  return String(buf);
}

// === display tool (maybe reo) ===

void displayText(const String &text, int size, int clear) {
  if (clear == 1) 
  {
    display.clearDisplay();
    display.setCursor(0, 0);
  }
  
  display.setTextSize(size);
  display.println(text); 
  display.display();
}

void displayData(const String &title, const String &timeLocal, double lat, double lng) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.println(title);
  display.println(timeLocal);
  display.println("Latitude: ");
  display.setTextSize(2);
  if (lat>=0 && lat<100) display.print("  ");
  if (lat<0 && lat>-100) display.print(" ");
  display.println(lat, 5);
  display.setTextSize(1);
  display.println("Longitude: ");
  display.setTextSize(2);
  if (lng>=0 && lng<100) display.print("  ");
  if (lng<0 && lng>-100) display.print(" ");  
  display.println(lng, 5);
  display.display();
  }
// === Logging ===
void openLogFiles(const String &dateStr) {
  currentDateStr = dateStr;

  String csvName = "/log_" + currentDateStr + ".csv";
  bool newFile_csv = !SD.exists(csvName);
  csvFile = SD.open(csvName, FILE_APPEND);
  csvHeaderWritten = newFile_csv;

  if (csvFile && csvHeaderWritten) {
    csvFile.println("Timestamp(Local),Latitude,Longitude,Satilites,HDOP,OffsetUTC");
    csvFile.flush();
  }

  String gpxName = "/track_" + currentDateStr + ".gpx";
  bool newFile_gpx = !SD.exists(gpxName);
  gpxFile = SD.open(gpxName, FILE_APPEND);
  gpxHeaderWritten = newFile_gpx;

  if (gpxFile && gpxHeaderWritten) {
    gpxFile.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    gpxFile.println("<gpx version=\"1.1\" creator=\"ESP32 Logger\"");
    gpxFile.println(" xmlns=\"http://www.topografix.com/GPX/1/1\"");
    gpxFile.println(" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"");
    gpxFile.println(" xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1");
    gpxFile.println(" http://www.topografix.com/GPX/1/1/gpx.xsd\">");
    gpxFile.println("<trk><name>GPSBOB Log</name><trkseg>");
    gpxFile.flush();
  }
}

void logData(const String &isoTimeLocal, const String &isoTimeUTC, int display_text){
  double lat = gps.location.lat();
  double lng = gps.location.lng();
  int sats = gps.satellites.value();
  double hdop = gps.hdop.hdop();

  String csv =  isoTimeLocal + "," +
              String(lat, 6) + "," +
              String(lng, 6) + "," +
              String(sats) + "," +
              String(hdop, 2) + "," +
              String(timezoneOffsetHours);

  if (csvFile) {
    csvFile.println(csv);
    csvFile.flush();
  }

  if (gpxFile) {
    gpxFile.print("<trkpt lat=\"");
    gpxFile.print(lat, 6);
    gpxFile.print("\" lon=\"");
    gpxFile.print(lng, 6);
    gpxFile.println("\">");
    gpxFile.print("  <time>");
    gpxFile.print(isoTimeUTC);
    gpxFile.println("</time>");
    gpxFile.println("</trkpt>");
    gpxFile.flush();
  }

  Serial.println(csv);

  lastTimestamp = isoTimeLocal;
  lastLat = lat;
  lastLng = lng;
}

void closeGPX() {
  if (gpxFile) {
    gpxFile.println("</trkseg></trk></gpx>");
    gpxFile.flush();
    gpxFile.close();
  }
}

// === Web Server ===
void startWiFiServer() {
  if (wifiStarted) return;
  
  WiFi.softAP(wifiSSID.c_str(), wifiPass.c_str());
  IPAddress ip = WiFi.softAPIP();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<html><head><title>GPS Logs</title></head><body><h2>Files</h2><ul>";
    fs::File root = SD.open("/");
    fs::File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String name = file.name();
        html += "<li><a href=\"/download?file=" + name + "\">" + name + "</a> ";
        html += String(file.size()) + " bytes ";
        html += "<a href=\"/delete?file=" + name + "\">[Delete]</a></li>";
      }
      file = root.openNextFile();
    }
    html += "</ul></body></html>";
    request->send(200, "text/html", html);
  });

  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
      String fname = request->getParam("file")->value();
      if (!fname.startsWith("/")) fname = "/" + fname;
      if (SD.exists(fname)) {
        request->send(SD, fname, "text/plain", true);
      } else {
        request->send(404, "text/plain", "File not found: " + fname);
      }
    } else {
      request->send(400, "text/plain", "Missing file parameter");
    }
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
      String fname = request->getParam("file")->value();
      if (!fname.startsWith("/")) fname = "/" + fname;
      SD.remove(fname);
      request->redirect("/");
    } else {
      request->send(400, "text/plain", "Missing file parameter");
    }
  });

  server.begin();
  wifiStarted = true;

  display.println("WiFi Enabled");
  display.print("SSID:");
  display.println(wifiSSID);
  display.print("Addr: ");
  display.println(ip);
  display.display();
}

void stopWiFiServer() {
  if (!wifiStarted) return;
  WiFi.softAPdisconnect(true);
  server.end();
  wifiStarted = false;
  // displayText(Mode_Name,1,1);
  // displayText("WIFI Off",1,0);
}

// === Button Handling ===
void handleButton() {
  static unsigned long lastChange = 0;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;

  if (pressed && !buttonWasPressed && millis() - lastChange > DEBOUNCE_MS) {
    buttonPressTime = millis();
    buttonWasPressed = true;
    lastChange = millis();
  }

  if (!pressed && buttonWasPressed) {
    buttonWasPressed = false;
    unsigned long pressDuration = millis() - buttonPressTime;

    if (pressDuration > LONG_PRESS_MS) {
      // Long press → toggle sleep
      sleepEnabled = !sleepEnabled;
      Serial.println(sleepEnabled ? "Entering Deep Sleep" : "Waking up");
      if (sleepEnabled) {
        displayText("Sleep\nEntering Sleep...\nLong Press Button to Wake up",1,1);
        delay(3000);
        esp_deep_sleep_start();
      } else {
        // Just wake normally
        currentMode = INFO_MODE;
      }
    } else {
      // Short press → cycle mode
      currentMode = (Mode)((currentMode + 1) % 4);
      switch (currentMode) {
        case INFO_MODE:
          stopWiFiServer();

          loadConfig();

          displayText("Info Mode", 1, 1);
          display.println("");

          display.print("Timezone offset: ");
          display.println(timezoneOffsetHours);

          display.print("Log interval: ");
          display.print(LOG_INTERVAL / 1000);
          display.println(" s");

          display.print("Live interval: ");
          display.print(LIVE_INTERVAL / 1000);
          display.println(" s");

          display.display();

          Serial.println("Switch to INFO");
          break;

        case LOG_MODE:
          stopWiFiServer();
          // gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
          displayData("Log Mode - Last Log",lastTimestamp,lastLat,lastLng);
          Serial.println("Switch to LOG");
          break;

        case LIVE_MODE:       
          stopWiFiServer();
          // gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
          displayData("Live Mode - Last Log",lastTimestamp,lastLat,lastLng);
          // displayText("LIVE MODE - Last Log",1,1);
          Serial.println("Switch to LIVE");
          break;

        case WIFI_MODE:
          displayText("WIFI MODE",1,1);
          // gpsSerial.end();
          startWiFiServer();
          Serial.println("Switch to WIFI");
          break;
      }
      // switch (currentMode) {
      //   case LOG_MODE:  
      //     Mode_Name = "Log Mode";
      //     stopWiFiServer();
      //     delay(2000);
      //     break;
      //   case WIFI_MODE: 
      //     Mode_Name = "Wi-Fi Mode";
      //     waiting = 2;
      //     startWiFiServer();
      //     break;
      //   case LIVE_MODE: 
      //     Mode_Name = "Live Mode";
      //     stopWiFiServer();
      //     delay(2000);
      //     break;
      // }
    }
  }
}

// === Setup ===
void setup() {
  Serial.begin(115200);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.setTextColor(SSD1306_WHITE);
  
  while (!SD.begin(SD_CS)) displayText("Error\nSD Error\nCheck if installed and Reset",1,1);

  loadConfig();

  displayText("Info Mode", 1, 1);
  display.println("");

  display.print("Timezone offset: ");
  display.println(timezoneOffsetHours);

  display.print("Log interval: ");
  display.print(LOG_INTERVAL / 1000);
  display.println(" s");

  display.print("Live interval: ");
  display.print(LIVE_INTERVAL / 1000);
  display.println(" s");

  display.display();
  // delay(5000);
}

// === Main Loop ===
void loop() {
  handleButton();

  if (currentMode == WIFI_MODE || currentMode == INFO_MODE) return;

  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  if (!gps.location.isValid() || !gps.date.isValid() || !gps.time.isValid()) return;

//   if (!gps.location.isValid() || !gps.date.isValid() || !gps.time.isValid()){
//     if (waiting == 0) {
//       // displayText("Mode_Name",1,1);
//       displayText("Waiting for GPS...",1,0);
//       waiting = 1;
//       Serial.println("Waiting for GPS...");
//       return;
//     } else if (waiting == 1) return;
//   }

  // if (!gps.location.isValid() || !gps.date.isValid() || !gps.time.isValid()) {
  //   Serial.println("No valid GPS data yet...");
  //   Serial.print("Chars processed: "); Serial.println(gps.charsProcessed());
  //   displayText("Waiting for GPS...", 1, 1);
  //   delay(2000); // slow refresh to avoid flicker
  //   return;
  // }
  
  // Get timestamp info
  TinyGPSDate date = gps.date;
  TinyGPSTime time = gps.time;

  String today = gpsDateStamp(date);

  String isoTimeLocal = toISO8601Local(date, time, timezoneOffsetHours);
  String isoTimeUTC = toISO8601(date, time);

  double lat = gps.location.lat();
  double lng = gps.location.lng();
  int sats = gps.satellites.value();
  double hdop = gps.hdop.hdop();

  String csv =  isoTimeLocal + "," +
              String(lat, 6) + "," +
              String(lng, 6) + "," +
              String(sats) + "," +
              String(hdop, 2) + "," +
              String(timezoneOffsetHours);

  // Switch logging file if date changed
  if (today != currentDateStr) {
    closeGPX();
    openLogFiles(today);
  }

  switch (currentMode) {
    case LIVE_MODE:
      if (millis() - lastLiveTime >= LIVE_INTERVAL) {
        displayData("Live Mode - Int " + String(LIVE_INTERVAL / 1000) + " s",isoTimeLocal,lat,lng);
        logData(isoTimeLocal, isoTimeUTC, 1);
        lastLiveTime = millis();
      }
      // displayData("Live Mode",isoTimeLocal,lat,lng);
      break;

    case LOG_MODE:
      if (millis() - lastLogTime >= LOG_INTERVAL) {
        Serial.println("Logging");
        displayData("Log Mode - Int " + String(LOG_INTERVAL / 1000) + " s",isoTimeLocal,lat,lng);
        logData(isoTimeLocal, isoTimeUTC, 1);
        lastLogTime = millis();
      }
      break;
  }
}