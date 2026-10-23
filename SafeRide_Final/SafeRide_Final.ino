#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>

// ================= PIN DEFINITIONS =================
#define SS_PIN       5
#define RST_PIN      27
#define GREEN_LED    25
#define RED_LED      26
#define BUZZER       14
#define MAX_STUDENTS 10

#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C
#define OLED_RESET   -1

// ================= DEBOUNCE / TIMING =================
#define TAP_COOLDOWN_MS       8000    // ignore same-card re-tap within this window
#define GPS_TIMEOUT_MS        15000   // NEO-6M fix considered stale after this
#define PHONE_TIMEOUT_MS      10000   // Phone GPS considered stale after this
#define OLED_REFRESH_MS       1000    // background OLED redraw interval
#define MISSED_STOP_CHECK_MS  5000    // how often to scan for missed stops (not per-loop)
#define MISSED_STOP_WINDOW_MS 60000   // demo value (1 min) - set to real route duration in production
// MISSED STOP WINDOW TIMER CHANGE
// 5 minutes	300000
// 10 minutes	600000
// 15 minutes	900000
// 30 minutes	1800000
// 45 minutes	2700000
// ================= MODULE OBJECTS =================
MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
HardwareSerial simSerial(1);   // UART1 - SIM800L
HardwareSerial gpsSerial(2);   // UART2 - NEO-6M
TinyGPSPlus gps;

// ================= WIFI AP (Phone GPS link) =================
const char* AP_SSID = "SafeRide_ESP32";
const char* AP_PASSWORD = "safe1234";
WebServer server(80);

double phoneLat = 0.0, phoneLng = 0.0, phoneAccuracy = 0.0;
unsigned long phoneLastUpdate = 0;
bool phoneDataValid = false;

// ================= LOCATION MANAGER =================
enum LocationSource { SOURCE_NEO6M, SOURCE_PHONE, SOURCE_NONE };

struct LocationData {
  double latitude;
  double longitude;
  double accuracy;
  LocationSource source;
};

LocationData getCurrentLocation() {
  LocationData result;

  // Primary: NEO-6M
  if (gps.location.isValid() && gps.location.age() < GPS_TIMEOUT_MS) {
    result.latitude = gps.location.lat();
    result.longitude = gps.location.lng();
    result.accuracy = 0;
    result.source = SOURCE_NEO6M;
    return result;
  }

  // Backup: Phone GPS
  if (phoneDataValid && (millis() - phoneLastUpdate) < PHONE_TIMEOUT_MS) {
    result.latitude = phoneLat;
    result.longitude = phoneLng;
    result.accuracy = phoneAccuracy;
    result.source = SOURCE_PHONE;
    return result;
  }

  // Neither available
  result.latitude = 0;
  result.longitude = 0;
  result.accuracy = 0;
  result.source = SOURCE_NONE;
  return result;
}

String sourceToString(LocationSource src) {
  switch (src) {
    case SOURCE_NEO6M: return "NEO-6M";
    case SOURCE_PHONE:  return "PHONE";
    default:            return "NONE";
  }
}

String getMapsLink(LocationData loc) {
  return "https://maps.google.com/?q=" + String(loc.latitude, 6) + "," + String(loc.longitude, 6);
}

// ================= STUDENT DATABASE =================
struct Student {
  String name;
  String uid;
  String parentPhone;
  bool onBus;
  String boardTime;
  String exitTime;
  unsigned long lastTapMillis;   // also doubles as "boarded at" timestamp for missed-stop timing
  bool missedStopAlerted;        // prevents repeat alerts for the same missed-stop event
};

Student students[MAX_STUDENTS] = {
  {"Sharvesh R",     "33 F1 3A 56", "+919677342516", false, "", "", 0, false},
  {"Siddharth K", "E3 74 02 29", "+918925124280", false, "", "", 0, false},
};
int studentCount = 2;

// ================= Background state (for OLED) =================
String lastScannedUID = "--";
String lastStudentName = "--";
String lastEventStatus = "--";

String getUIDString(MFRC522 &rfid) {
  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidStr += "0";
    uidStr += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uidStr += " ";
  }
  uidStr.toUpperCase();
  return uidStr;
}

int findStudentByUID(String scannedUID) {
  for (int i = 0; i < studentCount; i++) {
    if (students[i].uid == scannedUID) return i;
  }
  return -1;
}

