#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>

// =========================
// Konfigurasi koneksi
// =========================
// Kredensial WiFi yang dipakai ESP32 untuk tersambung ke jaringan lokal.
const char WIFI_SSID[] = "SSID";
const char WIFI_PASSWORD[] = "PASSWORD";

// Kredensial dan endpoint Firebase Realtime Database.
const char API_KEY[] = "AIzaSyBATQH6JMIHjLL6Zn5VkZ9FqnUQ_b63yGI";
const char DATABASE_URL[] = "https://voltsafe-8ead5-default-rtdb.firebaseio.com/";
const char USER_EMAIL[] = "esp32_1@device.local";
const char USER_PASSWORD[] = "12345678";

// Path utama data relay di Firebase.
const char REALTIME_PATH[] = "/devices/relay_device_1/realtime";

// Mapping jumlah relay dan pin output yang dipakai di ESP32.
const int RELAY_COUNT = 4;
const int RELAY_PINS[RELAY_COUNT] = {33, 25, 26, 27};

// =========================
// Objek library utama
// =========================
// `firebaseData` dipakai untuk operasi baca/tulis biasa.
// `streamData` dipakai khusus untuk koneksi stream realtime.
FirebaseData firebaseData;
FirebaseData streamData;
FirebaseAuth firebaseAuth;
FirebaseConfig firebaseConfig;

// =========================
// Status runtime
// =========================
// Menyimpan status relay yang sedang aktif di memori.
bool relayStates[RELAY_COUNT] = {false, false, false, false};

// Penanda state mesin utama agar kita tahu koneksi dan data sudah sampai mana.
bool initialDataLoaded = false;
bool streamStarted = false;
bool firebaseConfigured = false;
bool firebaseBeginIssued = false;
bool firebaseReadyLogged = false;
bool wifiEventHandlerInstalled = false;
bool deviceInitialized = false;
bool preferencesReady = false;
bool relayStateDirty = false;

// =========================
// Timer berbasis millis()
// =========================
// Semua timer di bawah ini dipakai agar program berjalan non-blocking.
// Jadi tidak ada `delay()` di loop utama, dan tugas periodik dijalankan
// hanya saat waktunya tiba.
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastFirebaseBeginMs = 0;
unsigned long lastStreamAttemptMs = 0;
unsigned long lastInitialReadAttemptMs = 0;
unsigned long lastDiagnosticsLogMs = 0;
unsigned long relayStateDirtySinceMs = 0;
unsigned long lastMainLoopRunMs = 0;

const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
const unsigned long FIREBASE_RETRY_INTERVAL_MS = 10000;
const unsigned long STREAM_RETRY_INTERVAL_MS = 5000;
const unsigned long INITIAL_READ_RETRY_INTERVAL_MS = 5000;
const unsigned long DIAGNOSTICS_LOG_INTERVAL_MS = 60000;
const unsigned long RELAY_STATE_SAVE_DELAY_MS = 2000;
const unsigned long MAIN_LOOP_INTERVAL_MS = 50;

// =========================
// Watchdog
// =========================
// Watchdog dipakai untuk memastikan firmware tidak hang terlalu lama.
const unsigned long WDT_TIMEOUT_S = 120;
unsigned long lastWatchdogFeedMs = 0;
uint32_t minFreeHeap = UINT32_MAX;

// =========================
// Preferences / NVS
// =========================
// NVS dipakai untuk:
// 1. Menandai apakah perangkat sudah pernah inisialisasi ke Firebase.
// 2. Menyimpan status relay terakhir agar bisa dipulihkan setelah restart.
Preferences preferences;
const char PREFS_NS[] = "relay";

void feedWatchdog();
void runControllerCycle();

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

// Menampilkan status periodik untuk membantu debug saat alat berjalan lama.
void logDiagnostics() {
  IPAddress ip = WiFi.localIP();
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < minFreeHeap) minFreeHeap = freeHeap;
  Serial.printf("[DIAG] uptime=%lus wifi=%d ip=", millis() / 1000UL, WiFi.status());
  printIpAddress(ip);
  Serial.printf(" firebase=%s stream=%s init=%s heap=%u minHeap=%u dirty=%s\n",
                Firebase.ready() ? "ready" : "not-ready",
                streamStarted ? "on" : "off",
                initialDataLoaded ? "done" : "pending",
                freeHeap,
                minFreeHeap,
                relayStateDirty ? "yes" : "no");
}

// Hanya log jika intervalnya sudah lewat agar serial monitor tidak banjir.
void maybeLogDiagnostics() {
  if (millis() - lastDiagnosticsLogMs < DIAGNOSTICS_LOG_INTERVAL_MS) return;
  logDiagnostics();
  lastDiagnosticsLogMs = millis();
}

