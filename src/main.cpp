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

// Status runtime utama untuk mengatur alur program tanpa blocking.
// Variabel-variabel ini dipakai sebagai penanda state koneksi, stream,
// inisialisasi perangkat, dan status relay saat program berjalan.
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

// Timer berbasis millis() untuk retry koneksi dan penyimpanan state relay.
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

// Watchdog menjaga agar ESP32 bisa reset otomatis jika loop utama macet
// atau terlalu lama tidak merespons.
const unsigned long WDT_TIMEOUT_S = 120;

// Preferences dipakai untuk menyimpan status relay terakhir di NVS,
// sehingga setelah restart perangkat bisa memulihkan kondisi sebelumnya.
Preferences preferences;
const char PREFS_NS[] = "relay";

void feedWatchdog();
void makeRelayFieldName(int relayIndex, char *relayFieldName, size_t fieldNameSize);
int getRelayIndexFromPath(const String &relayPath);

// Membuat nama field Firebase seperti relay1, relay2, dan seterusnya.
void makeRelayFieldName(int relayIndex, char *relayFieldName, size_t fieldNameSize) {
  snprintf(relayFieldName, fieldNameSize, "relay%d", relayIndex + 1);
}

// Mengubah path stream Firebase menjadi indeks relay lokal.
int getRelayIndexFromPath(const String &relayPath) {
  if (relayPath == "/relay1") return 0;
  if (relayPath == "/relay2") return 1;
  if (relayPath == "/relay3") return 2;
  if (relayPath == "/relay4") return 3;
  return -1;
}

// Saat koneksi terputus, status Firebase di-reset agar proses connect
// dan stream bisa dimulai ulang dengan bersih.
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

// Menulis status ON/OFF langsung ke pin relay fisik.
void writeRelayPin(int relayIndex, bool isOn) {
  digitalWrite(RELAY_PINS[relayIndex], isOn ? HIGH : LOW);
}

// Menandai bahwa ada perubahan relay yang nanti perlu disimpan ke NVS.
void markRelayStateChanged() {
  relayStateDirty = true;
  relayStateDirtySinceMs = millis();
}

// Dipakai saat startup pertama atau ketika preferences tidak tersedia,
// agar semua relay masuk ke kondisi aman yaitu OFF.
void turnOffAllRelays() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    relayStates[i] = false;
    writeRelayPin(i, false);
  }
}

// Memperbarui status relay di memori dan pin output hanya jika nilainya berubah.
void updateRelayState(int relayIndex, bool isOn) {
  if (relayIndex < 0 || relayIndex >= RELAY_COUNT) return;
  if (relayStates[relayIndex] == isOn) return;

  relayStates[relayIndex] = isOn;
  writeRelayPin(relayIndex, isOn);
  markRelayStateChanged();
  Serial.printf("relay%d %s\n", relayIndex + 1, isOn ? "ON" : "OFF");
}

// Menyimpan semua status relay ke NVS agar bisa dipulihkan saat restart.
void saveRelayStates() {
  if (!preferencesReady) return;

  for (int relayIndex = 0; relayIndex < RELAY_COUNT; relayIndex++) {
    char relayStateKey[8];
    snprintf(relayStateKey, sizeof(relayStateKey), "r%d", relayIndex + 1);
    preferences.putBool(relayStateKey, relayStates[relayIndex]);
    feedWatchdog();
  }

  relayStateDirty = false;
}

// Penyimpanan dibuat tertunda beberapa saat supaya tidak terlalu sering
// menulis ke flash setiap kali relay berubah.
void saveRelayStatesIfNeeded() {
  if (!relayStateDirty) return;
  if (millis() - relayStateDirtySinceMs < RELAY_STATE_SAVE_DELAY_MS) return;
  saveRelayStates();
}

// Memuat status relay yang tersimpan sebelumnya dari NVS.
void loadSavedRelayStates() {
  if (!preferencesReady) {
    turnOffAllRelays();
    return;
  }

  for (int relayIndex = 0; relayIndex < RELAY_COUNT; relayIndex++) {
    char relayStateKey[8];
    snprintf(relayStateKey, sizeof(relayStateKey), "r%d", relayIndex + 1);
    relayStates[relayIndex] = preferences.getBool(relayStateKey, false);
    writeRelayPin(relayIndex, relayStates[relayIndex]);
  }
}

// Menerapkan snapshot data penuh dari Firebase ke semua relay lokal.
void applyRelaySnapshot(FirebaseJson *snapshotJson) {
  if (snapshotJson == nullptr) return;

  FirebaseJsonData relayValue;
  char relayFieldName[8];

  for (int relayIndex = 0; relayIndex < RELAY_COUNT; relayIndex++) {
    makeRelayFieldName(relayIndex, relayFieldName, sizeof(relayFieldName));
    if (snapshotJson->get(relayValue, relayFieldName) &&
        relayValue.success &&
        relayValue.type == "bool") {
      updateRelayState(relayIndex, relayValue.boolValue);
    }
    feedWatchdog();
  }

  initialDataLoaded = true;
}