String nowTimeString() {
  // Placeholder time source (no RTC/NTP wired yet) - replace with RTC/NTP read if added later
  unsigned long s = millis() / 1000;
  int hh = (s / 3600) % 24;
  int mm = (s / 60) % 60;
  int ss = s % 60;
  char buf[12];
  sprintf(buf, "%02d:%02d:%02d", hh, mm, ss);
  return String(buf);
}

// ================= LCD — FAST, CRITICAL PATH ONLY =================
// Only what's needed in the moment. Everything else (lat/lng/accuracy/
// source/UID) moved to the OLED background dashboard below.

void showWaitingScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Waiting for");
  lcd.setCursor(0, 1);
  lcd.print("Student...");
}

void showQuick(String line1, String line2, int holdMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  if (holdMs > 0) delay(holdMs);
}

// ================= OLED — BACKGROUND DASHBOARD =================
// Redrawn on its own timer, independent of RFID taps. Carries all the
// "redundant" detail (lat/lng, accuracy, source, last UID) so the LCD
// never has to.

void drawOLEDDashboard() {
  LocationData loc = getCurrentLocation();

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0, 0);
  oled.print("SafeRide  [");
  oled.print(sourceToString(loc.source));
  oled.println("]");

  oled.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  oled.setCursor(0, 14);
  if (loc.source != SOURCE_NONE) {
    oled.print("Lat: "); oled.println(loc.latitude, 6);
    oled.setCursor(0, 24);
    oled.print("Lng: "); oled.println(loc.longitude, 6);
    if (loc.source == SOURCE_PHONE) {
      oled.setCursor(0, 34);
      oled.print("Acc: "); oled.print(loc.accuracy, 1); oled.println(" m");
    }
  } else {
    oled.println("Location: unavailable");
  }

  oled.drawLine(0, 45, 128, 45, SSD1306_WHITE);

  oled.setCursor(0, 49);
  oled.print("Last: "); oled.println(lastStudentName);
  oled.setCursor(0, 58);
  oled.print(lastEventStatus); oled.print("  UID:"); oled.println(lastScannedUID);

  oled.display();
}

// ================= FEEDBACK (LED + BUZZER) =================
void validFeedback() {
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(BUZZER, HIGH);
  delay(200);
  digitalWrite(BUZZER, LOW);
}

void invalidFeedback() {
  digitalWrite(RED_LED, HIGH);
  digitalWrite(BUZZER, HIGH);
  delay(150);
  digitalWrite(BUZZER, LOW);
  delay(150);
  digitalWrite(BUZZER, HIGH);
  delay(150);
  digitalWrite(BUZZER, LOW);
}

// ================= SMS SENDING =================
bool sendSMS(String number, String message) {
  simSerial.println("AT+CMGF=1");
  delay(400);
  flushSimResponse();

  simSerial.println("AT+CMGS=\"" + number + "\"");
  delay(400);
  flushSimResponse();

  simSerial.print(message);
  delay(100);
  simSerial.write(26); // Ctrl+Z
  delay(5000);

  String response = readSimResponse();
  Serial.println(response);

  return response.indexOf("+CMGS") != -1;
}

void flushSimResponse() {
  while (simSerial.available()) simSerial.read();
}

String readSimResponse() {
  String resp = "";
  unsigned long start = millis();
  while (millis() - start < 500) {
    while (simSerial.available()) {
      resp += (char)simSerial.read();
    }
  }
  return resp;
}

// ================= MISSED-STOP CHECK =================
// Runs on its own timer, independent of RFID taps. Scans every student
// currently marked onBus; if they've been on the bus longer than the
// expected window and haven't been alerted yet, fire one alert SMS.
void checkMissedStops() {
  for (int i = 0; i < studentCount; i++) {
    if (students[i].onBus && !students[i].missedStopAlerted) {
      unsigned long elapsed = millis() - students[i].lastTapMillis;
      if (elapsed > MISSED_STOP_WINDOW_MS) {
        LocationData loc = getCurrentLocation();

        String message = "SafeRide ALERT\n\n";
        message += students[i].name + " has not exited the\n";
        message += "school bus at the expected stop.\n\n";
        if (loc.source != SOURCE_NONE) {
          message += "Last known location:\n" + getMapsLink(loc) + "\n\n";
          message += "Source: " + sourceToString(loc.source) + "\n\n";
        }
        message += "Please contact the bus driver immediately.";

        Serial.println("MISSED STOP: " + students[i].name);
        bool sent = sendSMS(students[i].parentPhone, message);
        Serial.println(sent ? "Missed-stop alert sent." : "Missed-stop alert FAILED to send.");

        students[i].missedStopAlerted = true; // don't re-fire for this same event
      }
    }
  }
}

