#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>
// #include <RTClib.h>

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
fs::File csvFile;
fs::File gpxFile;

// === State ===
String currentDateStr = "";
unsigned long lastLogTime = 0;
#define LOG_INTERVAL 10000
bool gpxHeaderWritten = false;

// === Wi-Fi ===
AsyncWebServer server(80);


int timezoneOffsetHours = 0;            // Default UTC
String wifiSSID = "GPS_BOB";            // Default SSID
String wifiPass = "12345678";           // Default password

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
  }

  config.close();
}

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
      if (new_year % 400 == 0) days = 29; //leap year
      else if (new_year % 100 == 0) days = 28;
      else if (new_year % 4 == 0) days = 29; //leap year
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

void displayMessage(const String &msg) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.println(msg);
  display.display();
}

// === Logging ===
void openLogFiles(const String &dateStr) {
  currentDateStr = dateStr;

  String csvName = "/log_" + currentDateStr + ".csv";
  csvFile = SD.open(csvName, FILE_APPEND);
  if (csvFile && csvFile.size() == 0) {
    csvFile.println("Timestamp(Local),Lat,Lon,Sats,HDOP,OffsetUTC");
    csvFile.flush();
  }

  String gpxName = "/track_" + currentDateStr + ".gpx";
  bool newFile = !SD.exists(gpxName);
  gpxFile = SD.open(gpxName, FILE_APPEND);
  gpxHeaderWritten = newFile;

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

void closeGPX() {
  if (gpxFile) {
    gpxFile.println("</trkseg></trk></gpx>");
    gpxFile.flush();
    gpxFile.close();
  }
}

// === Web Server ===
void startWiFiServer() {
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

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi Mode:");
  display.println(wifiSSID);
  display.print("http://");
  display.println(ip);
  display.display();
}

// === Setup ===
void setup() {
  Serial.begin(115200);
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);

  if (!SD.begin(SD_CS)){
    displayMessage("SD error");
  } else {
  loadConfig();
  }

  displayMessage("Waiting for GPS...");
  delay(1000);

  startWiFiServer();
}

// === Main Loop ===
void loop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  if (!gps.location.isValid() || !gps.date.isValid() || !gps.time.isValid()) return;

  TinyGPSDate date = gps.date;
  TinyGPSTime time = gps.time;

  String today = gpsDateStamp(date);
  if (today != currentDateStr) {
    closeGPX();
    openLogFiles(today);
  }

  double lat = gps.location.lat();
  double lng = gps.location.lng();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.println(toISO8601Local(date, time, timezoneOffsetHours));
  display.println("");
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


  if (millis() - lastLogTime >= LOG_INTERVAL) {
    lastLogTime = millis();

    String isoTimeLocal = toISO8601Local(date, time, timezoneOffsetHours);
    String isoTimeUTC = toISO8601(date, time);

    String csv =  isoTimeLocal + "," +
                  String(gps.location.lat(), 6) + "," +
                  String(gps.location.lng(), 6) + "," +
                  String(gps.satellites.value()) + "," +
                  String(gps.hdop.hdop(), 2) + "," +
                  String(timezoneOffsetHours);

    if (csvFile) {
      csvFile.println(csv);
      csvFile.flush();
    }

    if (gpxFile) {
      gpxFile.print("<trkpt lat=\"");
      gpxFile.print(gps.location.lat(), 6);
      gpxFile.print("\" lon=\"");
      gpxFile.print(gps.location.lng(), 6);
      gpxFile.println("\">");
      gpxFile.print("  <time>");
      gpxFile.print(isoTimeUTC);
      gpxFile.println("</time>");
      gpxFile.println("</trkpt>");
      gpxFile.flush();
    }

    Serial.println(csv);
  }
}