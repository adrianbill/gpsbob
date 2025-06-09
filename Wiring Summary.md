# Wiring Summary (ESP32 WROOM)

| Component      | ESP32 Pin     | Notes                   |
| -------------- | ------------- | ----------------------- |
| GY-NEO6MV2 GPS | RX → GPIO17   | SoftwareSerial TX       |
|                | TX → GPIO16   | SoftwareSerial RX       |
| SD Card (SPI)  | CS → GPIO5    | Chip Select             |
|                | MOSI → GPIO23 |                         |
|                | MISO → GPIO19 |                         |
|                | SCK → GPIO18  |                         |
| SSD1306 OLED   | SDA → GPIO21  | I²C                     |
|                | SCL → GPIO22  |                         |
| DS1307 RTC     | SDA → GPIO21  | Shared I²C              |
|                | SCL → GPIO22  |                         |
| Reed Switch 1  | GPIO32        | Sleep trigger (pull-up) |
| Reed Switch 2  | GPIO33        | Logging mode            |
| Reed Switch 3  | GPIO25        | Wi-Fi data retrieval    |

## Notes

- This system logs NMEA sentences ($GPGGA) to CSV.
- GPS fix is checked by ensuring the fix quality field is not 0.
- A QR code generator (e.g. QR Code Monkey) can be used manually to point to <http://192.168.4.1/>.
- The display can be extended to generate and render a QR code if desired using the QRCode library (on request).
