/*
 * Linux kernel coding style applied:
 * - Spaces for indentation
 * - Braces on new lines
 * - No trailing whitespace
 * - Function names and variables in lower_case_with_underscores where possible
 * - 80 character line limit where possible
 * - Consistent spacing and alignment
 */

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


// === PINS ===
	// SDA D4 For reference, definition not needed
	// SCL D5 For reference, definition not needed
  #define SD_CS D2 /* SD Card Chip Select */
	#define GPS_RX D7
	#define GPS_TX D6
	#define BUTTON_PIN GPIO_NUM_2
	#define BATTERY_PIN 36 /* ADC, GPIO36 (VP) is commonly used for battery voltage sensing */

// === DISPLAY ===
	#define SCREEN_WIDTH 128
	#define SCREEN_HEIGHT 64
	#define SCREEN_ADDRESS 0x3C
	Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// === Logging SD Card ===
	fs::File csvFile;
	fs::File gpxFile;
	bool gpxHeaderWritten = false;
	bool csvHeaderWritten = false;
	int timezoneOffsetHours = 0; /* Default UTC */
	int LOG_INTERVAL = 30000;    /* default 30 seconds */
	int LIVE_INTERVAL = 5000;    /* default 5 seconds */

// === GPS ===

	HardwareSerial gpsSerial(0);
	TinyGPSPlus gps;

// ====== GPS INFO =====
	double WaypointLat = 0.0;
	double WaypointLng = 0.0;
	int FixState = 0;
	int FixStart = 0;
	int FixTime = 0;

// ====== LAST GPS INFO =====
	String currentDateStr = "";
	String lastTimestamp = "Waiting for GPS";
  String today = "";
  String lastUTC = "";
	long lastDisplay = 0;
	double lastLat = 0.0;
	double lastLng = 0.0;
	unsigned long lastLogTime = 0;
	unsigned long lastLiveTime = 0;
	unsigned long lastbatTime = 0;
	int lastFixTime = 0;
  int lastSats = 0;
	double lastHdop = 0.0;


// === Wi-Fi ===
	AsyncWebServer server(80);
	String wifiSSID = "GPS_BOB"; /* Default SSID */
	String wifiPass = "12345678"; /* Default password */
	bool wifiStarted = false;

// === Sleep & Modes ===
	RTC_DATA_ATTR int bootCount = 0;
	enum Mode {
		INFO_MODE,
		LIVE_MODE,
		LOG_MODE,
		NAV_MODE,
		WIFI_MODE
	};
	uint32_t buttonPressTime = 0;
	const uint16_t LONG_PRESS_MS = 3000; /* Long press length, 3 seconds */
	const uint16_t DEBOUNCE_MS = 50;
	bool sleepEnabled = false;
	bool buttonWasPressed = false;
	Mode currentMode = INFO_MODE; /* Default Start_Mode */
	bool update_display = true;
	bool first_load = true;
	int bat_ind = 0;

// === Battery ===
float battery_voltage(void);
int battery_percentage(float v);
void battery_update(void);
void battery_display(void);

// === Config File ===
void loadConfig(void);
bool replaceConfigLine(const char *filename, const String &key, const String &newValue);

// === Date & Time conversion ===
String toISO8601(TinyGPSDate date, TinyGPSTime time);
String toISO8601Local(TinyGPSDate date, TinyGPSTime time, int offsetHours);
String gpsDateStamp(TinyGPSDate date);

// === display tool (maybe reo) ===
void displayText(const String &text, int size, bool clear = false, bool excute = false);
void displayGPSData(const String &title);
void displayNAVData(const String &title);
void displayInfo(void);

// === GPS Checking ===
void updateGPSData(void);
void gpsFixTimeTest(void); /* for testing only, not used in final product */
int gpsFixCheck(void);

// === Logging ===
void openLogFiles(const String &dateStr);
void logData(void);
void closeGPX(void);
void startWiFiServer(void);
void stopWiFiServer(void);

// === Button Handling ===
void handleButton(void);