// Saat WiFi/Firebase putus, status stream dan proses baca awal harus diulang.
void resetFirebaseState() {
  if (streamStarted) {
    Firebase.RTDB.endStream(&streamData);
  }
  initialDataLoaded = false;
  streamStarted = false;
  firebaseBeginIssued = false;
  firebaseReadyLogged = false;
  lastStreamAttemptMs = 0;
  lastInitialReadAttemptMs = 0;
}

// Membentuk path seperti `/devices/.../relay1`, `/relay2`, dst.
void buildRelayPath(int index, char *buffer, size_t bufferSize) {
  snprintf(buffer, bufferSize, "%s/relay%d", REALTIME_PATH, index + 1);
}

// Memisahkan penulisan pin fisik agar logika relay lebih mudah dibaca.
void setRelayPin(int index, bool isOn) {
  digitalWrite(RELAY_PINS[index], isOn ? HIGH : LOW);
}

// Menandai bahwa state relay berubah dan perlu disimpan ke NVS,
// tetapi penyimpanan ditunda sebentar agar tidak terlalu sering write flash.
void markRelayStatesDirty() {
  relayStateDirty = true;
  relayStateDirtySinceMs = millis();
}

// Mode aman: semua relay dimatikan.
void setAllRelaysOff() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    relayStates[i] = false;
    setRelayPin(i, false);
  }
}

// Mengubah state relay hanya jika nilainya memang berbeda.
// Cara ini menghindari penulisan pin yang tidak perlu.
void applyRelayState(int index, bool value) {
  if (index < 0 || index >= RELAY_COUNT) return;
  if (relayStates[index] == value) return;

  relayStates[index] = value;
  setRelayPin(index, value);
  Serial.printf("relay%d = %s\n", index + 1, value ? "ON" : "OFF");
  markRelayStatesDirty();
}

// Menyimpan seluruh state relay ke NVS.
// Penyimpanan dilakukan dalam satu batch agar state konsisten.
void saveAllRelayStates() {
  if (!preferencesReady) return;

  for (int i = 0; i < RELAY_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "r%d", i + 1);
    preferences.putBool(key, relayStates[i]);
    feedWatchdog();
  }

  relayStateDirty = false;
  Serial.println("State relay disimpan ke NVS.");
}

// Menunda write ke flash beberapa saat setelah perubahan terakhir.
// Tujuannya agar aman untuk flash memory dan tetap responsif.
void maybeSaveRelayStates() {
  if (!relayStateDirty) return;
  if (millis() - relayStateDirtySinceMs < RELAY_STATE_SAVE_DELAY_MS) return;
  saveAllRelayStates();
}

// Memulihkan state relay terakhir dari NVS setelah restart.
void restoreRelayStates() {
  if (!preferencesReady) {
    setAllRelaysOff();
    Serial.println("Preferences belum siap, relay dibuat OFF sebagai mode aman.");
    return;
  }

  for (int i = 0; i < RELAY_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "r%d", i + 1);
    relayStates[i] = preferences.getBool(key, false);
    setRelayPin(i, relayStates[i]);
    Serial.printf("restore relay%d = %s\n", i + 1, relayStates[i] ? "ON" : "OFF");
  }
}

// Menerapkan snapshot lengkap yang datang dari stream Firebase.
// Snapshot biasanya muncul saat awal stream aktif atau ketika subtree berubah.
void applySnapshot(FirebaseJson *json) {
  if (json == nullptr) return;

  FirebaseJsonData result;
  char key[8];

  for (int i = 0; i < RELAY_COUNT; i++) {
    snprintf(key, sizeof(key), "relay%d", i + 1);
    if (json->get(result, key) && result.success && result.type == "bool") {
      applyRelayState(i, result.boolValue);
    }
    feedWatchdog();
  }

  if (!initialDataLoaded) {
    initialDataLoaded = true;
    Serial.println("Status awal relay dimuat dari stream Firebase.");
  }
}

// Event WiFi dipakai untuk mendeteksi perubahan koneksi tanpa polling berat.
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

// Setup koneksi WiFi awal.
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

// Retry koneksi WiFi secara berkala tanpa blocking.
void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;

  Serial.println("WiFi putus, mencoba sambung lagi...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
}

// Mengisi struktur konfigurasi Firebase sekali saja.
void configureFirebase() {
  firebaseConfig.api_key = API_KEY;
  firebaseConfig.database_url = DATABASE_URL;
  firebaseConfig.token_status_callback = tokenStatusCallback;

  firebaseAuth.user.email = USER_EMAIL;
  firebaseAuth.user.password = USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  firebaseConfigured = true;
}

