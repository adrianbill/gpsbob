#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>
#include "driver/rtc_io.h"

// #include <U8g2lib.h>

// #include "esp_sleep.h"

/* 
SPI  2
GPIO 13 MISO
GPIO 12 CLK
GPIO 11 MOSI
GPIO 10 CS

I2C
GPIO 9 (SCL)
GPIO 8 (SDA) 
*/

// #define SDA D4
// #define SCL D5

#define SD_CS D2 // 10


#define GPS_RX D7 // default
#define GPS_TX D6 // default

// const int BUTTON_PIN = 1;  
#define BUTTON_PIN GPIO_NUM_2
#define BATTERY_PIN GPIO_NUM_1 // ADC

// === DISPLAY ===
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// === GPS ===

HardwareSerial gpsSerial(0);
TinyGPSPlus gps;

// === Logging SD Card ===

fs::File csvFile;
fs::File gpxFile;
bool gpxHeaderWritten = false;
bool csvHeaderWritten = false;
int timezoneOffsetHours = 0;            // Default UTC
int LOG_INTERVAL = 30000;               // default 30 seconds
int LIVE_INTERVAL = 5000;               // default 5 seconds

// === State ===
String currentDateStr = "";
String lastTimestamp = "Waiting for GPS";
String lastDisplay = "";
// double lastLat = 48.424970;
// double lastLng = -123.373445;
double lastLat = 0.0;
double lastLng = 0.0;
unsigned long lastLogTime = 0;
unsigned long lastLiveTime = 0;
unsigned long lastbatTime = 0;
double WaypointLat = 0.0;
double WaypointLng = 0.0;
uint32_t startGetFixmS;
uint32_t endFixmS;
uint32_t FixTimemS;

// === Wi-Fi ===
AsyncWebServer server(80);
String wifiSSID = "GPS_BOB";            // Default SSID
String wifiPass = "12345678";           // Default password
bool wifiStarted = false;

// === Sleep & Modes ===

RTC_DATA_ATTR int bootCount = 0;
enum Mode {INFO_MODE, LIVE_MODE, LOG_MODE, NAV_MODE, WIFI_MODE};
uint32_t buttonPressTime = 0;
const uint16_t LONG_PRESS_MS = 3000; // Long press length, 3 seconds
const uint16_t DEBOUNCE_MS = 50;
bool sleepEnabled = false;
bool buttonWasPressed = false;
Mode currentMode = INFO_MODE;           //Default Start_Mode
bool update_display = true;
bool first_load = true;
int bat_ind = 0;
// String Mode_Name = "Live Mode";         

bool coord_updated( double lat, double lng);

// === Utilities ===
float battery_voltage() {
  uint32_t Vbatt = 0;
  for(int i = 0; i < 16; i++) Vbatt = Vbatt + analogReadMilliVolts(BATTERY_PIN); // ADC with correction   
  return 2 * Vbatt / 16 / 1000.0;     // attenuation ratio 1/2, mV --> V
}

void battery_percentage(float v) {
 	if (v >= 4.2) bat_ind = 100;
  else if (v > 4.15) bat_ind = 95;
  else if (v > 4.11) bat_ind = 90;
	else if (v > 4.08) bat_ind = 85;
	else if (v > 4.02) bat_ind = 80;
	else if (v > 3.98) bat_ind = 75;
	else if (v > 3.95) bat_ind = 70;
	else if (v > 3.91) bat_ind = 65;
	else if (v > 3.87) bat_ind = 60;
	else if (v > 3.85) bat_ind = 55;
	else if (v > 3.84) bat_ind = 50;
	else if (v > 3.82) bat_ind = 45;
	else if (v > 3.80) bat_ind = 40;
	else if (v > 3.79) bat_ind = 35;
	else if (v > 3.77) bat_ind = 30;
	else if (v > 3.75) bat_ind = 25;
	else if (v > 3.73) bat_ind = 20;
	else if (v > 3.71) bat_ind = 15;
	else if (v > 3.69) bat_ind = 10;
	else if (v > 3.61) bat_ind = 5;
	else if (v < 3.50) bat_ind = 0;
}