// === Setup ===
void setup(void)
{
	++bootCount;

	esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0); /* 1 = High, 0 = Low */
	pinMode(BUTTON_PIN, INPUT_PULLUP);
	gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
	display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
	display.setTextColor(SSD1306_WHITE);
	while (!SD.begin(SD_CS))
		displayText("Error\nSD Error\nCheck if installed and Reset", 1, true, true);
	loadConfig();
	currentMode = INFO_MODE;
	battery_update();
	displayInfo();
}

// === Main Loop ===
void loop(void)
{

	handleButton();

	if (currentMode == WIFI_MODE || currentMode == INFO_MODE)
		return;

	if (gpsFixCheck() == 0) {
		FixStart = millis();
		switch (currentMode) {
		case LIVE_MODE:
			displayGPSData("Live Mode - Last");
			break;
		case LOG_MODE:
			displayGPSData("Log Mode - Last");
			break;
		case NAV_MODE:
			displayNAVData("NAV Mode - Last");
			break;
		}
    update_display = false;
    return;
	} else if (gpsFixCheck() == 3) return;

	FixTime = millis() - FixStart;

	// update battery if needed
	if ((millis() - lastbatTime >= 300000) || first_load) {
		battery_update();
		lastbatTime = millis();
	}

	switch (currentMode) {
	case LIVE_MODE:
		if ((millis() - lastLiveTime >= LIVE_INTERVAL) || first_load) {
      update_display = true;
			updateGPSData();
			displayGPSData("Live Mode " + String(LIVE_INTERVAL / 1000) + " s ");
			lastLiveTime = millis();
			first_load = false;
		}
		break;

	case LOG_MODE:
		if ((millis() - lastLogTime >= LOG_INTERVAL) || first_load) {
      update_display = true;
			updateGPSData();
			displayGPSData("Log Mode " + String(LOG_INTERVAL / 1000) + " s ");
			logData();
			lastLogTime = millis();
			first_load = false;
		}
		break;

	case NAV_MODE:
		if ((millis() - lastLiveTime >= 1000) || first_load) {
      update_display = true;
			updateGPSData();
			displayNAVData("NAV Mode");
			lastLiveTime = millis();
			first_load = false;
		}
		break;
	}
}

// ======================= FUNCTIONS ==============
// === Utilities ===
float battery_voltage(void)
{
	uint32_t Vbatt = 0;
	int i;
	for (i = 0; i < 16; i++)
		Vbatt = Vbatt + analogReadMilliVolts(BATTERY_PIN); // ADC with correction
	return 2 * Vbatt / 16 / 1000.0;     // attenuation ratio 1/2, mV --> V
}

int battery_percentage(float v)
{
	if (v >= 4.2)
		return 100;
	else if (v > 4.15)
		return 95;
	else if (v > 4.11)
		return 90;
	else if (v > 4.08)
		return 85;
	else if (v > 4.02)
		return 80;
	else if (v > 3.98)
		return 75;
	else if (v > 3.95)
		return 70;
	else if (v > 3.91)
		return 65;
	else if (v > 3.87)
		return 60;
	else if (v > 3.85)
		return 55;
	else if (v > 3.84)
		return 50;
	else if (v > 3.82)
		return 45;
	else if (v > 3.80)
		return 40;
	else if (v > 3.79)
		return 35;
	else if (v > 3.77)
		return 30;
	else if (v > 3.75)
		return 25;
	else if (v > 3.73)
		return 20;
	else if (v > 3.71)
		return 15;
	else if (v > 3.69)
		return 10;
	else if (v > 3.61)
		return 5;
	else if (v > 3.00)
		return 0;
	return -1;
}

void battery_update(void)
{
	bat_ind = battery_percentage(battery_voltage());
}

