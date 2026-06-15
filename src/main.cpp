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
const char DEVICE_PATH[] = "/devices/relay_device_1";
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
bool firebaseReadyLogged = false;
bool wifiEventHandlerInstalled = false;
bool deviceInitialized = false;

// Timer
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastFirebaseBeginMs = 0;
unsigned long lastStreamAttemptMs = 0;
unsigned long lastInitialReadAttemptMs = 0;
unsigned long lastDiagnosticsLogMs = 0;
unsigned long lastHeartbeatMs = 0;

const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
const unsigned long FIREBASE_RETRY_INTERVAL_MS = 10000;
const unsigned long STREAM_RETRY_INTERVAL_MS = 5000;
const unsigned long INITIAL_READ_RETRY_INTERVAL_MS = 5000;
const unsigned long DIAGNOSTICS_LOG_INTERVAL_MS = 60000;
const unsigned long HEARTBEAT_INTERVAL_MS = 180000;

// Watchdog
const unsigned long WDT_TIMEOUT_S = 120;
unsigned long lastWatchdogFeedMs = 0;

// Preferences (deteksi koneksi pertama)
Preferences preferences;
const char PREFS_NS[] = "relay";

const char *resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_EXT:       return "External reset";
    case ESP_RST_SW:        return "Software reset";
    case ESP_RST_PANIC:     return "Panic/Guru Meditation";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "Task watchdog";
    case ESP_RST_WDT:       return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "Unknown";
  }
}

void printIpAddress(const IPAddress &ip) {
  Serial.printf("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

void logDiagnostics() {
  IPAddress ip = WiFi.localIP();
  Serial.printf("[DIAG] uptime=%lus wifi=%d ip=", millis() / 1000UL, WiFi.status());
  printIpAddress(ip);
  Serial.printf(" firebase=%s stream=%s init=%s heap=%u\n",
                Firebase.ready() ? "ready" : "not-ready",
                streamStarted ? "on" : "off",
                initialDataLoaded ? "done" : "pending",
                ESP.getFreeHeap());
}

void maybeLogDiagnostics() {
  if (millis() - lastDiagnosticsLogMs < DIAGNOSTICS_LOG_INTERVAL_MS) return;
  logDiagnostics();
  lastDiagnosticsLogMs = millis();
}

void resetFirebaseState() {
  initialDataLoaded = false;
  streamStarted = false;
  firebaseBeginIssued = false;
  firebaseReadyLogged = false;
}

String relayPath(int index) {
  return String(REALTIME_PATH) + "/relay" + String(index + 1);
}

void setRelayPin(int index, bool isOn) {
  digitalWrite(RELAY_PINS[index], isOn ? HIGH : LOW);
}

void setAllRelaysOff() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    relayStates[i] = false;
    setRelayPin(i, false);
  }
}

void applyRelayState(int index, bool value) {
  if (index < 0 || index >= RELAY_COUNT) return;
  if (relayStates[index] == value) return;

  relayStates[index] = value;
  setRelayPin(index, value);
  Serial.printf("relay%d = %s\n", index + 1, value ? "ON" : "OFF");

  preferences.begin(PREFS_NS, false);
  char key[8];
  snprintf(key, sizeof(key), "r%d", index + 1);
  preferences.putBool(key, value);
  preferences.end();
}

void saveAllRelayStates() {
  preferences.begin(PREFS_NS, false);
  for (int i = 0; i < RELAY_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "r%d", i + 1);
    preferences.putBool(key, relayStates[i]);
  }
  preferences.end();
}

void restoreRelayStates() {
  preferences.begin(PREFS_NS, true);
  for (int i = 0; i < RELAY_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "r%d", i + 1);
    relayStates[i] = preferences.getBool(key, false);
    setRelayPin(i, relayStates[i]);
    Serial.printf("restore relay%d = %s\n", i + 1, relayStates[i] ? "ON" : "OFF");
  }
  preferences.end();
}

