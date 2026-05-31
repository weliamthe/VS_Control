#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>

namespace {

constexpr char WIFI_SSID[] = "TP-Link_F060 - 6307";
constexpr char WIFI_PASSWORD[] = "6307310706";

constexpr char API_KEY[] = "AIzaSyBATQH6JMIHjLL6Zn5VkZ9FqnUQ_b63yGI";
constexpr char DATABASE_URL[] = "https://voltsafe-8ead5-default-rtdb.firebaseio.com/";
constexpr char USER_EMAIL[] = "esp32_1@device.local";
constexpr char USER_PASSWORD[] = "12345678";

constexpr char REALTIME_PATH[] = "/devices/relay_device_1/realtime";
constexpr char LOGS_PATH[] = "/devices/relay_device_1/logs";

constexpr uint8_t RELAY_PINS[] = {33, 25, 26, 27};
constexpr bool RELAY_ACTIVE_HIGH = true;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 30000;
constexpr unsigned long WIFI_RETRY_DELAY_MS = 500;
constexpr unsigned long FIREBASE_READY_TIMEOUT_MS = 15000;

FirebaseData fbdo;
FirebaseData stream;
FirebaseAuth auth;
FirebaseConfig config;

bool relayStates[4] = {false, false, false, false};
bool lastLoggedStates[4] = {false, false, false, false};
bool firebaseReady = false;
bool initialRelaySnapshotLoaded = false;
unsigned long lastHeartbeatMs = 0;

String relayPath(size_t index) {
  return String(REALTIME_PATH) + "/relay" + String(index + 1);
}

void writeRelayPin(size_t index, bool isOn) {
  const bool pinLevel = RELAY_ACTIVE_HIGH ? isOn : !isOn;
  digitalWrite(RELAY_PINS[index], pinLevel ? HIGH : LOW);
}

void setAllRelaysOff() {
  for (size_t i = 0; i < 4; ++i) {
    relayStates[i] = false;
    lastLoggedStates[i] = false;
    writeRelayPin(i, false);
  }
}

void connectWiFi() {
  Serial.printf("Menghubungkan ke WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_RETRY_DELAY_MS);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi terhubung. IP: ");
  Serial.println(WiFi.localIP());
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  Serial.println("Sinkronisasi waktu NTP...");

  time_t now = time(nullptr);
  uint8_t attempts = 0;
  while (now < 100000 && attempts < 20) {
    delay(500);
    Serial.print("#");
    now = time(nullptr);
    attempts++;
  }

  Serial.println();
  if (now >= 100000) {
    Serial.printf("Waktu sinkron: %lld\n", static_cast<long long>(now));
  } else {
    Serial.println("Gagal sinkron NTP, timestamp akan fallback ke 0 sampai waktu tersedia.");
  }
}

int64_t currentTimestamp() {
  const time_t now = time(nullptr);
  return now >= 100000 ? static_cast<int64_t>(now) : 0;
}

void logRelayStates() {
  if (!firebaseReady) {
    return;
  }

  FirebaseJson logJson;
  logJson.set("relay1", relayStates[0]);
  logJson.set("relay2", relayStates[1]);
  logJson.set("relay3", relayStates[2]);
  logJson.set("relay4", relayStates[3]);
  logJson.set("timestamp", currentTimestamp());

  if (Firebase.RTDB.pushJSON(&fbdo, LOGS_PATH, &logJson)) {
    Serial.println("Log relay berhasil disimpan.");
    for (size_t i = 0; i < 4; ++i) {
      lastLoggedStates[i] = relayStates[i];
    }
  } else {
    Serial.printf("Gagal menulis log: %s\n", fbdo.errorReason().c_str());
  }
}

void updatePresence() {
  if (!firebaseReady) {
    return;
  }

  FirebaseJson json;
  json.set("online", true);
  json.set("timestamp", currentTimestamp());

  if (!Firebase.RTDB.updateNode(&fbdo, REALTIME_PATH, &json)) {
    Serial.printf("Gagal update online/timestamp: %s\n", fbdo.errorReason().c_str());
  }
}

void handleRelayChange(size_t index, bool value, bool shouldLog = true) {
  if (index >= 4) {
    return;
  }

  relayStates[index] = value;
  writeRelayPin(index, value);
  Serial.printf("relay%u -> %s\n", static_cast<unsigned>(index + 1), value ? "ON" : "OFF");

  if (!initialRelaySnapshotLoaded || !shouldLog) {
    return;
  }

  bool changed = false;
  for (size_t i = 0; i < 4; ++i) {
    if (relayStates[i] != lastLoggedStates[i]) {
      changed = true;
      break;
    }
  }

  if (changed) {
    logRelayStates();
  }
}

void fetchInitialRelayStates() {
  for (size_t i = 0; i < 4; ++i) {
    bool value = false;
    if (Firebase.RTDB.getBool(&fbdo, relayPath(i))) {
      value = fbdo.boolData();
    } else {
      Serial.printf("relay%u belum ada / gagal dibaca, default OFF. Detail: %s\n",
                    static_cast<unsigned>(i + 1), fbdo.errorReason().c_str());
    }
    handleRelayChange(i, value, false);
    lastLoggedStates[i] = relayStates[i];
  }

  initialRelaySnapshotLoaded = true;
  Serial.println("Status awal relay berhasil dimuat.");
}

void streamCallback(FirebaseStream data) {
  const String path = data.dataPath();
  const String type = data.dataType();

  Serial.printf("Perubahan RTDB diterima. path=%s type=%s\n", path.c_str(), type.c_str());

  if (path == "/") {
    return;
  }

  for (size_t i = 0; i < 4; ++i) {
    if (path == "/relay" + String(i + 1) && type == "boolean") {
      handleRelayChange(i, data.boolData());
      return;
    }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Stream timeout, mencoba melanjutkan...");
  }
}

void beginFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  Firebase.begin(&config, &auth);

