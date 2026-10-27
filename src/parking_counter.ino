/*
  Parking space counter + servo barrier (POC / scale model)
  Board  : Arduino UNO R4 WiFi
  Sensors: 2x HC-SR04 side by side along the driving direction (sensor 1 = outside, sensor 2 = inside)
  Output : 16x2 I2C LCD, servo barrier (D9), WiFi + MQTT

  MQTT:
    The board publishes ONE value only: the number of free spaces, e.g. "27".
    It publishes when that number changes (and once after every (re)connect), as a retained
    message, so the dashboard shows the current value as soon as it opens.
    The dashboard works out "In / Out <amount>" from the difference between two values.

  ENTERING (car comes in from outside)
    Trigger : sensor 1 sees a car, sensor 2 does not, and the lot is not full
    Action  : open barrier, passed = false, timer not running
    Wait    : while sensor 1 or sensor 2 still sees a car, or the lane has not been clear long enough
              - sensor 2 sees a car   -> passed = true
              - either sensor blocked -> reset the clear timer
              - both just became clear -> start the clear timer
              - clear for CLEAR_HOLD_MS -> the car is gone, leave the wait
    Finish  : if passed, cars inside + 1;  then close barrier

  EXITING (car goes out from inside) - same, mirrored
    Trigger : sensor 2 sees a car, sensor 1 does not, and at least 1 car is inside
    passed  : set when sensor 1 sees a car;  at the end, if passed, cars inside - 1

  Anything else: do nothing, barrier stays closed.

  Servo starts at 90 deg (barrier closed) and lifts by 90 deg to open.

  Serial commands (9600 baud):
    r = reset to empty, + = add a car, - = remove a car
    o = open barrier,   c = close barrier   (handy for checking the servo direction)
*/
#include <WiFiS3.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// ========================
// Types: must stay above the first function. The Arduino IDE auto-generates function
// prototypes there, and they fail to compile if they mention a type defined later.
// ========================
struct Sensor {
  uint8_t trig;
  uint8_t echo;
  float   distance;   // cm, -1 = no echo
  bool    active;     // debounced: a car is in front of the sensor
  uint8_t hits;
  uint8_t misses;
};

enum State : uint8_t {
  IDLE,        // barrier closed, watching the sensors
  ENTERING,    // barrier open, car coming in
  EXITING      // barrier open, car going out
};

// ===== WiFi =====
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ===== MQTT =====
// mqtt-dashboard.com is HiveMQ's public broker: anyone can read and write any topic,
// so keep this topic name unique. The dashboard page must use the same topic.
const char* MQTT_SERVER = "mqtt-dashboard.com";
const int   MQTT_PORT = 1883;
const char* MQTT_PUBLISH_TOPIC = "kmitl-it/smart-parking/free-spaces";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

constexpr unsigned long NET_RETRY_MS            = 10000;  // time between reconnect attempts
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;

int lastPublishedFree = -1;       // -1 = nothing published yet, so the next check publishes

// ===== Connect WiFi (gives up after the timeout instead of blocking forever) =====
bool connectWiFi(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("\nWiFi failed (will retry)");
  return false;
}

// ===== Connect MQTT (single attempt). Publish only, no subscription. =====
bool connectMQTT() {
  Serial.print("Connecting to MQTT...");

  String clientId = "Arduino-";
  clientId += String((unsigned long)millis());

  if (mqtt.connect(clientId.c_str())) {
    Serial.println(" connected!");
    lastPublishedFree = -1;       // send the current value again after every (re)connect
    return true;
  }

  Serial.print(" failed, MQTT state = ");
  Serial.println(mqtt.state());
  return false;
}

// ========================
// Hardware
// ========================
LiquidCrystal_I2C lcd(0x27, 16, 2);   // SDA = A4, SCL = A5 (try 0x3F if the LCD stays blank)

const uint8_t TRIG_1 = 2;
const uint8_t ECHO_1 = 4;
const uint8_t TRIG_2 = 12;
const uint8_t ECHO_2 = 13;

// Servo barrier: starts at 90 deg (closed) and lifts by 90 deg.
// If it lifts the wrong way, change "- 90" to "+ 90" (opens at 180 deg instead of 0).
const uint8_t SERVO_PIN = 9;
const int BARRIER_CLOSED_ANGLE = 90;
const int BARRIER_OPEN_ANGLE   = BARRIER_CLOSED_ANGLE - 90;

Servo barrier;
bool barrierOpen = false;

void openBarrier() {
  if (!barrierOpen) {
    barrier.write(BARRIER_OPEN_ANGLE);
    barrierOpen = true;
    Serial.println("BARRIER,OPEN");
  }
}

