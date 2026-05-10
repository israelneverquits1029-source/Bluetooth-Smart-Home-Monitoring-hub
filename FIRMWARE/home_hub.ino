#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <NimBLEDevice.h>

// =========================
// Pin mapping (ESP32-C3)
// =========================
#define OLED_SDA       8
#define OLED_SCL       9

#define DHT_PIN        4
#define DHT_TYPE       DHT22

#define LDR_PIN        0   // AO from LDR module

#define LED_GREEN      1
#define LED_RED        2
#define LED_BLUE       3
#define LED_YELLOW     5
#define LED_WHITE      6

#define BUZZER_PIN     7
#define LAMP_PIN       10  // transistor base resistor from this pin

// =========================
// OLED
// =========================
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =========================
// Sensors
// =========================
DHT dht(DHT_PIN, DHT_TYPE);

// =========================
// BLE UART-style service
// =========================
static BLEUUID serviceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID rxUUID     ("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"); // write
static BLEUUID txUUID     ("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"); // notify

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* txChar = nullptr;
bool bleConnected = false;

// =========================
// System state
// =========================
enum SystemMode {
  MODE_MANUAL,
  MODE_AUTO,
  MODE_ALERT
};

SystemMode currentMode = MODE_AUTO;

float temperatureC = NAN;
float humidityPct = NAN;
int lightRaw = 0;

bool flagHighTemp = false;
bool flagLowTemp = false;
bool flagLowHumidity = false;
bool flagLowLight = false;

bool lampOn = false;
bool lampManualOverride = false;

String lastCommand = "NONE";
String lastEvent = "BOOT";
String lastError = "NONE";

int currentPage = 0; // 0 env, 1 system, 2 events

// =========================
// Thresholds
// =========================
float HIGH_TEMP_THRESHOLD = 30.0;
float LOW_TEMP_THRESHOLD = 18.0;
float LOW_HUM_THRESHOLD = 30.0;
int LOW_LIGHT_THRESHOLD = 1700; // tune after testing

// =========================
// Timing
// =========================
unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long DISPLAY_INTERVAL = 400;

// =========================
// Pending command buffer
// =========================
String pendingCommand = "";
bool commandReady = false;

// =========================
// BLE callbacks
// =========================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    bleConnected = true;
    lastEvent = "BLE connected";
  }

  void onDisconnect(NimBLEServer* pServer) {
    bleConnected = false;
    lastEvent = "BLE disconnected";
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    if (!value.empty()) {
      pendingCommand = String(value.c_str());
      pendingCommand.trim();
      commandReady = true;
    }
  }
};

// =========================
// Utility functions
// =========================
String modeToString(SystemMode mode) {
  switch (mode) {
    case MODE_MANUAL: return "MANUAL";
    case MODE_AUTO:   return "AUTO";
    case MODE_ALERT:  return "ALERT";
    default:          return "UNKNOWN";
  }
}

void sendMessage(const String& msg) {
  Serial.println(msg);
  if (bleConnected && txChar) {
    txChar->setValue(msg.c_str());
    txChar->notify();
  }
}

void beepOnce(int onMs = 80) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(onMs);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepHighTemp() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(70);
    digitalWrite(BUZZER_PIN, LOW);
    delay(60);
  }
}

void beepLowTemp() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(180);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepLowHumidity() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(40);
    digitalWrite(BUZZER_PIN, LOW);
    delay(40);
  }
}

void clearStatusLEDs() {
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_WHITE, LOW);
}

void updateStatusLEDs() {
  clearStatusLEDs();

  bool anyAlert = flagHighTemp || flagLowTemp || flagLowHumidity || flagLowLight;

  if (!anyAlert) {
    digitalWrite(LED_GREEN, HIGH);
  } else {
    if (flagHighTemp)    digitalWrite(LED_RED, HIGH);
    if (flagLowTemp)     digitalWrite(LED_BLUE, HIGH);
    if (flagLowHumidity) digitalWrite(LED_YELLOW, HIGH);
    if (flagLowLight)    digitalWrite(LED_WHITE, HIGH);
  }
}