void battery_update() {
	battery_percentage(battery_voltage());
}

// void battery_display() {
// 	int x = display.getCursorX();
// 	int y = display.getCursorY();
// 	display.setTextSize(1);
// 	display.setCursor(display.width() - (6 * 6), 0);
// 	display.print(bat_ind);
// 	display.display();
// 	display.setCursor(x, y);
// }

void battery_display(){

	int cell_width = 18;
	int cell_height = 7;
	int cell_xstart = display.width() - cell_width;
	int fill_gap = 2;
	if (bat_ind == 100) fill_gap = 0;
	int max_fill_width = cell_width - (2 * fill_gap);
	int fill_width = (max_fill_width * bat_ind) / 100;
	int fill_start = (cell_xstart + fill_gap) + (max_fill_width - fill_width);

	display.fillRect(cell_xstart - 2, 2, 2, 3, WHITE);
	display.drawRect(cell_xstart, 0, cell_width, cell_height, WHITE);
	display.fillRect(fill_start, fill_gap, fill_width, cell_height - (2 * fill_gap), WHITE);
	display.display();
}

void loadConfig() {
  if (!SD.exists("/config.txt")) {
    // Serial.println("No config.txt found, using defaults");
    return;
  }

  fs::File config = SD.open("/config.txt", FILE_READ);
  if (!config) {
    // Serial.println("Failed to open config.txt");
    return;
  }

  while (config.available()) {
    String line = config.readStringUntil('\n');
    line.trim();

    if (line.startsWith("timezone=")) {
      String val = line.substring(9);
      timezoneOffsetHours = val.toInt();
      // Serial.print("Loaded timezone offset: ");
      // Serial.println(timezoneOffsetHours);
    }
    else if (line.startsWith("ssid=")) {
      wifiSSID = line.substring(5);
      wifiSSID.trim();
      // Serial.print("Loaded SSID: ");
      // Serial.println(wifiSSID);
    }
    else if (line.startsWith("password=")) {
      wifiPass = line.substring(9);
      wifiPass.trim();
      if (wifiPass.length() < 8) {
        // Serial.println("Password too short, using default");
        wifiPass = "12345678";
      } else {
        // Serial.print("Loaded password: ");
        // Serial.println(wifiPass);
      }
    } 
    else if (line.startsWith("log_interval=")) {
      String val = line.substring(13);
      int interval = val.toInt() * 1000;
      if (interval >= 1000) LOG_INTERVAL = interval;
      // Serial.print("Loaded log interval: ");
      // Serial.print(LOG_INTERVAL / 1000);
      // Serial.println(" seconds");
    }
    else if (line.startsWith("live_interval=")) {
      String val = line.substring(14);
      int interval = val.toInt() * 1000;
      if (interval >= 1000) LIVE_INTERVAL = interval;
      // Serial.print("Loaded live interval: ");
      // Serial.print(LIVE_INTERVAL / 1000);
      // Serial.println(" seconds");
    }
    else if (line.startsWith("Latitude=")) {
      String val = line.substring(9);
      double wayLat = val.toDouble() ;
      if (wayLat != 0) WaypointLat = wayLat;
      // Serial.print("Loaded Waypoint Latitude: ");
      // Serial.println(WaypointLat,6);
    }
    else if (line.startsWith("Longitude=")) {
      String val = line.substring(10);
      double wayLng = val.toDouble() ;
      if (wayLng != 0) WaypointLng = wayLng;
      // Serial.print("Loaded Waypoint Longitude: ");
      // Serial.println(WaypointLng,6);
    }
  }
  config.close();
}