// Event WiFi dipakai untuk mendeteksi kapan perangkat berhasil terhubung
// atau kehilangan koneksi ke access point.
void handleWiFiEvent(WiFiEvent_t wifiEvent) {
  switch (wifiEvent) {
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

// Menyiapkan mode WiFi station dan memulai koneksi awal ke router.
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

// Jika WiFi terputus, fungsi ini akan mencoba sambung lagi secara berkala.
void reconnectWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;

  Serial.println("Reconnecting WiFi...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
}

// Konfigurasi dasar Firebase hanya diisi sekali sebelum koneksi dimulai.
void setupFirebaseConfig() {
  firebaseConfig.api_key = API_KEY;
  firebaseConfig.database_url = DATABASE_URL;
  firebaseConfig.token_status_callback = tokenStatusCallback;

  firebaseAuth.user.email = USER_EMAIL;
  firebaseAuth.user.password = USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  firebaseConfigured = true;
}

// Memulai autentikasi dan koneksi Firebase setelah WiFi tersedia.
void startFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!firebaseConfigured) setupFirebaseConfig();
  if (firebaseBeginIssued && millis() - lastFirebaseBeginMs < FIREBASE_RETRY_INTERVAL_MS) return;

  Serial.println("Connecting to Firebase...");
  Firebase.begin(&firebaseConfig, &firebaseAuth);
  firebaseBeginIssued = true;
  lastFirebaseBeginMs = millis();
}

// Jika Firebase belum siap, fungsi ini akan mencoba mulai atau mengulang koneksi.
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

// Saat perangkat pertama kali terhubung, data awal relay dibuat di Firebase
// agar cloud dan device punya struktur data yang sama.
bool createInitialFirebaseData() {
  if (!Firebase.ready()) return false;
  if (deviceInitialized) return true;

  FirebaseJson initialRelayJson;
  for (int relayIndex = 0; relayIndex < RELAY_COUNT; relayIndex++) {
    char relayFieldName[8];
    makeRelayFieldName(relayIndex, relayFieldName, sizeof(relayFieldName));
    initialRelayJson.set(relayFieldName, false);
    feedWatchdog();
  }

  if (!Firebase.RTDB.setJSONAsync(&firebaseData, REALTIME_PATH, &initialRelayJson)) {
    Serial.printf("Gagal menjadwalkan data awal: %s\n", firebaseData.errorReason().c_str());
    return false;
  }

  if (preferencesReady) {
    preferences.putBool("init", true);
  }

  deviceInitialized = true;
  return true;
}

// Callback stream dipanggil saat ada perubahan data dari Firebase.
// Jika path adalah "/", berarti data snapshot penuh. Jika path spesifik,
// berarti hanya satu relay yang berubah.
void handleFirebaseStream(FirebaseStream streamUpdate) {
  String relayPath = streamUpdate.dataPath();
  String dataType = streamUpdate.dataType();

  if (relayPath == "/") {
    applyRelaySnapshot(streamUpdate.to<FirebaseJson *>());
    return;
  }

  if (dataType != "boolean") return;

  int relayIndex = getRelayIndexFromPath(relayPath);
  if (relayIndex < 0) return;

  updateRelayState(relayIndex, streamUpdate.boolData());
  initialDataLoaded = true;
}

// Memulai stream realtime Firebase agar perubahan dari cloud bisa diterima
// tanpa perlu polling manual yang berat.
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

// Jika terlalu lama belum mendapat data awal dari stream, stream akan di-reset
// agar perangkat mencoba membangun koneksi realtime yang baru.
void restartStreamIfNoInitialData() {
  if (!streamStarted || initialDataLoaded) return;
  if (millis() - streamStartedAtMs < INITIAL_STREAM_TIMEOUT_MS) return;

  Firebase.RTDB.endStream(&streamData);
  streamStarted = false;
  streamStartedAtMs = 0;
}

// Watchdog di-feed secara berkala agar ESP32 tahu bahwa loop utama masih sehat.
void feedWatchdog() {
  if (millis() - lastWatchdogFeedMs >= 1000) {
    esp_task_wdt_reset();
    lastWatchdogFeedMs = millis();
  }
}

// Setup
// 1. mulai serial dan watchdog
// 2. siapkan pin relay
// 3. muat state relay lama jika ada
// 4. mulai koneksi WiFi
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

// Loop utama dibuat non-blocking:
// - feed watchdog
// - simpan state relay bila perlu
// - jaga koneksi WiFi dan Firebase
// - pastikan stream aktif
// - proses perubahan dari cloud
// Seluruh alur memakai pengecekan state dan millis(), tanpa delay panjang.
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
    String streamError = streamData.errorReason();
    if (streamError.length() > 0 && streamError != "stream timeout") {
      Serial.printf("Stream error: %s\n", streamError.c_str());
      streamStarted = false;
      initialDataLoaded = false;
      streamStartedAtMs = 0;
    }
  }

  restartStreamIfNoInitialData();
  yield();
}