void applySnapshot(FirebaseJson *json) {
  if (json == nullptr) return;

  FirebaseJsonData result;

  if (json->get(result, "relay1") && result.success && result.type == "bool")
    applyRelayState(0, result.boolValue);
  if (json->get(result, "relay2") && result.success && result.type == "bool")
    applyRelayState(1, result.boolValue);
  if (json->get(result, "relay3") && result.success && result.type == "bool")
    applyRelayState(2, result.boolValue);
  if (json->get(result, "relay4") && result.success && result.type == "bool")
    applyRelayState(3, result.boolValue);

  if (!initialDataLoaded) {
    initialDataLoaded = true;
    Serial.println("Status awal relay dimuat dari stream Firebase.");
  }
}

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WIFI] Connected to AP.");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WIFI] Got IP: ");
      printIpAddress(WiFi.localIP());
      Serial.println();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WIFI] Disconnected from AP.");
      resetFirebaseState();
      break;
    default:
      break;
  }
}

void connectWiFi() {
  Serial.printf("Menghubungkan ke WiFi: %s\n", WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  if (!wifiEventHandlerInstalled) {
    WiFi.onEvent(onWiFiEvent);
    wifiEventHandlerInstalled = true;
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;

  Serial.println("WiFi putus, mencoba sambung lagi...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
}

void configureFirebase() {
  firebaseConfig.api_key = API_KEY;
  firebaseConfig.database_url = DATABASE_URL;
  firebaseConfig.token_status_callback = tokenStatusCallback;

  firebaseAuth.user.email = USER_EMAIL;
  firebaseAuth.user.password = USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  firebaseConfigured = true;
}

void beginFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!firebaseConfigured) configureFirebase();
  if (firebaseBeginIssued && millis() - lastFirebaseBeginMs < FIREBASE_RETRY_INTERVAL_MS) return;

  Serial.println("Menghubungkan ke Firebase...");
  Firebase.begin(&firebaseConfig, &firebaseAuth);
  firebaseBeginIssued = true;
  firebaseReadyLogged = false;
  lastFirebaseBeginMs = millis();
}

void ensureFirebaseReady() {
  if (WiFi.status() != WL_CONNECTED) {
    firebaseReadyLogged = false;
    return;
  }

  if (!firebaseBeginIssued) {
    beginFirebase();
    return;
  }

  if (Firebase.ready()) {
    if (!firebaseReadyLogged) {
      Serial.println("Firebase siap.");
      firebaseReadyLogged = true;
    }
    return;
  }

  firebaseReadyLogged = false;

  if (millis() - lastFirebaseBeginMs >= FIREBASE_RETRY_INTERVAL_MS) {
    Serial.println("Firebase belum siap, mencoba mulai lagi...");
    Firebase.begin(&firebaseConfig, &firebaseAuth);
    lastFirebaseBeginMs = millis();
  }
}

bool initializeDeviceInFirebase() {
  if (!Firebase.ready()) return false;

  Serial.println("Koneksi pertama terdeteksi, menulis data awal ke Firebase...");

  FirebaseJson json;
  for (int i = 0; i < RELAY_COUNT; i++) {
    json.set(String("relay") + String(i + 1), false);
  }

  if (Firebase.RTDB.setJSON(&firebaseData, REALTIME_PATH, &json)) {
    Serial.println("Data relay awal berhasil ditulis ke Firebase.");

    FirebaseJson deviceJson;
    deviceJson.set("relayCount", RELAY_COUNT);
    Firebase.RTDB.setJSON(&firebaseData, DEVICE_PATH, &deviceJson);

    preferences.begin(PREFS_NS, false);
    preferences.putBool("init", true);
    preferences.end();

    deviceInitialized = true;
    initialDataLoaded = true;
    return true;
  }

  Serial.printf("Gagal menulis data awal: %s\n", firebaseData.errorReason().c_str());
  return false;
}

void streamCallback(FirebaseStream data) {
  String path = data.dataPath();
  String type = data.dataType();

  Serial.printf("Data berubah. path=%s type=%s\n", path.c_str(), type.c_str());

  if (path == "/") {
    applySnapshot(data.to<FirebaseJson *>());
    return;
  }

  if (type != "boolean") return;

  if (path == "/relay1")       applyRelayState(0, data.boolData());
  else if (path == "/relay2")  applyRelayState(1, data.boolData());
  else if (path == "/relay3")  applyRelayState(2, data.boolData());
  else if (path == "/relay4")  applyRelayState(3, data.boolData());
  else return;

  if (!initialDataLoaded) {
    initialDataLoaded = true;
    Serial.println("Status relay pertama diterima dari Firebase.");
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout, koneksi akan tetap dipantau.");
}

void startStream() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (lastStreamAttemptMs != 0 && millis() - lastStreamAttemptMs < STREAM_RETRY_INTERVAL_MS) return;

  lastStreamAttemptMs = millis();
  Serial.println("Memulai stream Firebase...");

  if (Firebase.RTDB.beginStream(&streamData, REALTIME_PATH)) {
    Firebase.RTDB.setStreamCallback(&streamData, streamCallback, streamTimeoutCallback);
    streamStarted = true;
    Serial.println("Stream Firebase aktif.");
  } else {
    streamStarted = false;
    Serial.printf("Gagal mulai stream: %s\n", streamData.errorReason().c_str());
  }
}

void sendHeartbeat() {
  if (!Firebase.ready()) return;

  String path = String(DEVICE_PATH) + "/heartbeat";

  if (Firebase.RTDB.setString(&firebaseData, path, String(millis()))) {
    lastHeartbeatMs = millis();
  } else {
    Serial.printf("Heartbeat gagal: %s\n", firebaseData.errorReason().c_str());
    streamStarted = false;
    initialDataLoaded = false;
  }
}

bool readInitialRelayStates() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return false;
  if (initialDataLoaded) return true;
  if (lastInitialReadAttemptMs != 0 &&
      millis() - lastInitialReadAttemptMs < INITIAL_READ_RETRY_INTERVAL_MS) return false;

  lastInitialReadAttemptMs = millis();
  Serial.println("Membaca status relay dari Firebase...");

  bool values[RELAY_COUNT];
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (!Firebase.RTDB.getBool(&firebaseData, relayPath(i))) {
      Serial.printf("Gagal baca relay%d: %s\n", i + 1, firebaseData.errorReason().c_str());
      return false;
    }
    values[i] = firebaseData.boolData();
  }

  for (int i = 0; i < RELAY_COUNT; i++) {
    applyRelayState(i, values[i]);
  }

  initialDataLoaded = true;
  Serial.println("Status relay dimuat dari Firebase.");
  return true;
}