bool replaceConfigLine(const char* filename, const String& key, const String& newValue) {
  File original = SD.open(filename, FILE_READ);
  if (!original) return false;

  File temp = SD.open("/temp.txt", FILE_WRITE);
  if (!temp) {
    original.close();
    return false;
  }

  while (original.available()) {
    String line = original.readStringUntil('\n');
    line.trim(); // Remove \r and whitespace
    if (line.startsWith(key + "=")) {
      temp.println(key + "=" + newValue);
    } else {
      temp.println(line);
    }
  }

  original.close();
  temp.close();

  SD.remove(filename);
  SD.rename("/temp.txt", filename);
  return true;
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

void displayGPSData(const String &title, const String &timeLocal, double lat, double lng) {

  // char buffer[22];
  // sprintf(buffer, "%9.5f, %9.5f", lat, lng);

  // String newCoord = buffer;
  
  // if (newCoord == lastDisplay) return;

  // lastDisplay = buffer;

  if (coord_updated(lat, lng)) return;
  
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
	battery_display();
  display.println(title);
  display.println(timeLocal);
  display.println("Lat");
  display.setTextSize(2);
  display.setCursor(2, display.getCursorY());
  if (lat>=0 && lat<100) display.print("  ");
  if (lat<0 && lat>-100) display.print(" ");
  display.println(lat, 5);
  display.setTextSize(1);
  display.println("Lon");
  display.setTextSize(2);
  display.setCursor(2, display.getCursorY());
  if (lng>=0 && lng<100) display.print("  ");
  if (lng<0 && lng>-100) display.print(" ");  
  display.println(lng, 5);
  display.display();
  }

void displayNAVData(const String &title, const String &timeLocal, double GPSlat, double GPSlng, double WAYlat, double WAYlng) {
  
  if (coord_updated(GPSlat, GPSlng)) return;
    
  double distance = TinyGPSPlus::distanceBetween(lastLat, lastLng, WaypointLat, WaypointLng);
  double courseToWaypoint = TinyGPSPlus::courseTo(lastLat, lastLng, WaypointLat, WaypointLng);
  const char *cardinal = TinyGPSPlus::cardinal(courseToWaypoint);
  
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
	battery_display();
  display.println(title);
  display.println(timeLocal);

  char buffer[16];

  if (distance < 1000) sprintf(buffer, "%5.f m", distance);
  else if (distance < 10000000) sprintf(buffer, " %6.1f km", distance / 1000.0);
  else {
    sprintf(buffer, ">10,000 km");
    display.printf("%8.4f, %8.4f",WaypointLat, WaypointLng);
    display.println("");
    display.setCursor((display.width() - (10 * 12)) / 2, display.getCursorY() + 4);
    display.setTextSize(2);
    display.println(buffer);
    display.setCursor((display.width() - (7 * 12)) / 2, display.getCursorY() + 4);
    display.print("Nav OFF");
    display.display();
    return;
  }

  display.printf("%8.4f, %8.4f",WaypointLat, WaypointLng);
  display.println("");
  display.setCursor(0, display.getCursorY() + 4);
  display.setTextSize(2);
  display.print(buffer);
  
  // // Serial.print("Distance: ");
  // // Serial.println(buffer);

  display.setTextSize(1);
  display.println("");
  int x = display.getCursorX();
  int y = display.getCursorY();

  display.setCursor(x, y + 4);

  sprintf(buffer, " %6.1f", courseToWaypoint);

  display.println("");
  display.setTextSize(2);
  display.print(buffer);
  x = display.getCursorX();
  y = display.getCursorY();
  display.drawCircle(x + 2, y - 1, 3, WHITE); // nice looking degree symbol  
  display.setTextSize(1);  
  x = display.getCursorX();
  y = display.getCursorY();
  display.println("");
  display.setCursor(x, (display.getCursorY() + y ) / 2);
  display.print(" (");
  display.print(cardinal);
  display.print(")");
  display.display();

  // // Serial.print("Bearing: ");
  // // Serial.print(buffer);
  // // Serial.print("° (");
  // // Serial.print(cardinal);
  // // Serial.println(")");
  }

void displayInfo() {
  displayText("Info Mode      ", 1, 1);
	display.print("Bat: ");
	display.print(battery_voltage(), 2);
	display.println(" V");

  display.print("Timezone offset: ");
  display.println(timezoneOffsetHours);

  display.print("Log interval: ");
  display.print(LOG_INTERVAL / 1000);
  display.println(" s");

  display.print("Live interval: ");
  display.print(LIVE_INTERVAL / 1000);
  display.println(" s");

  char buffer [20];

  display.println("Waypoint");
  sprintf(buffer, " Lat: %11.6f", WaypointLat);
  display.println(buffer);
  sprintf(buffer, " Lon: %11.6f", WaypointLng);
  display.print(buffer);
	battery_display();
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

  // Serial.println(csv);
}

void closeGPX() {
  if (gpxFile) {
    gpxFile.println("</trkseg></trk></gpx>");
    gpxFile.flush();
    gpxFile.close();
  }
}

void startWiFiServer() {
  if (wifiStarted) return;
  wifiStarted = true;

  WiFi.softAP(wifiSSID.c_str(), wifiPass.c_str());
  IPAddress IP = WiFi.softAPIP();
  // Serial.print("AP IP address: ");
  // Serial.println(IP);

  // Root route
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = R"rawliteral(
      <!DOCTYPE html>
      <html>
      <head>
        <meta name='viewport' content='width=device-width, initial-scale=1'>
        <style>
          body { 
            font-family: sans-serif; 
            padding: 1em; 
          }
          input, select { 
            width: 100%; 
            padding: 0.5em; 
            margin: 0.5em 0; 
            font-size: 1em; 
          }

          .button {
            display: inline-block;
            width: 100%;
            padding: 0.5em;
            margin: 1em 0 0 0;
            font-size: 1em;
            background: #007bff;
            color: white;
            border: none;
            border-radius: 5px;
            text-align: center;
            text-decoration: none;
          }
          h1 {
            margin-bottom: 0.5em;
          }
        </style>
      </head>
      <body>
        <h2>GPS BOB</h2>
        <a class='button' href='/Waypoint'>Waypoint</a>
        <a class='button' href='/Settings'>Settings</a>
        <ul>
      )rawliteral";

    File root = SD.open("/");
    if (!root) {
      request->send(500, "text/plain", "Failed to open SD root");
      return;
    }

    File file = root.openNextFile();
    while (file) {
      html += "<li><a href='/" + String(file.name()) + "'>" + String(file.name()) + "</a></li>";
      file = root.openNextFile();
    }
    html += "</ul>";
    request->send(200, "text/html", html);
  });

    // Waypoint GET
  server.on("/Waypoint", HTTP_GET, [](AsyncWebServerRequest *request) {
    File f = SD.open("/config.txt");
    if (!f) {
      request->send(500, "text/plain", "Failed to open Waypoint file");
      return;
    }

    String WayLat = "0", WayLng = "0";
    while (f.available()) {
      String line = f.readStringUntil('\n');
      if (line.startsWith("Latitude=")) WayLat = line.substring(9);
      if (line.startsWith("Longitude=")) WayLng = line.substring(10);
    }
    f.close();

    String html = R"rawliteral(
      <!DOCTYPE html>
      <html>
      <head>
        <meta name='viewport' content='width=device-width, initial-scale=1'>
        <style>
          body { 
            font-family: sans-serif; 
            padding: 1em; 
          }
          input, select { 
            width: 100%; 
            padding: 0.5em; 
            margin: 0.5em 0; 
            font-size: 1em; 
          }

          .button {
            display: inline-block;
            width: 100%;
            padding: 0.5em;
            margin: 1em 0 0 0;
            font-size: 1em;
            background: #007bff;
            color: white;
            border: none;
            border-radius: 5px;
            text-align: center;
            text-decoration: none;
          }
          h1 {
            margin-bottom: 0.5em;
          }
        </style>
      </head>
      <body>
        <h2>Waypoint</h2>
        <form method='POST' action='/Waypoint'>
    )rawliteral";
        html += "Latitude: <input name='WayLat' value='" + WayLat + "'><br>";
        html += "Longitude: <input name='WayLng' value='" + WayLng + "'><br>";
        html += "<input type='submit' class='button' value='Save'>";
        html += "</form>";
        html += "<a class='button' href='/'>Main Menu</a>";
        html += "</body></html>";
        
    request->send(200, "text/html", html);
  });

