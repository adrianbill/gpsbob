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
#include <Adafruit_SH110X.h>

// === PINS ===
// SDA D4 For reference, definition not needed
// SCL D5 For reference, definition not needed
#define SD_CS D3 /* SD Card Chip Select */
#define GPS_RX D7
#define GPS_TX D6
#define BUTTON_PIN GPIO_NUM_2
#define BATTERY_PIN 36 /* ADC, GPIO36 (VP) is commonly used for battery voltage sensing */

// === DISPLAY ===
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SCREEN_ADDRESS 0x3C
// Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// === Logging SD Card ===
fs::File csv_file;
fs::File gpx_file;
bool gpx_header_written = false;
bool csv_header_written = false;
int timezone_offset_hours = 0; /* Default UTC */
int log_interval = 30000;    /* default 30 seconds */
int live_interval = 5000;    /* default 5 seconds */

// === GPS ===
HardwareSerial gpsSerial(0);
TinyGPSPlus gps;

// ====== GPS INFO =====
double waypoint_lat = 0.0;
double waypoint_lng = 0.0;
int fix_state = 0;
int fix_start = 0;
int fix_Time = 0;

// ====== LAST GPS INFO =====
String current_date_str = "";
String last_timestamp = "Waiting for GPS";
String today = "";
String last_utc = "";
long last_display = 0;
double last_lat = 0.0;
double last_lng = 0.0;
unsigned long last_log_time = 0;
unsigned long last_live_time = 0;
unsigned long last_bat_time = 0;
int last_fix_time = 0;
int last_sats = 0;
double last_hdop = 0.0;

// === Wi-Fi ===
AsyncWebServer server(80);
String wifi_ssid = "GPS_BOB"; /* Default SSID */
String wifi_pass = "12345678"; /* Default password */
bool wifi_started = false;

// === Sleep & Modes ===

RTC_DATA_ATTR int boot_count = 0;
enum Mode {
    INFO_MODE,
    LIVE_MODE,
    LOG_MODE,
    NAV_MODE_A,
    NAV_MODE_B,
    WIFI_MODE
};
uint32_t button_press_time = 0;
const uint16_t long_press_ms = 3000; /* Long press length, 3 seconds */
const uint16_t debounce_ms = 50;
bool sleep_enabled = false;
bool button_was_pressed = false;
Mode current_mode = INFO_MODE; /* Default Start_Mode */
Mode last_mode = WIFI_MODE;
bool update_display = true;
bool first_load = true;
int bat_ind = 0;

// ___ FUNCTION DECLARATIONS ________________________________________________________________

// === Battery Utilities ===
float battery_voltage(void);
int battery_percentage(float v);
void battery_update(void);
void battery_display(void);

// === Config Management ===
void load_config(void); 
bool replace_config_line(const char *filename, const String &key, const String &newValue);

// === Date & Time conversion ===
String to_iso8601(TinyGPSDate date, TinyGPSTime time); 
String to_iso8601_local(TinyGPSDate date, TinyGPSTime time, int offsetHours); 
String gps_date_stamp(TinyGPSDate date); 

// === display tool (maybe redo); ===
void display_text(const String &text, int size, bool clear = false, bool excute = false);
void display_gps_data(const String &title);
void display_nav_data(const String &title);
void display_info(void);

// === Log File Handling===
const char* mode_to_string(Mode mode);
void open_log_files(const String &dateStr, const String &mode_name); 
void log_data(void); 
void close_gpx(void); 

// === Webserver===
void start_wifi_server(void); 
void stop_wifi_server(void);

// === Button Handling ===
void handle_button(void); 

// === GPS Utilities===
void update_gps_data(void);
void gps_fix_test(void); /* for testing only, not used in final product */
int gps_fix_check(void);

// ___ SETUP & LOOP ________________________________________________________________

// === Setup ===
void setup(void)
{
	++boot_count;

	esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0); /* 1 = High, 0 = Low */
	pinMode(BUTTON_PIN, INPUT_PULLUP);
	gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

	// display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    display.begin(SCREEN_ADDRESS, true);
	display.setTextColor(WHITE);
	
  while (!SD.begin(SD_CS))
		display_text("Error\nSD Error\nCheck if installed and Reset", 1, true, true);
	
  load_config();
	current_mode = INFO_MODE;
	battery_update();
	display_info();
}

