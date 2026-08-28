#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

// Fish feeder ESP32-C3 Super Mini + 28BYJ-48 + ULN2003 + 1.3" I2C OLED.
// Libraries: U8g2, ArduinoJson, NimBLE-Arduino.

#define DEVICE_NAME "FishFeeder"

#define BUTTON_PIN 4
#define OLED_SDA 8
#define OLED_SCL 9

#define MOTOR_PIN_1 0
#define MOTOR_PIN_2 1
#define MOTOR_PIN_3 2
#define MOTOR_PIN_4 3

#define STEPS_PER_REVOLUTION 2048L
#define DEGREES_PER_RATION 22.5f
#define MOTOR_STEP_DELAY_MS 10
#define MOTOR_REVERSE 0
#define MAX_SCHEDULES 3
#define BLE_PAIRING_TIMEOUT_MS 120000UL

#define BLE_SERVICE_UUID "5b857000-2f5c-4e7b-bb6d-4f77230c0001"
#define BLE_CONFIG_UUID  "5b857001-2f5c-4e7b-bb6d-4f77230c0001"

struct FeedSchedule {
  bool enabled;
  uint8_t hour;
  uint8_t minute;
  uint8_t rations;
  int lastFedDay;
};

Preferences prefs;
FeedSchedule schedules[MAX_SCHEDULES];

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* configCharacteristic = nullptr;

bool bleEnabled = false;
bool bleClientConnected = false;
bool feedingNow = false;
uint32_t bleStartedAt = 0;
int64_t epochBase = 0;
uint32_t millisBase = 0;
int32_t timezoneOffsetSec = 0;
uint8_t pendingRations = 0;
uint8_t activeSchedule = 255;
long feedStepsRemaining = 0;
uint8_t motorStepIndex = 0;
uint32_t nextMotorStepAt = 0;

const uint8_t motorSequence[8][4] = {
  {1, 0, 0, 0},
  {1, 1, 0, 0},
  {0, 1, 0, 0},
  {0, 1, 1, 0},
  {0, 0, 1, 0},
  {0, 0, 1, 1},
  {0, 0, 0, 1},
  {1, 0, 0, 1}
};

long rationSteps() {
  return lround((DEGREES_PER_RATION / 360.0f) * STEPS_PER_REVOLUTION);
}

void writeMotorStep(uint8_t index) {
  digitalWrite(MOTOR_PIN_1, motorSequence[index][0]);
  digitalWrite(MOTOR_PIN_2, motorSequence[index][1]);
  digitalWrite(MOTOR_PIN_3, motorSequence[index][2]);
  digitalWrite(MOTOR_PIN_4, motorSequence[index][3]);
}

void motorOff() {
  digitalWrite(MOTOR_PIN_1, LOW);
  digitalWrite(MOTOR_PIN_2, LOW);
  digitalWrite(MOTOR_PIN_3, LOW);
  digitalWrite(MOTOR_PIN_4, LOW);
}

bool timeIsValid() {
  return epochBase > 1700000000LL;
}

int64_t nowEpoch() {
  if (!timeIsValid()) return 0;
  return epochBase + ((int64_t)(millis() - millisBase) / 1000LL);
}

void saveConfig() {
  prefs.begin("feeder", false);
  prefs.putLong64("epoch", epochBase);
  prefs.putUInt("millisBase", millisBase);
  prefs.putInt("tz", timezoneOffsetSec);
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    char key[16];
    snprintf(key, sizeof(key), "en%d", i);
    prefs.putBool(key, schedules[i].enabled);
    snprintf(key, sizeof(key), "h%d", i);
    prefs.putUChar(key, schedules[i].hour);
    snprintf(key, sizeof(key), "m%d", i);
    prefs.putUChar(key, schedules[i].minute);
    snprintf(key, sizeof(key), "r%d", i);
    prefs.putUChar(key, schedules[i].rations);
  }
  prefs.end();
}

void loadConfig() {
  prefs.begin("feeder", true);
  epochBase = prefs.getLong64("epoch", 0);
  millisBase = millis();
  timezoneOffsetSec = prefs.getInt("tz", 0);
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    char key[16];
    snprintf(key, sizeof(key), "en%d", i);
    schedules[i].enabled = prefs.getBool(key, i == 0);
    snprintf(key, sizeof(key), "h%d", i);
    schedules[i].hour = prefs.getUChar(key, 12);
    snprintf(key, sizeof(key), "m%d", i);
    schedules[i].minute = prefs.getUChar(key, 0);
    snprintf(key, sizeof(key), "r%d", i);
    schedules[i].rations = prefs.getUChar(key, 1);
    schedules[i].lastFedDay = -1;
  }
  prefs.end();
}

void startFeeding(uint8_t rations, uint8_t scheduleIndex) {
  if (feedingNow || rations == 0) return;
  pendingRations = rations;
  activeSchedule = scheduleIndex;
  feedingNow = true;
  feedStepsRemaining = rationSteps();
  nextMotorStepAt = 0;
}

void runFeedingMotor() {
  if (!feedingNow) return;

  if (feedStepsRemaining > 0) {
    uint32_t now = millis();
    if ((int32_t)(now - nextMotorStepAt) < 0) return;
    nextMotorStepAt = now + MOTOR_STEP_DELAY_MS;

#if MOTOR_REVERSE
    motorStepIndex = (motorStepIndex + 7) % 8;
#else
    motorStepIndex = (motorStepIndex + 1) % 8;
#endif
    writeMotorStep(motorStepIndex);
    feedStepsRemaining--;
    return;
  }

  if (pendingRations > 1) {
    pendingRations--;
    feedStepsRemaining = rationSteps();
    nextMotorStepAt = millis() + 250;
    return;
  }

  if (activeSchedule < MAX_SCHEDULES && timeIsValid()) {
    schedules[activeSchedule].lastFedDay = (nowEpoch() + timezoneOffsetSec) / 86400;
  }
  pendingRations = 0;
  activeSchedule = 255;
  feedingNow = false;
  motorOff();
}

