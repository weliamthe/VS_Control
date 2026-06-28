#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>

// Data koneksi WiFi
const char WIFI_SSID[] = "TP-Link_F060 - 6307";
const char WIFI_PASSWORD[] = "6307310706";

// Data koneksi Firebase
const char API_KEY[] = "AIzaSyBATQH6JMIHjLL6Zn5VkZ9FqnUQ_b63yGI";
const char DATABASE_URL[] = "https://voltsafe-8ead5-default-rtdb.firebaseio.com/";
const char USER_EMAIL[] = "esp32_1@device.local";
const char USER_PASSWORD[] = "12345678";

// Lokasi data di Firebase
const char REALTIME_PATH[] = "/devices/relay_device_1/realtime";

// Pengaturan relay
const int RELAY_COUNT = 4;
const int RELAY_PINS[RELAY_COUNT] = {33, 25, 26, 27};

// Objek utama Firebase
FirebaseData firebaseData;
FirebaseData streamData;
FirebaseAuth firebaseAuth;
FirebaseConfig firebaseConfig;

// Status runtime
bool relayStates[RELAY_COUNT] = {false, false, false, false};
bool initialDataLoaded = false;
bool streamStarted = false;
bool firebaseConfigured = false;
bool firebaseBeginIssued = false;
bool wifiEventHandlerInstalled = false;
bool deviceInitialized = false;
bool preferencesReady = false;
bool relayStateDirty = false;
bool wifiConnectedLogged = false;
bool firebaseReadyLogged = false;

// Timer
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastFirebaseBeginMs = 0;
unsigned long lastStreamAttemptMs = 0;
unsigned long lastWatchdogFeedMs = 0;
unsigned long relayStateDirtySinceMs = 0;
unsigned long streamStartedAtMs = 0;

const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
const unsigned long FIREBASE_RETRY_INTERVAL_MS = 10000;
const unsigned long STREAM_RETRY_INTERVAL_MS = 5000;
const unsigned long RELAY_STATE_SAVE_DELAY_MS = 2000;
const unsigned long INITIAL_STREAM_TIMEOUT_MS = 15000;

// Watchdog
const unsigned long WDT_TIMEOUT_S = 120;

// Preferences
Preferences preferences;
const char PREFS_NS[] = "relay";

void feedWatchdog();
void makeRelayFieldName(int index, char *buffer, size_t bufferSize);
int getRelayIndexFromPath(const String &path);

void makeRelayFieldName(int index, char *buffer, size_t bufferSize) {
  snprintf(buffer, bufferSize, "relay%d", index + 1);
}

int getRelayIndexFromPath(const String &path) {
  if (path == "/relay1") return 0;
  if (path == "/relay2") return 1;
  if (path == "/relay3") return 2;
  if (path == "/relay4") return 3;
  return -1;
}

void resetFirebaseConnection() {
  if (streamStarted) {
    Firebase.RTDB.endStream(&streamData);
  }

  initialDataLoaded = false;
  streamStarted = false;
  firebaseBeginIssued = false;
  firebaseReadyLogged = false;
  lastStreamAttemptMs = 0;
  streamStartedAtMs = 0;
}

void writeRelayPin(int index, bool isOn) {
  digitalWrite(RELAY_PINS[index], isOn ? HIGH : LOW);
}

void markRelayStateChanged() {
  relayStateDirty = true;
  relayStateDirtySinceMs = millis();
}

void turnOffAllRelays() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    relayStates[i] = false;
    writeRelayPin(i, false);
  }
}

void updateRelayState(int index, bool value) {
  if (index < 0 || index >= RELAY_COUNT) return;
  if (relayStates[index] == value) return;

  relayStates[index] = value;
  writeRelayPin(index, value);
  markRelayStateChanged();
  Serial.printf("relay%d %s\n", index + 1, value ? "ON" : "OFF");
}

void saveRelayStates() {
  if (!preferencesReady) return;

  for (int i = 0; i < RELAY_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "r%d", i + 1);
    preferences.putBool(key, relayStates[i]);
    feedWatchdog();
  }

  relayStateDirty = false;
}

void saveRelayStatesIfNeeded() {
  if (!relayStateDirty) return;
  if (millis() - relayStateDirtySinceMs < RELAY_STATE_SAVE_DELAY_MS) return;
  saveRelayStates();
}

void loadSavedRelayStates() {
  if (!preferencesReady) {
    turnOffAllRelays();
    return;
  }

  for (int i = 0; i < RELAY_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "r%d", i + 1);
    relayStates[i] = preferences.getBool(key, false);
    writeRelayPin(i, relayStates[i]);
  }
}

void applyRelaySnapshot(FirebaseJson *json) {
  if (json == nullptr) return;

  FirebaseJsonData result;
  char key[8];

  for (int i = 0; i < RELAY_COUNT; i++) {
    makeRelayFieldName(i, key, sizeof(key));
    if (json->get(result, key) && result.success && result.type == "bool") {
      updateRelayState(i, result.boolValue);
    }
    feedWatchdog();
  }

  initialDataLoaded = true;
}

void handleWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnectedLogged = true;
      Serial.println("WiFi connected");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiConnectedLogged = false;
      Serial.println("WiFi disconnected");
      resetFirebaseConnection();
      break;
    default:
      break;
  }
}

void startWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  if (!wifiEventHandlerInstalled) {
    WiFi.onEvent(handleWiFiEvent);
    wifiEventHandlerInstalled = true;
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
}

void reconnectWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;

  Serial.println("Reconnecting WiFi...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
}

void setupFirebaseConfig() {
  firebaseConfig.api_key = API_KEY;
  firebaseConfig.database_url = DATABASE_URL;
  firebaseConfig.token_status_callback = tokenStatusCallback;

  firebaseAuth.user.email = USER_EMAIL;
  firebaseAuth.user.password = USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  firebaseConfigured = true;
}

void startFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!firebaseConfigured) setupFirebaseConfig();
  if (firebaseBeginIssued && millis() - lastFirebaseBeginMs < FIREBASE_RETRY_INTERVAL_MS) return;

  Serial.println("Connecting to Firebase...");
  Firebase.begin(&firebaseConfig, &firebaseAuth);
  firebaseBeginIssued = true;
  lastFirebaseBeginMs = millis();
}

void reconnectFirebaseIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!firebaseBeginIssued) {
    startFirebase();
    return;
  }

  if (Firebase.ready()) {
    if (!firebaseReadyLogged) {
      Serial.println("Firebase connected");
      firebaseReadyLogged = true;
    }
    return;
  }

  if (millis() - lastFirebaseBeginMs >= FIREBASE_RETRY_INTERVAL_MS) {
    firebaseReadyLogged = false;
    Serial.println("Retrying Firebase...");
    Firebase.begin(&firebaseConfig, &firebaseAuth);
    lastFirebaseBeginMs = millis();
  }
}

bool createInitialFirebaseData() {
  if (!Firebase.ready()) return false;
  if (deviceInitialized) return true;

  FirebaseJson json;
  for (int i = 0; i < RELAY_COUNT; i++) {
    char key[8];
    makeRelayFieldName(i, key, sizeof(key));
    json.set(key, false);
    feedWatchdog();
  }

  if (!Firebase.RTDB.setJSONAsync(&firebaseData, REALTIME_PATH, &json)) {
    Serial.printf("Gagal menjadwalkan data awal: %s\n", firebaseData.errorReason().c_str());
    return false;
  }

  if (preferencesReady) {
    preferences.putBool("init", true);
  }

  deviceInitialized = true;
  return true;
}

void handleFirebaseStream(FirebaseStream data) {
  String path = data.dataPath();
  String type = data.dataType();

  if (path == "/") {
    applyRelaySnapshot(data.to<FirebaseJson *>());
    return;
  }

  if (type != "boolean") return;

  int relayIndex = getRelayIndexFromPath(path);
  if (relayIndex < 0) return;

  updateRelayState(relayIndex, data.boolData());
  initialDataLoaded = true;
}

void startFirebaseStream() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (lastStreamAttemptMs != 0 && millis() - lastStreamAttemptMs < STREAM_RETRY_INTERVAL_MS) return;

  lastStreamAttemptMs = millis();

  if (Firebase.RTDB.beginStream(&streamData, REALTIME_PATH)) {
    Firebase.RTDB.setStreamCallback(&streamData, handleFirebaseStream, nullptr);
    streamStarted = true;
    streamStartedAtMs = millis();
    Serial.println("Firebase stream active");
  } else {
    streamStarted = false;
    streamStartedAtMs = 0;
    Serial.printf("Gagal mulai stream: %s\n", streamData.errorReason().c_str());
  }
}

void restartStreamIfNoInitialData() {
  if (!streamStarted || initialDataLoaded) return;
  if (millis() - streamStartedAtMs < INITIAL_STREAM_TIMEOUT_MS) return;

  Firebase.RTDB.endStream(&streamData);
  streamStarted = false;
  streamStartedAtMs = 0;
}

void feedWatchdog() {
  if (millis() - lastWatchdogFeedMs >= 1000) {
    esp_task_wdt_reset();
    lastWatchdogFeedMs = millis();
  }
}

void setup() {
  Serial.begin(115200);
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);

  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  preferencesReady = preferences.begin(PREFS_NS, false);
  deviceInitialized = preferencesReady ? preferences.getBool("init", false) : false;

  if (!deviceInitialized) {
    turnOffAllRelays();
  } else {
    loadSavedRelayStates();
  }

  startWiFi();
  lastWatchdogFeedMs = millis();
}

void loop() {
  feedWatchdog();
  saveRelayStatesIfNeeded();
  reconnectWiFiIfNeeded();

  if (WiFi.status() != WL_CONNECTED) {
    yield();
    return;
  }

  reconnectFirebaseIfNeeded();

  if (!Firebase.ready()) {
    yield();
    return;
  }

  if (!deviceInitialized) {
    createInitialFirebaseData();
  }

  if (!streamStarted) {
    startFirebaseStream();
  }

  if (streamStarted && !Firebase.RTDB.readStream(&streamData)) {
    String error = streamData.errorReason();
    if (error.length() > 0 && error != "stream timeout") {
      Serial.printf("Stream error: %s\n", error.c_str());
      streamStarted = false;
      initialDataLoaded = false;
      streamStartedAtMs = 0;
    }
  }

  restartStreamIfNoInitialData();
  yield();
}