// Config: POST
  server.on("/Waypoint", HTTP_POST, [](AsyncWebServerRequest *request){
    String WayLat  = request->getParam("WayLat", true)->value();
    String WayLng  = request->getParam("WayLng", true)->value();

    // Serial.println("Writing new waypoint:");
    // Serial.println("LAT: " + WayLat);
    // Serial.println("LNG: " + WayLng);

    replaceConfigLine("/config.txt", "Latitude", WayLat.c_str());
    replaceConfigLine("/config.txt", "Longitude", WayLng.c_str());
    // SD.remove("/config.txt");
    // File f = SD.open("/config.txt", FILE_WRITE);

    // if (!f) {
    //   request->send(500, "text/plain", "Failed to save waypoint");
    //   return;
    // }
    // f.printf("Latitude=%s\n", WayLat.c_str());
    // f.printf("Longitude=%s\n", WayLng.c_str());
    // f.close();



    request->redirect("/Waypoint");
  });

  // Settings GET
  server.on("/Settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    File f = SD.open("/config.txt");
    if (!f) {
      request->send(500, "text/plain", "Failed to open Settings file");
      return;
    }

    String ssid = "", pass = "", tz = "0", log = "0", live = "0", WayLat = "0", WayLng = "0";
    while (f.available()) {
      String line = f.readStringUntil('\n');
      if (line.startsWith("ssid=")) ssid = line.substring(5);
      if (line.startsWith("password=")) pass = line.substring(9);
      if (line.startsWith("timezone=")) tz = line.substring(9);
      if (line.startsWith("log_interval=")) log = line.substring(13);
      if (line.startsWith("live_interval=")) live = line.substring(14);
      if (line.startsWith("Latitude=")) WayLat = line.substring(9);
      if (line.startsWith("Longitude=")) WayLng = line.substring(10);
    }
    f.close();

    String html = R"rawliteral(
             <!DOCTYPE html>
      <html>
      <head>
        <meta name='viewport' content='width=device-width, initial-scale=1'>
        <style>
          body { 
            font-family: sans-serif; 
            padding: 1em; 
          }
          input, select { 
            width: 100%; 
            padding: 0.5em; 
            margin: 0.5em 0; 
            font-size: 1em; 
          }

          .button {
            display: inline-block;
            width: 100%;
            padding: 0.5em;
            margin: 1em 0 0 0;
            font-size: 1em;
            background: #007bff;
            color: white;
            border: none;
            border-radius: 5px;
            text-align: center;
            text-decoration: none;
          }
          h1 {
            margin-bottom: 0.5em;
          }
        </style>
      </head>
      <body>
        <h2>Settings</h2>
        <form method='POST' action='/Settings'>
      )rawliteral";

        html += "SSID: <input name='ssid' value='" + ssid + "'><br>";
        html += "Password: <input name='password' value='" + pass + "'><br>";
        html += "Timezone Offset: <input name='tz' value='" + tz + "'><br>";
        html += "Log Interval (seconds): <input name='log' value='" + log + "'><br>";
        html += "Live Update (seconds): <input name='live' value='" + live + "'><br>";
        html += "<h4>Waypoint</h4>";
        html += "Latitude: <input name='WayLat' value='" + WayLat + "'><br>";
        html += "Longitude: <input name='WayLng' value='" + WayLng + "'><br>";
        html += "<input type='submit' class='button' value='Save'>";
        html += "</form>";
        html += "<a class='button' href='/'>Main Menu</a>";
        html += "</body></html>";
        
    request->send(200, "text/html", html);
  });