void feedWatchdog() {
  if (millis() - lastWatchdogFeedMs >= 1000) {
    esp_task_wdt_reset();
    lastWatchdogFeedMs = millis();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("ESP32 Relay Firebase Controller");
  Serial.printf("Reset reason: %s\n", resetReasonText(esp_reset_reason()));

  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);

  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  preferences.begin(PREFS_NS, true);
  deviceInitialized = preferences.getBool("init", false);
  preferences.end();

  if (!deviceInitialized) {
    setAllRelaysOff();
    Serial.println("Mode: KONEKSI PERTAMA (relay mati semua)");
  } else {
    restoreRelayStates();
    Serial.println("Mode: RECONNECT (state relay dipulihkan dari NVS)");
  }

  connectWiFi();
  lastWatchdogFeedMs = millis();
}

void loop() {
  feedWatchdog();
  ensureWiFiConnected();

  if (WiFi.status() != WL_CONNECTED) {
    maybeLogDiagnostics();
    delay(50);
    return;
  }

  ensureFirebaseReady();

  if (!Firebase.ready()) {
    maybeLogDiagnostics();
    delay(50);
    return;
  }

  if (!deviceInitialized) {
    if (!initializeDeviceInFirebase()) {
      delay(50);
      return;
    }
  }

  if (!streamStarted) {
    startStream();
  }

  if (streamStarted && !Firebase.RTDB.readStream(&streamData)) {
    String error = streamData.errorReason();
    if (error.length() > 0 && error != "stream timeout") {
      Serial.printf("Stream error: %s\n", error.c_str());
      streamStarted = false;
      initialDataLoaded = false;
    }
  }

  if (!initialDataLoaded) {
    readInitialRelayStates();
  }

  if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
  }

  maybeLogDiagnostics();
  delay(50);
}