  Serial.println("Login ke Firebase...");
  const unsigned long start = millis();
  while (!Firebase.ready() && millis() - start < FIREBASE_READY_TIMEOUT_MS) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  firebaseReady = Firebase.ready();
  if (!firebaseReady) {
    Serial.println("Firebase belum siap. ESP32 akan terus mencoba di loop.");
    return;
  }

  Serial.println("Firebase siap.");
  fetchInitialRelayStates();
  updatePresence();

  if (Firebase.RTDB.beginStream(&stream, REALTIME_PATH)) {
    Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);
    Serial.println("Stream RTDB berhasil dimulai.");
  } else {
    Serial.printf("Gagal memulai stream: %s\n", stream.errorReason().c_str());
  }
}

void ensureFirebaseReady() {
  if (firebaseReady || !Firebase.ready()) {
    return;
  }

  firebaseReady = true;
  Serial.println("Firebase sekarang siap.");
  fetchInitialRelayStates();
  updatePresence();

  if (!Firebase.RTDB.beginStream(&stream, REALTIME_PATH)) {
    Serial.printf("Gagal memulai stream: %s\n", stream.errorReason().c_str());
    return;
  }

  Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);
  Serial.println("Stream RTDB berhasil dimulai.");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("ESP32 Relay Firebase Controller");

  for (uint8_t pin : RELAY_PINS) {
    pinMode(pin, OUTPUT);
  }
  setAllRelaysOff();

  connectWiFi();
  syncTime();
  beginFirebase();
}

void loop() {
  ensureFirebaseReady();

  if (firebaseReady) {
    if (!Firebase.RTDB.readStream(&stream)) {
      const String error = stream.errorReason();
      if (error.length() > 0 && error != "stream timeout") {
        Serial.printf("Stream read error: %s\n", error.c_str());
      }
    }

    const unsigned long now = millis();
    if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs = now;
      updatePresence();
    }
  }

  delay(50);
}