// Memulai session Firebase. Fungsi ini aman dipanggil berulang
// karena ada proteksi interval retry.
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

// Menjaga agar Firebase siap dipakai.
// Jika belum siap, sistem akan mencoba inisialisasi ulang secara periodik.
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

// Menulis struktur data awal ke Firebase hanya sekali saat perangkat baru pertama kali aktif.
bool initializeDeviceInFirebase() {
  if (!Firebase.ready()) return false;

  Serial.println("Koneksi pertama terdeteksi, menulis data awal ke Firebase...");

  FirebaseJson json;
  char key[8];
  for (int i = 0; i < RELAY_COUNT; i++) {
    snprintf(key, sizeof(key), "relay%d", i + 1);
    json.set(key, false);
    feedWatchdog();
  }

  if (!Firebase.RTDB.setJSON(&firebaseData, REALTIME_PATH, &json)) {
    Serial.printf("Gagal menulis data awal: %s\n", firebaseData.errorReason().c_str());
    return false;
  }

  if (preferencesReady) {
    preferences.putBool("init", true);
  }

  deviceInitialized = true;
  initialDataLoaded = true;
  markRelayStatesDirty();
  Serial.println("Data awal Firebase berhasil dibuat.");
  return true;
}

// Callback stream dipanggil setiap ada perubahan data realtime di Firebase.
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

// Timeout stream tidak selalu fatal. Biasanya koneksi masih bisa lanjut.
void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout, koneksi akan tetap dipantau.");
}

// Menyalakan stream realtime jika koneksi WiFi dan Firebase sudah siap.
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

// Fallback pembacaan awal.
// Jika stream belum sempat memberi snapshot awal, kita baca satu per satu nilai relay.
bool readInitialRelayStates() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return false;
  if (initialDataLoaded) return true;
  if (lastInitialReadAttemptMs != 0 &&
      millis() - lastInitialReadAttemptMs < INITIAL_READ_RETRY_INTERVAL_MS) return false;

  lastInitialReadAttemptMs = millis();
  Serial.println("Membaca status relay dari Firebase...");

  bool values[RELAY_COUNT];
  char path[64];
  for (int i = 0; i < RELAY_COUNT; i++) {
    buildRelayPath(i, path, sizeof(path));
    if (!Firebase.RTDB.getBool(&firebaseData, path)) {
      Serial.printf("Gagal baca relay%d: %s\n", i + 1, firebaseData.errorReason().c_str());
      return false;
    }
    values[i] = firebaseData.boolData();
    feedWatchdog();
  }

  for (int i = 0; i < RELAY_COUNT; i++) {
    applyRelayState(i, values[i]);
  }

  initialDataLoaded = true;
  Serial.println("Status relay dimuat dari Firebase.");
  return true;
}

// Watchdog tidak perlu di-reset pada setiap iterasi CPU.
// Cukup periodik agar overhead kecil tetapi tetap aman.
void feedWatchdog() {
  if (millis() - lastWatchdogFeedMs >= 1000) {
    esp_task_wdt_reset();
    lastWatchdogFeedMs = millis();
  }
}

void setup() {
  Serial.begin(115200);

  Serial.println();
  Serial.println("ESP32 Relay Firebase Controller");
  Serial.printf("Reset reason: %s\n", resetReasonText(esp_reset_reason()));

  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);

  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  preferencesReady = preferences.begin(PREFS_NS, false);
  if (!preferencesReady) {
    Serial.println("WARNING: Preferences gagal dibuka, state relay tidak akan dipersist.");
  }

  deviceInitialized = preferencesReady ? preferences.getBool("init", false) : false;

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

// Satu siklus kerja utama controller.
// Fungsi ini dipisah dari `loop()` agar alur state machine lebih mudah dibaca.
void runControllerCycle() {
  feedWatchdog();
  maybeSaveRelayStates();
  ensureWiFiConnected();

  if (WiFi.status() != WL_CONNECTED) {
    maybeLogDiagnostics();
    return;
  }

  ensureFirebaseReady();

  if (!Firebase.ready()) {
    maybeLogDiagnostics();
    return;
  }

  if (!deviceInitialized) {
    if (!initializeDeviceInFirebase()) {
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

  maybeLogDiagnostics();
}

void loop() {
  // Scheduler sederhana berbasis millis().
  // Dengan pola ini loop tetap berputar cepat, tetapi pekerjaan utama
  // hanya dijalankan setiap 50 ms tanpa memblokir CPU memakai `delay()`.
  feedWatchdog();

  if (lastMainLoopRunMs != 0 && millis() - lastMainLoopRunMs < MAIN_LOOP_INTERVAL_MS) {
    return;
  }

  lastMainLoopRunMs = millis();
  runControllerCycle();
}