void closeBarrier() {
  if (barrierOpen) {
    barrier.write(BARRIER_CLOSED_ANGLE);
    barrierOpen = false;
    Serial.println("BARRIER,CLOSED");
  }
}

// ========================
// Configuration
// ========================
const int TOTAL_SPACES = 30;      // the dashboard has the same number in its CONFIG

// Distance from the sensor to the (model) car, in cm
const float DETECT_MIN_CM = 5;     // closer than this = ignore
const float DETECT_MAX_CM = 15;    // a car is detected when distance is in [MIN, MAX]
const float RELEASE_CM    = 30;    // once active, stay active until farther than this (hysteresis)

const uint8_t ON_SAMPLES  = 2;     // consecutive hits to become active (debounce)
const uint8_t OFF_SAMPLES = 2;     // consecutive misses to become inactive

const unsigned long SCAN_INTERVAL_MS = 40;
const unsigned long CLEAR_HOLD_MS    = 1500;   // lane must stay clear this long before the car counts as gone
const unsigned long LCD_INTERVAL_MS  = 150;
const unsigned long ECHO_TIMEOUT_US  = 6000;   // ~100 cm max range

// ========================
// Sensors
// ========================
Sensor s1 = {TRIG_1, ECHO_1, -1, false, 0, 0};
Sensor s2 = {TRIG_2, ECHO_2, -1, false, 0, 0};

float readDistanceCm(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, ECHO_TIMEOUT_US);
  if (duration == 0) return -1;                  // no echo
  return duration * 0.0343f / 2.0f;
}

// Measure, debounce, and apply hysteresis
void updateSensor(Sensor &s) {
  s.distance = readDistanceCm(s.trig, s.echo);

  bool inRange;
  if (s.distance < 0)  inRange = false;
  else if (s.active)   inRange = s.distance < RELEASE_CM;
  else                 inRange = (s.distance >= DETECT_MIN_CM && s.distance <= DETECT_MAX_CM);

  if (inRange) { if (s.hits < 255)   s.hits++;   s.misses = 0; }
  else         { if (s.misses < 255) s.misses++; s.hits = 0; }

  if (!s.active && s.hits   >= ON_SAMPLES)  s.active = true;
  if ( s.active && s.misses >= OFF_SAMPLES) s.active = false;
}

// ========================
// Gate logic
// ========================
State state = IDLE;
bool passed = false;              // the car reached the far sensor during this cycle
unsigned long clearSince = 0;     // when both sensors became clear (0 = timer not running)

long entries = 0;
long exits   = 0;
int  carsInside = 0;              // never below 0

int freeSpaces() {
  int f = TOTAL_SPACES - carsInside;
  return f < 0 ? 0 : f;
}

void logEvent(const char *event, unsigned long now) {
  // CSV: millis,event,occupied,free,entries,exits
  Serial.print(now);          Serial.print(',');
  Serial.print(event);        Serial.print(',');
  Serial.print(carsInside);   Serial.print(',');
  Serial.print(freeSpaces()); Serial.print(',');
  Serial.print(entries);      Serial.print(',');
  Serial.println(exits);
}

// Runs once after every sensor scan. It is the "wait in a loop" from the description,
// written so that each call does one round and returns (the LCD, MQTT and serial keep running).
void runGate(unsigned long now) {
  bool a1 = s1.active;
  bool a2 = s2.active;

  if (state == IDLE) {
    if (a1 && !a2 && freeSpaces() > 0) {
      // Car coming in from outside, and there is room
      state = ENTERING;
      passed = false;
      clearSince = 0;
      openBarrier();
      logEvent("OPEN_IN", now);
    } else if (a2 && !a1 && carsInside > 0) {
      // Car going out from inside
      state = EXITING;
      passed = false;
      clearSince = 0;
      openBarrier();
      logEvent("OPEN_OUT", now);
    }
    return;                       // otherwise: do nothing, barrier stays closed
  }

  // ENTERING or EXITING: barrier is open, wait until the car has completely passed
  if (state == ENTERING && a2) passed = true;    // car reached the inside sensor
  if (state == EXITING  && a1) passed = true;    // car reached the outside sensor

  if (a1 || a2) {
    clearSince = 0;               // something still in front of a sensor: not clear yet
  } else if (clearSince == 0) {
    clearSince = now;             // just became clear: start the timer
  }

  if (clearSince != 0 && now - clearSince >= CLEAR_HOLD_MS) {
    // Clear long enough: the car is gone
    if (passed) {
      if (state == ENTERING) {
        entries++;
        carsInside++;
        logEvent("ENTRY", now);
      } else {
        exits++;
        if (carsInside > 0) carsInside--;
        logEvent("EXIT", now);
      }
    } else {
      logEvent("CANCEL", now);    // car came to one sensor but never passed: no count
    }
    closeBarrier();
    state = IDLE;
  }
}