// Config: POST
  server.on("/Settings", HTTP_POST, [](AsyncWebServerRequest *request){
    String ssid    = request->getParam("ssid", true)->value();
    String pass    = request->getParam("password", true)->value();
    String tz      = request->getParam("tz", true)->value();
    String log     = request->getParam("log", true)->value();
    String live    = request->getParam("live", true)->value();
    String WayLat  = request->getParam("WayLat", true)->value();
    String WayLng  = request->getParam("WayLng", true)->value();

    // Serial.println("Writing new config:");
    // Serial.println("SSID: " + ssid);
    // Serial.println("PASS: " + pass);
    // Serial.println("TZ: " + tz);
    // Serial.println("LOG: " + log);
    // Serial.println("LIVE: " + live);
    // Serial.println("LAT: " + WayLat);
    // Serial.println("LNG: " + WayLng);

    SD.remove("/config.txt");
    File f = SD.open("/config.txt", FILE_WRITE);
    if (!f) {
      request->send(500, "text/plain", "Failed to save settings");
      return;
    }

    f.printf("ssid=%s\n", ssid.c_str());
    f.printf("password=%s\n", pass.c_str());
    f.printf("timezone=%s\n", tz.c_str());
    f.printf("log_interval=%s\n", log.c_str());
    f.printf("live_interval=%s\n", live.c_str());
    f.printf("Latitude=%s\n", WayLat.c_str());
    f.printf("Longitude=%s\n", WayLng.c_str());
    f.close();

    request->redirect("/settings");
  });

  // Serve all static files from SD
  server.serveStatic("/", SD, "/");

  // 404 handler
  server.onNotFound([](AsyncWebServerRequest *request){
    request->send(404, "text/plain", "Not found");
  });

  server.begin();

  display.print("\nSSID:");
  display.println(wifiSSID);
  display.print("Password:");
  display.println(wifiPass);
  display.print("Addr: ");
  display.println(IP);
  display.display();
}