// === Main Loop ===
void loop(void)
{
	handle_button();

	if (current_mode == WIFI_MODE || current_mode == INFO_MODE)
		return;

    int gps_check = gps_fix_check();


	if (gps_check == 0) {
		fix_start = millis();
		switch (current_mode) {
		case LIVE_MODE:
			display_gps_data("Live Mode - Last");
			break;
		case LOG_MODE:
			display_gps_data("Log Mode - Last");
			break;
		case NAV_MODE_A:
			display_nav_data("NAV Mode - Last");
			break;
        case NAV_MODE_B:
			display_nav_data("NAV Mode - Last");
			break;
		}
        update_display = false;
        first_load = true;
        return;
	} else if (gps_check == 3) return;

	fix_Time = millis() - fix_start;

	// update battery if needed
	if ((millis() - last_bat_time >= 300000) || first_load) {
		battery_update();
		last_bat_time = millis();
	}

	switch (current_mode) {
	case LIVE_MODE:
		if ((millis() - last_live_time >= live_interval) || first_load) {
            update_display = true;
			update_gps_data();
			display_gps_data("LIVE Freq:" + String(live_interval / 1000) + " s ");
			last_live_time = millis();
			first_load = false;
		}
		break;

	case LOG_MODE:
		if ((millis() - last_log_time >= log_interval) || first_load) {
            update_display = true;
			update_gps_data();
			display_gps_data("LOG Freq: " + String(log_interval / 1000) + " s ");
			log_data();
			last_log_time = millis();
			first_load = false;
		}
		break;

	case NAV_MODE_A:
		if ((millis() - last_live_time >= 1000) || first_load) {
            update_display = true;
			update_gps_data();
			display_nav_data("NAV A");
			last_live_time = millis();
			// first_load = false;
		}
		if (millis() - last_log_time >= log_interval || first_load) {
            log_data();
			last_log_time = millis();   
		}
        first_load = false;
		break;
        
    case NAV_MODE_B:
		if ((millis() - last_live_time >= 1000) || first_load) {
            update_display = true;
			update_gps_data();
			display_nav_data("NAV B");
			last_live_time = millis();
			// first_load = false;
		}
		if (millis() - last_log_time >= log_interval || first_load) {
            log_data();
			last_log_time = millis();   
		}
        first_load = false;
		break;
	}
}

// ___ FUNCTIONS ________________________________________________________________

// === Battery Utilities ===
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

// === Config Management ===
void load_config(void) 
{
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
			timezone_offset_hours = val.toInt();
			// Serial.print("Loaded timezone offset: ");
			// Serial.println(timezone_offset_hours);
		}
		else if (line.startsWith("ssid=")) {
			wifi_ssid = line.substring(5);
			wifi_ssid.trim();
			// Serial.print("Loaded SSID: ");
			// Serial.println(wifi_ssid);
		}
		else if (line.startsWith("password=")) {
			wifi_pass = line.substring(9);
			wifi_pass.trim();
			if (wifi_pass.length() < 8) {
				// Serial.println("Password too short, using default");
				wifi_pass = "12345678";
			} else {
				// Serial.print("Loaded password: ");
				// Serial.println(wifi_pass);
			}
		} 
		else if (line.startsWith("log_interval=")) {
			String val = line.substring(13);
			int interval = val.toFloat() * 1000;
			if (interval >= 1000) log_interval = interval;
			// Serial.print("Loaded log interval: ");
			// Serial.print(log_interval / 1000);
			// Serial.println(" seconds");
		}
		else if (line.startsWith("live_interval=")) {
			String val = line.substring(14);
			int interval = val.toFloat() * 1000;
			if (interval >= 1000) live_interval = interval;
			// Serial.print("Loaded live interval: ");
			// Serial.print(live_interval / 1000);
			// Serial.println(" seconds");
		}
		else if (line.startsWith("Latitude=")) {
			String val = line.substring(9);
			double wayLat = val.toDouble() ;
			if (wayLat != 0) waypoint_lat = wayLat;
			// Serial.print("Loaded Waypoint Latitude: ");
			// Serial.println(waypoint_lat,6);
		}
		else if (line.startsWith("Longitude=")) {
			String val = line.substring(10);
			double wayLng = val.toDouble() ;
			if (wayLng != 0) waypoint_lng = wayLng;
			// Serial.print("Loaded Waypoint Longitude: ");
			// Serial.println(waypoint_lng,6);
		}
	}
	config.close();
}