void updateLampOutput() {
  digitalWrite(LAMP_PIN, lampOn ? HIGH : LOW);
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int l = analogRead(LDR_PIN);

  if (!isnan(t)) temperatureC = t;
  if (!isnan(h)) humidityPct = h;
  lightRaw = l;
}

void evaluateConditions() {
  flagHighTemp = !isnan(temperatureC) && (temperatureC > HIGH_TEMP_THRESHOLD);
  flagLowTemp = !isnan(temperatureC) && (temperatureC < LOW_TEMP_THRESHOLD);
  flagLowHumidity = !isnan(humidityPct) && (humidityPct < LOW_HUM_THRESHOLD);
  flagLowLight = (lightRaw < LOW_LIGHT_THRESHOLD);

  bool anySensorAlert = flagHighTemp || flagLowTemp || flagLowHumidity;

  if (anySensorAlert) {
    currentMode = MODE_ALERT;
  } else if (currentMode == MODE_ALERT) {
    currentMode = MODE_AUTO;
  }

  // Lamp should only auto-react to low light in AUTO mode,
  // unless manual override is active.
  if (!lampManualOverride) {
    if (currentMode == MODE_AUTO) {
      lampOn = flagLowLight;
    } else {
      lampOn = false;
    }
  }
}

void handleAlertBeep() {
  static bool prevHigh = false;
  static bool prevLow = false;
  static bool prevHum = false;

  if (flagHighTemp && !prevHigh) {
    lastEvent = "High temp alert";
    beepHighTemp();
  }
  if (flagLowTemp && !prevLow) {
    lastEvent = "Low temp alert";
    beepLowTemp();
  }
  if (flagLowHumidity && !prevHum) {
    lastEvent = "Low humidity alert";
    beepLowHumidity();
  }

  prevHigh = flagHighTemp;
  prevLow = flagLowTemp;
  prevHum = flagLowHumidity;
}

void sendStatus() {
  String msg = "MODE=" + modeToString(currentMode) +
               " TEMP=" + (isnan(temperatureC) ? String("NA") : String(temperatureC, 1)) +
               "C HUM=" + (isnan(humidityPct) ? String("NA") : String(humidityPct, 1)) +
               "% LIGHT=" + String(lightRaw) +
               " LAMP=" + String(lampOn ? "ON" : "OFF");
  sendMessage(msg);
}

void sendHelp() {
  sendMessage("Commands: HELP, STATUS, AUTO ON, AUTO OFF, LAMP ON, LAMP OFF, BUZZ, SCENE NORMAL, SCENE NIGHT, SCENE ALERT, PAGE NEXT, PAGE PREV");
}

void setSceneNormal() {
  currentMode = MODE_AUTO;
  lampManualOverride = false;
  lampOn = false;
  lastEvent = "Scene normal";
  sendMessage("Scene set: NORMAL");
}

void setSceneNight() {
  currentMode = MODE_MANUAL;
  lampManualOverride = true;
  lampOn = true;
  lastEvent = "Scene night";
  sendMessage("Scene set: NIGHT");
}

void setSceneAlert() {
  currentMode = MODE_ALERT;
  lampManualOverride = true;
  lampOn = true;
  lastEvent = "Scene alert";
  sendMessage("Scene set: ALERT");
}

void parseCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  Serial.println("CMD RECEIVED: " + cmd);

  if (cmd.length() == 0) return;

  lastCommand = cmd;

  if (cmd == "HELP") {
    sendHelp();
  } else if (cmd == "STATUS") {
    sendStatus();
  } else if (cmd == "AUTO ON") {
    currentMode = MODE_AUTO;
    lampManualOverride = false;
    lastEvent = "Auto enabled";
    sendMessage("AUTO mode enabled");
  } else if (cmd == "AUTO OFF") {
    currentMode = MODE_MANUAL;
    lastEvent = "Auto disabled";
    sendMessage("AUTO mode disabled");
  } else if (cmd == "LAMP ON") {
    lampManualOverride = true;
    lampOn = true;
    lastEvent = "Lamp on";
    sendMessage("Lamp ON");
  } else if (cmd == "LAMP OFF") {
    lampManualOverride = true;
    lampOn = false;
    lastEvent = "Lamp off";
    sendMessage("Lamp OFF");
  } else if (cmd == "BUZZ") {
    beepOnce();
    lastEvent = "Manual buzz";
    sendMessage("Buzzer OK");
  } else if (cmd == "SCENE NORMAL") {
    setSceneNormal();
  } else if (cmd == "SCENE NIGHT") {
    setSceneNight();
  } else if (cmd == "SCENE ALERT") {
    setSceneAlert();
  } else if (cmd == "PAGE NEXT") {
    currentPage = (currentPage + 1) % 3;
    lastEvent = "Page next";
    sendMessage("Page changed");
  } else if (cmd == "PAGE PREV") {
    currentPage = (currentPage + 2) % 3;
    lastEvent = "Page prev";
    sendMessage("Page changed");
  } else {
    lastError = "Invalid cmd";
    lastEvent = "Cmd error";
    beepOnce(120);
    sendMessage("ERROR: Invalid command");
  }
}

void pollSerialCommands() {
  static String serialBuffer = "";

  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        pendingCommand = serialBuffer;
        pendingCommand.trim();
        commandReady = true;
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }
}

void drawPage0() {
  display.setCursor(0, 0);
  display.println("Env Dashboard");

  display.setCursor(0, 14);
  display.print("Temp: ");
  if (isnan(temperatureC)) display.println("NA");
  else {
    display.print(temperatureC, 1);
    display.println(" C");
  }

  display.setCursor(0, 26);
  display.print("Hum : ");
  if (isnan(humidityPct)) display.println("NA");
  else {
    display.print(humidityPct, 1);
    display.println(" %");
  }

  display.setCursor(0, 38);
  display.print("Light: ");
  display.println(lightRaw);

  display.setCursor(0, 52);
  display.print("Pg 1/3");
}

void drawPage1() {
  display.setCursor(0, 0);
  display.println("System Status");

  display.setCursor(0, 14);
  display.print("Mode: ");
  display.println(modeToString(currentMode));

  display.setCursor(0, 26);
  display.print("BLE : ");
  display.println(bleConnected ? "Connected" : "Waiting");

  display.setCursor(0, 38);
  display.print("Lamp: ");
  display.println(lampOn ? "ON" : "OFF");

  display.setCursor(0, 52);
  display.print("Pg 2/3");
}

void drawPage2() {
  display.setCursor(0, 0);
  display.println("Events");

  display.setCursor(0, 14);
  display.print("Cmd: ");
  display.println(lastCommand.substring(0, 14));

  display.setCursor(0, 30);
  display.print("Evt: ");
  display.println(lastEvent.substring(0, 14));

  display.setCursor(0, 46);
  display.print("Err: ");
  display.println(lastError.substring(0, 14));
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (currentPage == 0) drawPage0();
  else if (currentPage == 1) drawPage1();
  else drawPage2();

  display.display();
}

void setup() {
  Serial.begin(115200);

  Wire.begin(OLED_SDA, OLED_SCL);

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_WHITE, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LAMP_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LAMP_PIN, LOW);
  clearStatusLEDs();

  dht.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (true) {}
  }
  display.clearDisplay();
  display.display();

  NimBLEDevice::init("C3 Smart Hub");
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService* service = bleServer->createService(serviceUUID);

  NimBLECharacteristic* rxChar =
      service->createCharacteristic(rxUUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());

  txChar = service->createCharacteristic(txUUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  txChar->setValue("Hub ready");

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(serviceUUID);
  advertising->start();

  beepOnce(100);
  lastEvent = "System ready";
  updateDisplay();
  sendMessage("Hub ready");
}

void loop() {
  if (millis() - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = millis();
    readSensors();
    evaluateConditions();
    updateStatusLEDs();
    updateLampOutput();
    handleAlertBeep();
  }

  pollSerialCommands();

  if (commandReady) {
    commandReady = false;
    parseCommand(pendingCommand);
    pendingCommand = "";
    updateLampOutput();
    updateStatusLEDs();
  }

  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
}