void battery_display(void)
{
	int cell_width = 18;
	int cell_height = 7;
	int cell_xstart = display.width() - cell_width;
	int fill_gap = 2;
	if (bat_ind == 100)
		fill_gap = 0;
	int max_fill_width = cell_width - (2 * fill_gap);
	int fill_width = (max_fill_width * bat_ind) / 100;
	int fill_start = (cell_xstart + fill_gap) + (max_fill_width - fill_width);

	display.fillRect(cell_xstart - 2, 2, 2, 3, WHITE);
	display.drawRect(cell_xstart, 0, cell_width, cell_height, WHITE);
	display.fillRect(fill_start, fill_gap, fill_width, cell_height - (2 * fill_gap), WHITE);
	if (bat_ind == -1)
		display.drawLine(cell_xstart, 0, cell_xstart + cell_width, cell_height - 1, WHITE);
}

void loadConfig(void) {
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
			int interval = val.toFloat() * 1000;
			if (interval >= 1000) LOG_INTERVAL = interval;
			// Serial.print("Loaded log interval: ");
			// Serial.print(LOG_INTERVAL / 1000);
			// Serial.println(" seconds");
		}
		else if (line.startsWith("live_interval=")) {
			String val = line.substring(14);
			int interval = val.toFloat() * 1000;
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

void displayText(const String &text, int size, bool clear, bool excute) {
	if (clear) 
	{
		display.clearDisplay();
		display.setCursor(0, 0);
	}
	
	display.setTextSize(size);
	display.println(text);
	if (excute) display.display();
}

void updateGPSData(void)
{
  TinyGPSDate date = gps.date;
	TinyGPSTime time = gps.time;
  today = gpsDateStamp(date);
  lastUTC = toISO8601(date, time);
	lastTimestamp = toISO8601Local(date, time, timezoneOffsetHours);
	lastLat = gps.location.lat();
	lastLng = gps.location.lng();
	lastSats = gps.satellites.value();
	lastHdop = gps.hdop.hdop();
}

void displayGPSData(const String &title)
{
    if (update_display == false)
        return;
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    battery_display();
    display.println(title);
    display.println(lastTimestamp);
    display.println("Lat");
    display.setTextSize(2);
    display.setCursor(2, display.getCursorY());
    if (lastLat >= 0 && lastLat < 100) display.print("  ");
    if (lastLat < 0 && lastLat > -100) display.print(" ");
    display.println(lastLat, 5);
    display.setTextSize(1);
    display.println("Lon");
    display.setTextSize(2);
    display.setCursor(2, display.getCursorY());
    if (lastLng >= 0 && lastLng < 100) display.print("  ");
    if (lastLng < 0 && lastLng > -100) display.print(" ");
    display.println(lastLng, 5);
    display.display();
    update_display = false;
}

void displayNAVData(const String &title)
{
    if (update_display == false)
        return;
    double distance = TinyGPSPlus::distanceBetween(lastLat, lastLng, WaypointLat, WaypointLng);
    double course_to_waypoint = TinyGPSPlus::courseTo(lastLat, lastLng, WaypointLat, WaypointLng);
    const char *cardinal = TinyGPSPlus::cardinal(course_to_waypoint);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    battery_display();
    display.println(title);
    display.println(lastTimestamp);
    char buffer[16];
    if (distance < 1000)
        sprintf(buffer, "%5.f m", distance);
    else if (distance < 10000000)
        sprintf(buffer, " %6.1f km", distance / 1000.0);
    else {
        sprintf(buffer, ">10,000 km");
        display.printf("%8.4f, %8.4f", WaypointLat, WaypointLng);
        display.println("");
        display.setCursor((display.width() - (10 * 12)) / 2, display.getCursorY() + 4);
        display.setTextSize(2);
        display.println(buffer);
        display.setCursor((display.width() - (7 * 12)) / 2, display.getCursorY() + 4);
        display.print("Nav OFF");
        display.display();
        return;
    }
    display.printf("%8.4f, %8.4f", WaypointLat, WaypointLng);
    display.println("");
    display.setCursor(0, display.getCursorY() + 4);
    display.setTextSize(2);
    display.print(buffer);
    display.setTextSize(1);
    display.println("");
    int x = display.getCursorX();
    int y = display.getCursorY();
    display.setCursor(x, y + 4);
    sprintf(buffer, " %6.1f", course_to_waypoint);
    display.println("");
    display.setTextSize(2);
    display.print(buffer);
    x = display.getCursorX();
    y = display.getCursorY();
    display.drawCircle(x + 2, y - 1, 3, WHITE);
    display.setTextSize(1);
    x = display.getCursorX();
    y = display.getCursorY();
    display.println("");
    display.setCursor(x, (display.getCursorY() + y) / 2);
    display.print(" (");
    display.print(cardinal);
    display.print(")");
    display.display();
    update_display = false;
}

void displayInfo() {
	displayText("Info Mode      ", 1, true);
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

void logData() {
	if (today != currentDateStr) {
		closeGPX();
		openLogFiles(today);
	}

	String csv =  lastTimestamp + "," +
				String(lastLat, 6) + "," +
				String(lastLng, 6) + "," +
				String(lastSats) + "," +
				String(lastHdop, 2) + "," +
				String(timezoneOffsetHours);

	if (csvFile) {
		csvFile.println(csv);
		csvFile.flush();
	}

	if (gpxFile) {
		gpxFile.print("<trkpt lat=\"");
		gpxFile.print(lastLat, 6);
		gpxFile.print("\" lon=\"");
		gpxFile.print(lastLng, 6);
		gpxFile.println("\">");
		gpxFile.print("  <time>");
		gpxFile.print(lastUTC);
		gpxFile.println("</time>");
		gpxFile.println("</trkpt>");
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
				<a class='button' href='/waypoint'>Waypoint</a>
				<a class='button' href='/settings'>Settings</a>
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
	server.on("/waypoint", HTTP_GET, [](AsyncWebServerRequest *request) {
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
				<form method='POST' action='/waypoint'>
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
	server.on("/waypoint", HTTP_POST, [](AsyncWebServerRequest *request){
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



		request->redirect("/waypoint");
	});

	// Settings GET
	server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
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
				<form method='POST' action='/settings'>
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
	server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request){
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
	display.print("\nWIFI Enabled");
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
				displayText("Sleep Mode\nEntering Sleep...\nPress Button to Wake up", 1, true, true);
				gpsSerial.end();
				stopWiFiServer();
				delay(3000);
				displayText("", 1, true, true);
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
				displayText("WIFI MODE", 1, true);
				battery_update();
				battery_display();
				startWiFiServer();
				// Serial.println("Switch to WIFI");
				break;
			}
			update_display = true;
		}
	}
}

void gpsFixTimeTest() {
	if (FixState == 0) {
		displayText("Waiting for Fix", 1, true, true); 
		FixState++;
		return;
	} else if (FixState == 1) {
		uint32_t startmS = millis();
		uint8_t GPSchar;

		uint32_t endFixmS;
		uint32_t FixTimeS;

		while (true) {
			if (gpsSerial.available() > 0) gps.encode(gpsSerial.read());

		//ensures that GGA and RMC sentences have been received
			if (gps.speed.isUpdated() && gps.satellites.isUpdated()) {
				endFixmS = millis();   //record the time when we got a GPS fix
				FixTimeS = (endFixmS - startmS);
				displayText("Fix Aquired", 1, true);
				display.print(FixTimeS);
				display.println(" ms");

				TinyGPSDate date = gps.date;
				TinyGPSTime time = gps.time;

				String isoTimeLocal = toISO8601Local(date, time, timezoneOffsetHours);
				
				display.println(isoTimeLocal);
				display.print("Lat:  ");
				display.println(gps.location.lat(), 5);
				display.print("Lng:  ");
				display.println(gps.location.lng(), 5);
				display.print("Sats: ");
				display.println(gps.satellites.value());
				display.display();
				FixState++;
				return;
			}
		}
	}
}

int gpsFixCheck() {
	if (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
    if (gps.speed.isUpdated() && gps.satellites.isUpdated()) return 1; //ensures that GGA and RMC sentences have been received
    return 3; // GPS data is available but not updated
  }
	return 0; // No data available
}