void checkSchedules() {
  if (!timeIsValid() || feedingNow) return;

  int64_t localNow = nowEpoch() + timezoneOffsetSec;
  int day = localNow / 86400;
  int minuteOfDay = (localNow % 86400) / 60;

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (!schedules[i].enabled || schedules[i].rations == 0) continue;
    int targetMinute = schedules[i].hour * 60 + schedules[i].minute;
    if (minuteOfDay == targetMinute && schedules[i].lastFedDay != day) {
      startFeeding(schedules[i].rations, i);
      break;
    }
  }
}

void sendStatus(const char* message) {
  if (!configCharacteristic) return;

  StaticJsonDocument<256> doc;
  doc["status"] = message;
  doc["timeOk"] = timeIsValid();
  doc["feeding"] = feedingNow;
  doc["degreesPerRation"] = DEGREES_PER_RATION;

  String out;
  serializeJson(doc, out);
  configCharacteristic->setValue(out.c_str());
  configCharacteristic->notify();
}

void applyConfigJson(const std::string& value) {
  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, value.c_str());
  if (err) {
    sendStatus("json_error");
    return;
  }

  if (doc["epoch"].is<int64_t>()) {
    epochBase = doc["epoch"].as<int64_t>();
    millisBase = millis();
  }

  if (doc["tzOffsetSec"].is<int32_t>()) {
    timezoneOffsetSec = doc["tzOffsetSec"].as<int32_t>();
  }

  JsonArray arr = doc["schedules"].as<JsonArray>();
  if (!arr.isNull()) {
    int i = 0;
    for (JsonObject item : arr) {
      if (i >= MAX_SCHEDULES) break;
      schedules[i].enabled = item["enabled"] | false;
      schedules[i].hour = constrain(item["hour"] | 12, 0, 23);
      schedules[i].minute = constrain(item["minute"] | 0, 0, 59);
      schedules[i].rations = constrain(item["rations"] | 1, 0, 9);
      schedules[i].lastFedDay = -1;
      i++;
    }
  }

  if (doc["feedNow"] | false) {
    startFeeding(doc["feedNowRations"] | 1, 255);
  }

  saveConfig();
  sendStatus("saved");
}

class ConfigCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    applyConfigJson(characteristic->getValue());
  }
};

void startBleAdvertising() {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->stop();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setName(DEVICE_NAME);
  advertising->start();
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    bleClientConnected = true;
    bleStartedAt = millis();
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    bleClientConnected = false;
    bleStartedAt = millis();
    if (bleEnabled) {
      startBleAdvertising();
    }
  }
};

void startBlePairing() {
  if (bleEnabled) {
    bleStartedAt = millis();
    if (!bleClientConnected) {
      startBleAdvertising();
    }
    return;
  }

  NimBLEDevice::init(DEVICE_NAME);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  NimBLEService* service = bleServer->createService(BLE_SERVICE_UUID);

  configCharacteristic = service->createCharacteristic(
    BLE_CONFIG_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
  );
  configCharacteristic->setCallbacks(new ConfigCallbacks());
  configCharacteristic->setValue("{\"status\":\"ready\"}");

  service->start();
  startBleAdvertising();

  bleEnabled = true;
  bleClientConnected = false;
  bleStartedAt = millis();
}

void stopBlePairing() {
  if (!bleEnabled) return;
  NimBLEDevice::getAdvertising()->stop();
  NimBLEDevice::deinit(true);
  bleServer = nullptr;
  configCharacteristic = nullptr;
  bleEnabled = false;
  bleClientConnected = false;
}

void handleButton() {
  static bool last = HIGH;
  static uint32_t lastChange = 0;
  bool current = digitalRead(BUTTON_PIN);

  if (current != last && millis() - lastChange > 40) {
    lastChange = millis();
    last = current;
    if (current == LOW) startBlePairing();
  }
}

void drawDisplay() {
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tf);
  display.drawStr(0, 11, "Fish feeder");

  if (bleEnabled) {
    display.drawStr(78, 11, bleClientConnected ? "BLE*" : "BLE");
  }

  if (feedingNow) {
    display.drawStr(0, 25, "Alimentando...");
  } else if (timeIsValid()) {
    int64_t localNow = nowEpoch() + timezoneOffsetSec;
    int seconds = localNow % 86400;
    char line[24];
    snprintf(line, sizeof(line), "Hora %02d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    display.drawStr(0, 25, line);
  } else {
    display.drawStr(0, 25, "Sin hora: abrir app");
  }

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    char line[32];
    snprintf(line, sizeof(line), "%d %s %02d:%02d x%d",
             i + 1,
             schedules[i].enabled ? "ON " : "OFF",
             schedules[i].hour,
             schedules[i].minute,
             schedules[i].rations);
    display.drawStr(0, 39 + i * 12, line);
  }

  display.sendBuffer();
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(MOTOR_PIN_1, OUTPUT);
  pinMode(MOTOR_PIN_2, OUTPUT);
  pinMode(MOTOR_PIN_3, OUTPUT);
  pinMode(MOTOR_PIN_4, OUTPUT);
  motorOff();

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin();

  loadConfig();
}

void loop() {
  handleButton();
  checkSchedules();
  runFeedingMotor();

  if (bleEnabled && !bleClientConnected && millis() - bleStartedAt > BLE_PAIRING_TIMEOUT_MS) {
    stopBlePairing();
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 500) {
    lastDraw = millis();
    drawDisplay();
  }
}