bool replace_config_line(const char* filename, const String& key, const String& newValue) 
{
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
String to_iso8601(TinyGPSDate date, TinyGPSTime time) 
{
	char buf[25];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
			 date.year(), date.month(), date.day(),
			 time.hour(), time.minute(), time.second());
	return String(buf);
}

String to_iso8601_local(TinyGPSDate date, TinyGPSTime time, int offsetHours) 
{
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

String gps_date_stamp(TinyGPSDate date) 
{
	char buf[9];
	snprintf(buf, sizeof(buf), "%04d%02d%02d", date.year(), date.month(), date.day());
	return String(buf);
}

// === display tool (maybe redo) ===
void display_text(const String &text, int size, bool clear, bool excute) 
{
	if (clear) 
	{
		display.clearDisplay();
		display.setCursor(0, 0);
	}
	
	display.setTextSize(size);
	display.println(text);
	if (excute) display.display();
}



void display_gps_data(const String &title)
{
    if (update_display == false)
        return;
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(WHITE);
    battery_display();
    display.println(title);
    display.println(last_timestamp);
    display.println("Lat");
    display.setTextSize(2);
    display.setCursor(2, display.getCursorY());
    if (last_lat >= 0 && last_lat < 100) display.print("  ");
    if (last_lat < 0 && last_lat > -100) display.print(" ");
    display.println(last_lat, 5);
    display.setTextSize(1);
    display.println("Lon");
    display.setTextSize(2);
    display.setCursor(2, display.getCursorY());
    if (last_lng >= 0 && last_lng < 100) display.print("  ");
    if (last_lng < 0 && last_lng > -100) display.print(" ");
    display.println(last_lng, 5);
    display.display();
    update_display = false;
}

void display_nav_data(const String &title)
{
    if (update_display == false)
        return;
    double distance = TinyGPSPlus::distanceBetween(last_lat, last_lng, waypoint_lat, waypoint_lng);
    double course_to_waypoint = TinyGPSPlus::courseTo(last_lat, last_lng, waypoint_lat, waypoint_lng);
    const char *cardinal = TinyGPSPlus::cardinal(course_to_waypoint);
    
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(WHITE);
    battery_display();
    display.println(title);
    display.println(last_timestamp);
    char buffer[16];
    if (distance < 1000)
        sprintf(buffer, "%5.f m", distance);
    else if (distance < 10000000)
        sprintf(buffer, " %6.1f km", distance / 1000.0);
    else {
        sprintf(buffer, ">10,000 km");
        display.printf("%8.4f, %8.4f", waypoint_lat, waypoint_lng);
        display.println("");
        display.setCursor((display.width() - (10 * 12)) / 2, display.getCursorY() + 4);
        display.setTextSize(2);
        display.println(buffer);
        display.setCursor((display.width() - (7 * 12)) / 2, display.getCursorY() + 4);
        display.print("Nav OFF");
        display.display();
        return;
    }
    display.printf("%8.4f, %8.4f", waypoint_lat, waypoint_lng);
    display.println("");
    display.setCursor(0, display.getCursorY() + 4);
    display.setTextSize(2);
    display.print(buffer);
    display.setTextSize(1);
    display.println("");
    int x = display.getCursorX();
    int y = display.getCursorY();
    display.setCursor(x, y + 4);
    sprintf(buffer, " %5.0f", course_to_waypoint);
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

void display_info(void) 
{
	display_text("Info Mode      ", 1, true);
	display.print("Bat: ");
	display.print(battery_voltage(), 2);
	display.println(" V");

	display.print("Timezone offset: ");
	display.println(timezone_offset_hours);

	display.print("Log interval: ");
	display.print(log_interval / 1000);
	display.println(" s");

	display.print("Live interval: ");
	display.print(live_interval / 1000);
	display.println(" s");

	char buffer [20];

	display.println("Waypoint");
	sprintf(buffer, " Lat: %11.6f", waypoint_lat);
	display.println(buffer);
	sprintf(buffer, " Lon: %11.6f", waypoint_lng);
	display.print(buffer);
	battery_display();
	display.display();
}

// === Log File Handling===
const char* mode_to_string(Mode mode)
{
    switch (mode) {
        case INFO_MODE:   return "INFO_MODE";
        case LIVE_MODE:   return "LIVE_MODE";
        case LOG_MODE:    return "LOG_MODE";
        case NAV_MODE_A:  return "NAV_MODE_A";
        case NAV_MODE_B:  return "NAV_MODE_B";
        case WIFI_MODE:   return "WIFI_MODE";
        default:          return "UNKNOWN_MODE";
    }
}

void open_log_files(const String &dateStr, const String &mode_name) 
{
	current_date_str = dateStr;

	String csvName = "/log_" + mode_name + dateStr + ".csv";
	bool newFile_csv = !SD.exists(csvName);
	csv_file = SD.open(csvName, FILE_APPEND);
	csv_header_written = newFile_csv;

	if (csv_file && csv_header_written) {
		csv_file.println("_timestamp(_local),Latitude,Longitude,Satilites,HDOP,OffsetUTC");
		csv_file.flush();
	}

	String gpxName = "/track_" + mode_name + dateStr + ".gpx";
	bool newFile_gpx = !SD.exists(gpxName);
	gpx_file = SD.open(gpxName, FILE_APPEND);
	gpx_header_written = newFile_gpx;

	if (gpx_file && gpx_header_written) {
		gpx_file.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
		gpx_file.println("<gpx version=\"1.1\" creator=\"ESP32 Logger\"");
		gpx_file.println(" xmlns=\"http://www.topografix.com/GPX/1/1\"");
		gpx_file.println(" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"");
		gpx_file.println(" xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1");
		gpx_file.println(" http://www.topografix.com/GPX/1/1/gpx.xsd\">");
		gpx_file.println("<trk><name>GPSBOB Log</name><trkseg>");
		gpx_file.flush();
	}
}

void log_data() 
{
// 	String csvName = "/log_" + mode_name + dateStr + ".csv";
//     String gpxName = "/track_" + mode_name + dateStr + ".gpx";
    
    // if (today != current_date_str) {
	// 	close_gpx();
	// 	open_log_files(today, mode_to_string(current_mode));
	// }

    if ((current_mode != last_mode) || (today != current_date_str)) {
		close_gpx();
		open_log_files(today, mode_to_string(current_mode));
        current_mode = last_mode;   
	}

	String csv =  last_timestamp + "," +
				String(last_lat, 6) + "," +
				String(last_lng, 6) + "," +
				String(last_sats) + "," +
				String(last_hdop, 2) + "," +
				String(timezone_offset_hours);

	if (csv_file) {
		csv_file.println(csv);
		csv_file.flush();
	}

	if (gpx_file) {
		gpx_file.print("<trkpt lat=\"");
		gpx_file.print(last_lat, 6);
		gpx_file.print("\" lon=\"");
		gpx_file.print(last_lng, 6);
		gpx_file.println("\">");
		gpx_file.print("  <time>");
		gpx_file.print(last_utc);
		gpx_file.println("</time>");
		gpx_file.println("</trkpt>");
		gpx_file.flush();
	}

}

void close_gpx(void) 
{
	if (gpx_file) {
		gpx_file.println("</trkseg></trk></gpx>");
		gpx_file.flush();
		gpx_file.close();
	}
}

// === Webserver===
void start_wifi_server(void) 
{
	if (wifi_started) return;
	wifi_started = true;

	WiFi.softAP(wifi_ssid.c_str(), wifi_pass.c_str());
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
	server.on("/waypoint", HTTP_POST, [](AsyncWebServerRequest *request) {
		String WayLat  = request->getParam("WayLat", true)->value();
		String WayLng  = request->getParam("WayLng", true)->value();

		// Serial.println("Writing new waypoint:");
		// Serial.println("LAT: " + WayLat);
		// Serial.println("LNG: " + WayLng);

		replace_config_line("/config.txt", "Latitude", WayLat.c_str());
		replace_config_line("/config.txt", "Longitude", WayLng.c_str());
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
	server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request) {
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
	server.onNotFound([](AsyncWebServerRequest *request) {
		request->send(404, "text/plain", "Not found");
	});

	server.begin();

	display.print("\nSSID:");
	display.println(wifi_ssid);
	display.print("Password:");
	display.println(wifi_pass);
	display.print("Addr: ");
	display.println(IP);
	display.print("\nWIFI Enabled");
	display.display();
}

void stop_wifi_server(void) 
{
	if (!wifi_started) return;
	WiFi.softAPdisconnect(true);
	server.end();
	wifi_started = false;
}

// === Button Handling ===
void handle_button(void) 
{
	static unsigned long lastChange = 0;
	bool pressed = digitalRead(BUTTON_PIN) == LOW;

	if (pressed && !button_was_pressed && millis() - lastChange > debounce_ms) {
		button_press_time = millis();
		button_was_pressed = true;
		lastChange = millis();
	}

	if (!pressed && button_was_pressed) {
		button_was_pressed = false;
		unsigned long pressDuration = millis() - button_press_time;

		if (pressDuration > long_press_ms) {
			// Long press → toggle sleep
			sleep_enabled = !sleep_enabled;
			// Serial.println(sleep_enabled ? "Entering Deep Sleep" : "Waking up");
			if (sleep_enabled) {
				display_text("Sleep Mode\nEntering Sleep...\nPress Button to Wake up", 1, true, true);
				gpsSerial.end();
				stop_wifi_server();
				delay(3000);
				display_text("", 1, true, true);
				esp_deep_sleep_start();
			}
		} else {
			// Short press → cycle mode
            last_mode = current_mode;
			current_mode = (Mode)((current_mode + 1) % 5);
			switch (current_mode) {
			case INFO_MODE:
				stop_wifi_server();
				battery_update();
				load_config();
				display_info();
				// Serial.println("Switch to INFO");
				break;

			case LIVE_MODE:		
				stop_wifi_server();
				battery_update();
				// Serial.println("Switch to LIVE");
				first_load = true;
				break;

			case LOG_MODE:
				stop_wifi_server();
				battery_update();
				// Serial.println("Switch to LOG");
				first_load = true;
				break;

			case NAV_MODE_A:		
				stop_wifi_server();
				battery_update();
				// Serial.println("Switch to NAV A");
				first_load = true;
				break;

            case NAV_MODE_B:		
				stop_wifi_server();
				battery_update();
				// Serial.println("Switch to NAV B");
				first_load = true;
				break;
			
			case WIFI_MODE:
				display_text("WIFI MODE", 1, true);
				battery_update();
				battery_display();
				start_wifi_server();
				// Serial.println("Switch to WIFI");
				break;
			}
			update_display = true;
		}
	}
}

// === GPS Utilities===
void update_gps_data(void)
{
    TinyGPSDate date = gps.date;
    TinyGPSTime time = gps.time;
    today = gps_date_stamp(date);
    last_utc = to_iso8601(date, time);
	last_timestamp = to_iso8601_local(date, time, timezone_offset_hours);
	last_lat = gps.location.lat();
	last_lng = gps.location.lng();
	last_sats = gps.satellites.value();
	last_hdop = gps.hdop.hdop();
}

void gps_fix_test(void) 
{
	if (fix_state == 0) {
		display_text("Waiting for fix_", 1, true, true); 
		fix_state++;
		return;
	} else if (fix_state == 1) {
		uint32_t startmS = millis();
		uint8_t GPSchar;

		uint32_t endfix_mS;
		uint32_t fix_TimeS;

		while (true) {
			if (gpsSerial.available() > 0) gps.encode(gpsSerial.read());

		//ensures that GGA and RMC sentences have been received
			if (gps.speed.isUpdated() && gps.satellites.isUpdated()) {
				endfix_mS = millis();   //record the time when we got a GPS fix
				fix_TimeS = (endfix_mS - startmS);
				display_text("fix_ Aquired", 1, true);
				display.print(fix_TimeS);
				display.println(" ms");

				TinyGPSDate date = gps.date;
				TinyGPSTime time = gps.time;

				String isoTime_local = to_iso8601_local(date, time, timezone_offset_hours);
				
				display.println(isoTime_local);
				display.print("Lat:  ");
				display.println(gps.location.lat(), 5);
				display.print("Lng:  ");
				display.println(gps.location.lng(), 5);
				display.print("Sats: ");
				display.println(gps.satellites.value());
				display.display();
				fix_state++;
				return;
			}
		}
	}
}

int gps_fix_check(void) 
{
	if (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
    if (gps.speed.isUpdated() && gps.satellites.isUpdated()) return 1; //ensures that GGA and RMC sentences have been received
    return 3; // GPS data is available but not updated
  }
	return 0; // No data available
}