void handleSerialCommands(unsigned long now) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'r')      { carsInside = 0; entries = 0; exits = 0; logEvent("RESET", now); }
    else if (c == '+') { carsInside++; logEvent("MANUAL_ADD", now); }
    else if (c == '-') { if (carsInside > 0) carsInside--; logEvent("MANUAL_SUB", now); }
    else if (c == 'o') { openBarrier(); }
    else if (c == 'c') { closeBarrier(); }
  }
}

// ========================
// Network service (non-blocking for the gate)
// ========================
unsigned long lastNetTry = 0;

void networkService(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    mqtt.loop();
    return;
  }

  // Reconnect only while the gate is idle and the barrier is closed:
  // these calls can take several seconds, and counting pauses meanwhile.
  if (state != IDLE || barrierOpen) return;
  if (now - lastNetTry < NET_RETRY_MS) return;
  lastNetTry = now;

  if (connectWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
    connectMQTT();
  }
}

// Publish the free-space number, but only when it differs from the last one that was sent.
// Retained = the broker keeps the latest value for a dashboard that opens later.
// If the network is down, nothing is sent; connectMQTT() resets lastPublishedFree,
// so the newest value goes out as soon as the link is back.
void publishFreeIfChanged() {
  int f = freeSpaces();
  if (f == lastPublishedFree) return;
  if (!mqtt.connected()) return;

  char payload[8];
  snprintf(payload, sizeof(payload), "%d", f);

  if (mqtt.publish(MQTT_PUBLISH_TOPIC, payload, true)) {
    lastPublishedFree = f;
    Serial.print("MQTT,PUBLISH,");
    Serial.println(payload);
  }
}

// ========================
// LCD
// ========================
void formatDistance(char *buf, const Sensor &s) {
  if (s.distance < 0) strcpy(buf, "---");
  else                snprintf(buf, 5, "%-3d", (int)s.distance);
}

unsigned long lastLcd = 0;

void updateLcd(unsigned long now) {
  if (now - lastLcd < LCD_INTERVAL_MS) return;
  lastLcd = now;

  char line[17], d1[6], d2[6];

  // Row 0: free spaces
  if (freeSpaces() == 0) snprintf(line, sizeof(line), "FULL  0/%-3d      ", TOTAL_SPACES);
  else                   snprintf(line, sizeof(line), "Free: %2d/%-3d     ", freeSpaces(), TOTAL_SPACES);
  lcd.setCursor(0, 0);
  lcd.print(line);

  // Row 1: live sensor distances + barrier state (O = open, C = closed)
  formatDistance(d1, s1);
  formatDistance(d2, s2);
  snprintf(line, sizeof(line), "S1:%-3s S2:%-3s %c ", d1, d2, barrierOpen ? 'O' : 'C');
  lcd.setCursor(0, 1);
  lcd.print(line);
}

// ========================
// Setup / loop
// ========================
unsigned long lastScan = 0;

void setup() {
  Serial.begin(9600);

  // Servo first, so the barrier starts at 90 deg (closed)
  barrier.attach(SERVO_PIN);
  barrier.write(BARRIER_CLOSED_ANGLE);
  barrierOpen = false;

  pinMode(TRIG_1, OUTPUT);  pinMode(ECHO_1, INPUT);
  pinMode(TRIG_2, OUTPUT);  pinMode(ECHO_2, INPUT);
  digitalWrite(TRIG_1, LOW);
  digitalWrite(TRIG_2, LOW);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Parking counter");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  Serial.println("ms,event,occupied,free,entries,exits");

  // One network attempt at startup. If it fails, the gate still works and loop() retries later.
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setSocketTimeout(3);               // don't let a dead broker block for the 15 s default
  lastNetTry = millis();
  if (connectWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
    connectMQTT();
  }

  delay(800);
  lcd.clear();
}

void loop() {
  unsigned long now = millis();

  networkService(now);

  // Scan both sensors one after the other (so they never hear each other), then run the gate logic
  if (now - lastScan >= SCAN_INTERVAL_MS) {
    lastScan = now;
    updateSensor(s1);
    delay(10);                         // let echoes from sensor 1 die out
    updateSensor(s2);
    runGate(now);
  }

  handleSerialCommands(now);
  publishFreeIfChanged();              // sends only when the free-space number changed
  updateLcd(now);
}