// ================= WEB SERVER (Phone GPS page) =================
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><title>SafeRide Phone GPS</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:linear-gradient(160deg,#0A1931,#1A5276);color:#fff;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:24px;text-align:center}
.badge{background:rgba(255,255,255,.1);border-radius:20px;padding:6px 16px;font-size:12px;letter-spacing:1px;margin-bottom:24px;text-transform:uppercase}
h1{font-size:22px;margin-bottom:4px}.subtitle{font-size:13px;color:#AECDE8;margin-bottom:32px}
.pulse-dot{width:16px;height:16px;background:#2ECC71;border-radius:50%;margin:0 auto 20px;animation:pulse 1.5s infinite}
.pulse-dot.error{background:#E74C3C}
@keyframes pulse{0%{box-shadow:0 0 0 0 rgba(46,204,113,.6)}70%{box-shadow:0 0 0 16px rgba(46,204,113,0)}100%{box-shadow:0 0 0 0 rgba(46,204,113,0)}}
#status{font-size:15px;font-weight:500;margin-bottom:20px;min-height:20px}
.card{background:rgba(255,255,255,.08);border-radius:14px;padding:18px 20px;width:100%;max-width:340px}
.row{display:flex;justify-content:space-between;font-size:13px;padding:8px 0;border-bottom:1px solid rgba(255,255,255,.08)}
.row:last-child{border-bottom:none}.row span:first-child{color:#AECDE8}.row span:last-child{font-family:monospace;font-weight:600}
.maps-link{display:block;margin-top:16px;background:#F5A623;color:#0A1931;text-decoration:none;font-weight:600;font-size:14px;padding:12px;border-radius:10px}
footer{margin-top:28px;font-size:11px;color:#6E8CAE}
</style></head><body>
<div class="badge">SafeRide Backup Link</div>
<h1>Phone GPS Source</h1>
<p class="subtitle">Keep this page open while the bus is moving</p>
<div class="pulse-dot" id="dot"></div>
<p id="status">Requesting location access...</p>
<div class="card">
<div class="row"><span>Latitude</span><span id="lat">--</span></div>
<div class="row"><span>Longitude</span><span id="lng">--</span></div>
<div class="row"><span>Accuracy</span><span id="acc">--</span></div>
<div class="row"><span>Last sent</span><span id="time">--</span></div>
<a href="#" id="mapsLink" class="maps-link" target="_blank">Open in Google Maps</a>
</div>
<footer>ESP32 Local Link - No internet required</footer>
<script>
const dot=document.getElementById('dot'),status=document.getElementById('status'),mapsLink=document.getElementById('mapsLink');
function sendLocation(p){
const lat=p.coords.latitude,lng=p.coords.longitude,acc=p.coords.accuracy;
document.getElementById('lat').innerText=lat.toFixed(6);
document.getElementById('lng').innerText=lng.toFixed(6);
document.getElementById('acc').innerText=acc.toFixed(1)+' m';
mapsLink.href=`https://maps.google.com/?q=${lat},${lng}`;
fetch(`/update?lat=${lat}&lng=${lng}&acc=${acc}`).then(()=>{
dot.classList.remove('error');status.innerText="Live - sending to bus unit";
document.getElementById('time').innerText=new Date().toLocaleTimeString();
}).catch(()=>{dot.classList.add('error');status.innerText="Cannot reach bus unit";});
}
function handleError(e){dot.classList.add('error');status.innerText="GPS Error: "+e.message;}
function startTracking(){navigator.geolocation.getCurrentPosition(sendLocation,handleError,{enableHighAccuracy:true,timeout:10000});}
setInterval(startTracking,3000);startTracking();
</script></body></html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleUpdate() {
  if (server.hasArg("lat") && server.hasArg("lng")) {
    phoneLat = server.arg("lat").toDouble();
    phoneLng = server.arg("lng").toDouble();
    phoneAccuracy = server.hasArg("acc") ? server.arg("acc").toDouble() : 0.0;
    phoneLastUpdate = millis();
    phoneDataValid = true;
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing lat/lng");
  }
}

// ================= SETUP =================
unsigned long lastOledRefresh = 0;

void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER, LOW);

  SPI.begin();
  rfid.PCD_Init();

  Wire.begin(21, 22); // shared I2C bus for LCD + OLED

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SafeRide v1.0");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed - check wiring/address (0x3C)");
  } else {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("SafeRide booting...");
    oled.display();
  }

  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);   // NEO-6M - primary
  simSerial.begin(9600, SERIAL_8N1, 4, 33);    // SIM800L - RX moved off GPIO2 (avoids onboard LED)
  delay(3000); // let SIM800L settle

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("Phone GPS AP ready. Connect to: ");
  Serial.println(AP_SSID);
  Serial.print("Then browse to: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/update", handleUpdate);
  server.begin();

  Serial.println("=== SafeRide Full Integration Ready ===");

  // Show the phone-GPS link on the LCD for a few seconds at boot,
  // so the driver/admin doesn't have to check Serial Monitor.
  IPAddress apIP = WiFi.softAPIP();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connect WiFi:");
  lcd.setCursor(0, 1);
  lcd.print(AP_SSID);
  delay(2500);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Then browse to:");
  lcd.setCursor(0, 1);
  lcd.print(apIP);
  delay(2500);

  showWaitingScreen();
}

// ================= MAIN LOOP =================
unsigned long lastMissedStopCheck = 0;

void loop() {
  server.handleClient();

  // Missed-stop check runs on its own timer - independent of RFID taps
  if (millis() - lastMissedStopCheck >= MISSED_STOP_CHECK_MS) {
    lastMissedStopCheck = millis();
    checkMissedStops();
  }

  // Feed NEO-6M parser continuously - never blocks, never skipped
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Background OLED redraw - independent of RFID taps, non-blocking
  if (millis() - lastOledRefresh >= OLED_REFRESH_MS) {
    lastOledRefresh = millis();
    drawOLEDDashboard();
  }

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String scannedUID = getUIDString(rfid);
  int idx = findStudentByUID(scannedUID);
  lastScannedUID = scannedUID;

  if (idx == -1) {
    Serial.println("Scanned: " + scannedUID + " -> NOT REGISTERED");
    invalidFeedback();
    showQuick("Invalid Card", scannedUID, 1200);
    digitalWrite(RED_LED, LOW);
    lastStudentName = "Unknown";
    lastEventStatus = "INVALID";
  } else {
    // ---- Debounce: ignore repeat taps of the same card within cooldown ----
    if (millis() - students[idx].lastTapMillis < TAP_COOLDOWN_MS) {
      rfid.PICC_HaltA();
      return; // silently ignore - too soon after last tap
    }
    students[idx].lastTapMillis = millis();

    // ---- Toggle boarding/exit status ----
    String status;
    if (!students[idx].onBus) {
      students[idx].onBus = true;
      students[idx].boardTime = nowTimeString();
      students[idx].missedStopAlerted = false; // fresh boarding - re-arm the missed-stop check
      status = "BOARDED";
    } else {
      students[idx].onBus = false;
      students[idx].exitTime = nowTimeString();
      status = "EXITED";
    }

    Serial.println("Scanned: " + scannedUID + " -> " + students[idx].name + " [" + status + "]");
    validFeedback();

    // Fast LCD: name + status only (no lat/lng/source here anymore)
    showQuick(students[idx].name, status, 900);

    LocationData loc = getCurrentLocation();

    String message = "SafeRide Alert\n\n";
    message += "Student: " + students[idx].name + "\n";
    message += "Status: " + status + "\n\n";
    if (loc.source != SOURCE_NONE) {
      message += "Location:\n" + getMapsLink(loc) + "\n\n";
      message += "Source: " + sourceToString(loc.source);
    } else {
      message += "Location: unavailable";
    }

    showQuick("Sending SMS...", "", 0);
    bool sent = sendSMS(students[idx].parentPhone, message);

    if (sent) {
      showQuick("SMS SENT", students[idx].name, 1200);
      Serial.println("SMS confirmed sent to " + students[idx].parentPhone);
    } else {
      showQuick("SMS FAILED", "Check GSM", 1200);
      Serial.println("SMS send FAILED for " + students[idx].parentPhone);
    }

    lastStudentName = students[idx].name;
    lastEventStatus = status;
    digitalWrite(GREEN_LED, LOW);
  }

  rfid.PICC_HaltA();
  showWaitingScreen();
}