void stopWiFiServer() {
  if (!wifiStarted) return;
  WiFi.softAPdisconnect(true);
  server.end();
  wifiStarted = false;
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
      // Serial.println(sleepEnabled ? "Entering Deep Sleep" : "Waking up");
      if (sleepEnabled) {
        displayText("Sleep Mode\nEntering Sleep...\nPress Button to Wake up",1,1);
        gpsSerial.end();
        stopWiFiServer();
        delay(3000);
        display.ssd1306_command(SSD1306_DISPLAYOFF);
        esp_deep_sleep_start();
      }
    } else {
      // Short press → cycle mode
      currentMode = (Mode)((currentMode + 1) % 5);
      switch (currentMode) {
        case INFO_MODE:
          stopWiFiServer();
					battery_update();
          loadConfig();
          displayInfo();
          // Serial.println("Switch to INFO");
          break;

        case LIVE_MODE:       
          stopWiFiServer();
					battery_update();
          // Serial.println("Switch to LIVE");
          first_load = true;
          break;

        case LOG_MODE:
          stopWiFiServer();
					battery_update();
          // Serial.println("Switch to LOG");
          first_load = true;
          break;

        case NAV_MODE:       
          stopWiFiServer();
					battery_update();
          // Serial.println("Switch to NAV");
          first_load = true;
          break;
        
        case WIFI_MODE:
					battery_update();
          displayText("WIFI MODE",1,1);
					battery_display();
          // gpsSerial.end();
          startWiFiServer();
          displayText("\nWIFI Enabled",1,0);
          // Serial.println("Switch to WIFI");
          break;
      }
      update_display = true;
    }
  }
}

bool coord_updated( double lat, double lng) {

  handleButton();
  
  char buffer[22];

  sprintf(buffer, "%9.5f, %9.5f", lat, lng);

  String newCoord = buffer;
  
  if (newCoord == lastDisplay) return true;

  lastDisplay = buffer;

  return false;
}

bool gpsWaitFix(uint16_t waitmS) {
  //waits a specified number of millis for a fix, returns true for updated fix
  uint32_t startmS;
  uint8_t GPSchar;

  Serial.print(F("Wait GPS Fix "));
  Serial.print(waitmS);
  Serial.println(F("mS"));

  startmS = millis();

  while ( (uint32_t) (millis() - startmS) < waitmS)   //allows for millis() overflow
  {
  handleButton();
  if (gpsSerial.available() > 0)
   {
    GPSchar = gpsSerial.read();
    gps.encode(GPSchar);
    Serial.write(GPSchar);
   }

   if (gps.speed.isUpdated() && gps.satellites.isUpdated()) //ensures that GGA and RMC sentences have been received
   {
    endFixmS = millis();   //record the time when we got a GPS fix
    Serial.println();
    Serial.print(F("Fix time "));
    FixTimemS = endFixmS - startmS;
    Serial.print(FixTimemS);
    Serial.println(F("ms"));
    return true;
   }
  }
  return false;
}

// === Setup ===
void setup() {
  // Serial.begin(115200);
  // delay(1000);

  // Wire.begin(SDA, SCL);
  //Increment boot number and print it every reboot
  ++bootCount;
  // Serial.println("Boot number: " + String(bootCount));

  if (bootCount == 1) display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  else display.ssd1306_command(SSD1306_DISPLAYON);
  

  esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0);  //1 = High, 0 = Low

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.setTextColor(SSD1306_WHITE);
  
  while (!SD.begin(SD_CS)) displayText("Error\nSD Error\nCheck if installed and Reset",1,1);

  loadConfig();

  currentMode = INFO_MODE;

	battery_update();

  displayInfo();

}


// === Main Loop ===
void loop() {
  handleButton();

  if (currentMode == WIFI_MODE || currentMode == INFO_MODE) return;

  startGetFixmS = millis();//record time reading GPS for fix starts

  if (!gpsWaitFix(5000)) {
    handleButton();
    if (update_display) { 
      update_display = false;
      switch (currentMode) {
        case LIVE_MODE:    
          displayGPSData("Live Mode - Last", lastTimestamp, lastLat, lastLng);
          return;
        case LOG_MODE:
          displayGPSData("Log Mode - Last", lastTimestamp, lastLat, lastLng);
          return;
        case NAV_MODE:
          displayNAVData("NAV Mode - Last", lastTimestamp, lastLat, lastLng, WaypointLat, WaypointLng);
          return;
        default:
          return;
      }
    } else return;
  }

  // while (gpsSerial.available()) gps.encode(gpsSerial.read());

  // if (currentMode == INFO_MODE) return;

  // if (!gps.location.isValid() || !gps.date.isValid() || !gps.time.isValid()) {
  //   if (update_display) { 
  //     update_display = false;
  //     switch (currentMode) {
  //       case LIVE_MODE:    
  //         displayGPSData("Live Mode - Last", lastTimestamp, lastLat, lastLng);
  //         return;
  //       case LOG_MODE:
  //         displayGPSData("Log Mode - Last", lastTimestamp, lastLat, lastLng);
  //         return;
  //       case NAV_MODE:
  //         displayNAVData("NAV Mode - Last", lastTimestamp, lastLat, lastLng, WaypointLat, WaypointLng);
  //         return;
  //       default:
  //         return;
  //     }
  //   } else return;
  // }
  
  update_display = true;

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

	if ((millis() - lastbatTime >= 60000) || first_load) {
		battery_update();
		lastbatTime = millis();
	}

  switch (currentMode) {
    case LIVE_MODE:
      if ((millis() - lastLiveTime >= LIVE_INTERVAL) || first_load) {
        displayGPSData("Live Mode " + String(LIVE_INTERVAL / 1000) + " s ", isoTimeLocal, lat, lng);
        // logData(isoTimeLocal, isoTimeUTC, 1);
        lastTimestamp = isoTimeLocal;
        lastLat = lat;
        lastLng = lng;
        lastLiveTime = millis();
        first_load = false;
      }
      break;

    case LOG_MODE:
      if ((millis() - lastLogTime >= LOG_INTERVAL) || first_load) {
        // Serial.println("Logging");
        displayGPSData("Log Mode " + String(LOG_INTERVAL / 1000) + " s ", isoTimeLocal, lat, lng);
        logData(isoTimeLocal, isoTimeUTC, 1);
        lastTimestamp = isoTimeLocal;
        lastLat = lat;
        lastLng = lng;
        lastLogTime = millis();
        first_load = false;
      }
      break;

    case NAV_MODE: 
       if ((millis() - lastLiveTime >= LIVE_INTERVAL) || first_load) {
        displayNAVData("NAV Mode", lastTimestamp, lat, lng, WaypointLat, WaypointLng);
        lastTimestamp = isoTimeLocal;
        lastLat = lat;
        lastLng = lng;
        lastLiveTime = millis();
        first_load = false;
      }
      break;
  }
}