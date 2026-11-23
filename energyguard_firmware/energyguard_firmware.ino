/*
 * EnergyGuard - Firmware do ESP32 (v2)
 *
 * Sensores:
 *   - SCT013-000 (100 A : 50 mA) com resistor de burden de 33 ohms -> GPIO34
 *   - ZMPT101B (saida analogica)                                    -> GPIO35
 *   Os dois sinais tem bias de ~1,65 V (divisor 2x10k + capacitor).
 *
 * Arquitetura (duas tarefas do FreeRTOS, uma em cada nucleo):
 *   - measureTask (nucleo 1): amostra os dois canais em janelas de 0,5 s, calcula Vrms, Irms,
 *     potencia, fator de potencia e frequencia, integra a energia (kWh) e, a cada
 *     SEND_INTERVAL_MS, coloca uma media numa fila. Nunca espera pela rede.
 *   - netTask (nucleo 0): cuida do Wi-Fi, do horario (NTP) e envia a fila em lotes por HTTP(S).
 *     Se a rede ou a API cair, as leituras ficam na fila (ate QUEUE_LEN) e seguem depois,
 *     com o horario original da medicao.
 *   - Watchdog: se qualquer tarefa travar, a placa reinicia. Sem Wi-Fi por muito tempo, tambem.
 *
 * Arquivos desta pasta: energyguard_firmware.ino e energy_math.h (devem ficar juntos).
 * Bibliotecas: so as que ja vem com o core ESP32 (nao precisa instalar ArduinoJson).
 * Feito para Arduino-ESP32 2.x e 3.x (confira ao compilar).
 *
 * CALIBRACAO pelo Monitor Serial (115200, "Nova linha"):
 *   v127.5  -> informa a tensao real medida com multimetro (calibra o ZMPT101B)
 *   i3.20   -> informa a corrente real (calibra o SCT013)
 *   z       -> zera a energia acumulada
 *   h       -> ajuda
 */

#include <Arduino.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "energy_math.h"

// ======================= CONFIGURACAO =======================
const char* WIFI_SSID = "CLARO_2G957672";
const char* WIFI_PASS = "658493drikagg";

// Endereco da API SEM o caminho. Rede local: "http://192.168.0.10:8000".
// Internet: "https://seu-servico.onrender.com".
const char* API_BASE = "https://api-energyguardv2.onrender.com";
const char* API_KEY  = "";            // igual ao API_KEY do backend (vazio = sem chave)
const char* DEVICE_ID = "esp32-01";   // use so letras, numeros, - e _

// HTTPS: com API_ROOT_CA = nullptr a conexao e criptografada, mas o servidor NAO e verificado.
// Para verificar, cole aqui o certificado raiz do servidor (PEM) como texto.
const char* API_ROOT_CA = nullptr;

const uint8_t PIN_CURRENT = 34;       // SCT013 (ADC1, so entrada)
const uint8_t PIN_VOLTAGE = 35;       // ZMPT101B OUT (ADC1, so entrada)

const float SCT_RATIO   = 2000.0f;    // 100 A / 50 mA
const float BURDEN_OHMS = 33.0f;
const float WIRE_TURNS  = 1.0f;       // voltas do fio dentro da garra (divide a leitura)
const float CURRENT_CAL_DEFAULT = SCT_RATIO / (BURDEN_OHMS * 1000.0f * WIRE_TURNS);
const float VOLTAGE_CAL_DEFAULT = 0.25f;

const uint32_t WINDOW_US        = 500000UL;   // janela de medida (0,5 s = 30 ciclos a 60 Hz)
const uint16_t MAX_SAMPLES      = 10000;
const uint32_t SEND_INTERVAL_MS = 5000;       // intervalo entre leituras enviadas
const uint32_t BIAS_SETTLE_MS   = 20000;      // capacitor grande demora a estabilizar
const uint32_t ENERGY_SAVE_MS   = 300000;
const float    MIN_VOLTAGE_V    = 20.0f;
const float    MIN_CURRENT_A    = 0.15f;

const uint16_t QUEUE_LEN        = 720;        // 1 h de leituras a cada 5 s
const uint8_t  BATCH_MAX        = 20;         // leituras por requisicao
const uint32_t WDT_TIMEOUT_S    = 40;
const uint32_t WIFI_REBOOT_MS   = 600000UL;   // reinicia se ficar 10 min sem Wi-Fi
const uint32_t MIN_FREE_HEAP    = 20000;
// ============================================================

const uint32_t MIN_VALID_EPOCH = 1700000000UL;
const size_t   BODY_CAP        = 6144;

static uint16_t vBuf[MAX_SAMPLES];
static uint16_t iBuf[MAX_SAMPLES];
static char     bodyBuf[BODY_CAP];

struct Sample {
  uint32_t epoch;   // horario UTC da medicao (0 = ainda sem NTP)
  uint32_t tMs;     // millis() da medicao
  float v, i, p, pf, f;
  float energyKwh;
};

struct Accum {
  double v = 0, i = 0, p = 0, pf = 0, f = 0;
  uint16_t n = 0, nf = 0;
};

Preferences prefs;
SemaphoreHandle_t prefsMutex;
QueueHandle_t sampleQueue;

volatile float voltageCal = VOLTAGE_CAL_DEFAULT;
volatile float currentCal = CURRENT_CAL_DEFAULT;
volatile float lastVrmsMv = 0, lastIrmsMv = 0;
volatile bool  resetEnergyRequested = false;
volatile uint32_t sendFailures = 0;
volatile uint32_t droppedSamples = 0;
volatile uint32_t wifiDownSince = 0;
double energyKwh = 0.0;
bool voltageCalibrated = false;

// ---------------------------------------------------------------
static uint32_t currentEpoch() {
  const time_t t = time(nullptr);
  return t > (time_t)MIN_VALID_EPOCH ? (uint32_t)t : 0;
}

static void watchdogInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg;
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
}

static void savePref(const char* key, float value) {
  xSemaphoreTake(prefsMutex, portMAX_DELAY);
  prefs.putFloat(key, value);
  xSemaphoreGive(prefsMutex);
}

static void saveEnergy() {
  xSemaphoreTake(prefsMutex, portMAX_DELAY);
  prefs.putDouble("kwh", energyKwh);
  xSemaphoreGive(prefsMutex);
}

// ---------------------------------------------------------------
static void enqueueSample(const Sample& s) {
  if (xQueueSend(sampleQueue, &s, 0) == pdTRUE) return;
  Sample oldest;
  xQueueReceive(sampleQueue, &oldest, 0);
  xQueueSend(sampleQueue, &s, 0);
  droppedSamples++;
  Serial.printf("[fila] cheia: descartei a leitura mais antiga (%lu no total)\n", (unsigned long)droppedSamples);
}

static bool takeWindow(Metrics& out) {
  uint16_t n = 0;
  const uint32_t t0 = micros();
  while (n < MAX_SAMPLES && (micros() - t0) < WINDOW_US) {
    vBuf[n] = (uint16_t)analogReadMilliVolts(PIN_VOLTAGE);
    iBuf[n] = (uint16_t)analogReadMilliVolts(PIN_CURRENT);
    n++;
  }
  const uint32_t elapsedUs = micros() - t0;
  out = computeMetrics(vBuf, iBuf, n, elapsedUs, voltageCal, currentCal, MIN_VOLTAGE_V, MIN_CURRENT_A);
  return out.valid;
}

static void measureTask(void*) {
  esp_task_wdt_add(NULL);

  uint32_t lastMsg = 0;
  while (millis() < BIAS_SETTLE_MS) {
    esp_task_wdt_reset();
    if (millis() - lastMsg > 5000) {
      lastMsg = millis();
      Serial.printf("[bias] estabilizando... %u s\n", (unsigned)((BIAS_SETTLE_MS - millis()) / 1000));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  Accum acc;
  uint32_t lastEnergyMs = millis(), lastSaveMs = millis(), lastPushMs = millis();

  for (;;) {
    esp_task_wdt_reset();

    Metrics m;
    if (!takeWindow(m)) {
      Serial.println("[medida] amostras insuficientes");
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    lastVrmsMv = m.vrmsMv;
    lastIrmsMv = m.irmsMv;

    const uint32_t now = millis();
    if (resetEnergyRequested) {
      energyKwh = 0;
      resetEnergyRequested = false;
      saveEnergy();
      Serial.println("[energia] zerada");
    }
    energyKwh += (double)m.power * (double)(now - lastEnergyMs) / 3.6e9;
    lastEnergyMs = now;

    acc.v += m.vrms; acc.i += m.irms; acc.p += m.power; acc.pf += m.pf; acc.n++;
    if (m.freq > 0) { acc.f += m.freq; acc.nf++; }

    Serial.printf("V=%.1f V  I=%.3f A  P=%.1f W  FP=%.2f  f=%.2f Hz  | bruto: %.1f mV / %.1f mV (%u amostras)\n",
                  m.vrms, m.irms, m.power, m.pf, m.freq, m.vrmsMv, m.irmsMv, m.samples);

    if (now - lastPushMs >= SEND_INTERVAL_MS && acc.n > 0) {
      Sample s;
      s.epoch = currentEpoch();
      s.tMs = now;
      s.v = (float)(acc.v / acc.n);
      s.i = (float)(acc.i / acc.n);
      s.p = (float)(acc.p / acc.n);
      s.pf = (float)(acc.pf / acc.n);
      s.f = acc.nf ? (float)(acc.f / acc.nf) : 0.0f;
      s.energyKwh = (float)energyKwh;
      enqueueSample(s);
      acc = Accum();
      lastPushMs = now;
    }

    if (now - lastSaveMs >= ENERGY_SAVE_MS) {
      lastSaveMs = now;
      saveEnergy();
    }

    vTaskDelay(1);
  }
}

// ---------------------------------------------------------------
static int buildBody(const Sample* batch, int n, uint32_t nowEpoch, uint32_t nowMs) {
  int w = snprintf(bodyBuf, BODY_CAP, "{\"readings\":[");
  if (w < 0) return 0;
  size_t len = (size_t)w;

  const int rssi = WiFi.RSSI();
  const unsigned long uptime = millis() / 1000;
  int included = 0;

  for (int k = 0; k < n; k++) {
    uint32_t epoch = batch[k].epoch;
    if (epoch == 0 && nowEpoch != 0) {
      const uint32_t ageS = (nowMs - batch[k].tMs) / 1000;
      epoch = nowEpoch > ageS ? nowEpoch - ageS : 0;
    }
    char ts[32] = "";
    if (epoch != 0) snprintf(ts, sizeof(ts), ",\"measured_ts\":%lu", (unsigned long)epoch);

    w = snprintf(bodyBuf + len, BODY_CAP - len,
                 "%s{\"device_id\":\"%s\",\"voltage\":%.2f,\"current\":%.3f,\"power\":%.2f,"
                 "\"energy\":%.4f,\"frequency\":%.2f,\"power_factor\":%.3f,"
                 "\"rssi\":%d,\"uptime_s\":%lu,\"send_failures\":%lu%s}",
                 k ? "," : "", DEVICE_ID, batch[k].v, batch[k].i, batch[k].p, batch[k].energyKwh,
                 batch[k].f, batch[k].pf, rssi < 0 ? rssi : -1, uptime, (unsigned long)sendFailures, ts);
    if (w < 0 || (size_t)w >= BODY_CAP - len - 4) break;
    len += (size_t)w;
    included++;
  }
  snprintf(bodyBuf + len, BODY_CAP - len, "]}");
  return included;
}

static int postBatch(size_t bodyLen) {
  const String url = String(API_BASE) + "/api/readings/batch";
  WiFiClient plain;
  WiFiClientSecure secure;
  WiFiClient* client = &plain;
  if (url.startsWith("https://")) {
    if (API_ROOT_CA) secure.setCACert(API_ROOT_CA);
    else secure.setInsecure();
    client = &secure;
  }

  HTTPClient http;
  const bool https = url.startsWith("https://");
  http.setConnectTimeout(https ? 8000 : 5000);
  http.setTimeout(https ? 15000 : 8000);
  if (!http.begin(*client, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  if (strlen(API_KEY) > 0) http.addHeader("X-API-Key", API_KEY);
  const int code = http.POST((uint8_t*)bodyBuf, bodyLen);
  http.end();
  return code;
}

static void waitWithWatchdog(uint32_t ms) {
  for (uint32_t waited = 0; waited < ms; waited += 500) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

static void netTask(void*) {
  esp_task_wdt_add(NULL);

  uint32_t lastWifiTry = 0, backoffMs = 2000;
  bool sntpStarted = false;
  static Sample batch[BATCH_MAX];

  for (;;) {
    esp_task_wdt_reset();

    if (WiFi.status() != WL_CONNECTED) {
      if (wifiDownSince == 0) wifiDownSince = millis();
      if (millis() - lastWifiTry > 10000) {
        lastWifiTry = millis();
        Serial.println("[wifi] reconectando...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    wifiDownSince = 0;

    if (!sntpStarted) {
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      sntpStarted = true;
    }

    int n = 0;
    while (n < BATCH_MAX && xQueueReceive(sampleQueue, &batch[n], 0) == pdTRUE) n++;
    if (n == 0) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    const int included = buildBody(batch, n, currentEpoch(), millis());
    int code = -1;
    if (included > 0) code = postBatch(strlen(bodyBuf));

    if (code >= 200 && code < 300) {
      for (int k = n - 1; k >= included; k--) xQueueSendToFront(sampleQueue, &batch[k], 0);
      Serial.printf("[http] %d OK: %d leitura(s) enviada(s), %u na fila\n", code, included,
                    (unsigned)uxQueueMessagesWaiting(sampleQueue));
      backoffMs = 2000;
      continue;
    }

    sendFailures++;
    const bool poison = (code == 400 || code == 413 || code == 422);
    if (poison) {
      Serial.printf("[http] erro %d: lote rejeitado pela API, descartando %d leitura(s)\n", code, n);
    } else {
      Serial.printf("[http] erro %d: mantendo %d leitura(s) na fila, nova tentativa em %lu s\n", code, n,
                    (unsigned long)(backoffMs / 1000));
      for (int k = n - 1; k >= 0; k--) xQueueSendToFront(sampleQueue, &batch[k], 0);
    }
    waitWithWatchdog(backoffMs);
    backoffMs = backoffMs * 2 > 60000 ? 60000 : backoffMs * 2;
  }
}

// ---------------------------------------------------------------
static void printHelp() {
  Serial.println("Comandos: v<volts> calibra tensao | i<amperes> calibra corrente | z zera energia | h ajuda");
}

static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  const char cmd = tolower(line.charAt(0));
  const float val = line.substring(1).toFloat();

  if (cmd == 'v' && val > 0 && lastVrmsMv > 0) {
    voltageCal = val / lastVrmsMv;
    voltageCalibrated = true;
    savePref("vcal", voltageCal);
    Serial.printf("[cal] tensao: %.4f V/mV (RMS bruto %.1f mV)\n", (float)voltageCal, (float)lastVrmsMv);
  } else if (cmd == 'i' && val > 0 && lastIrmsMv > 0) {
    currentCal = val / lastIrmsMv;
    savePref("ical", currentCal);
    Serial.printf("[cal] corrente: %.5f A/mV (RMS bruto %.1f mV)\n", (float)currentCal, (float)lastIrmsMv);
  } else if (cmd == 'z') {
    resetEnergyRequested = true;
  } else if (cmd == 'h') {
    printHelp();
  } else {
    Serial.println("Comando invalido (ou sem leitura valida ainda).");
    printHelp();
  }
}

static void supervise() {
  static uint32_t lowHeapSince = 0;
  if (wifiDownSince != 0 && millis() - wifiDownSince > WIFI_REBOOT_MS) {
    Serial.println("[watchdog] sem Wi-Fi ha muito tempo, reiniciando");
    saveEnergy();
    delay(200);
    ESP.restart();
  }
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    if (lowHeapSince == 0) lowHeapSince = millis();
    if (millis() - lowHeapSince > 60000) {
      Serial.println("[watchdog] pouca memoria livre, reiniciando");
      saveEnergy();
      delay(200);
      ESP.restart();
    }
  } else {
    lowHeapSince = 0;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(500);
  Serial.println("\n=== EnergyGuard ESP32 v2 ===");

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CURRENT, ADC_11db);
  analogSetPinAttenuation(PIN_VOLTAGE, ADC_11db);

  prefsMutex = xSemaphoreCreateMutex();
  prefs.begin("energyguard", false);
  voltageCalibrated = prefs.isKey("vcal");
  voltageCal = prefs.getFloat("vcal", VOLTAGE_CAL_DEFAULT);
  currentCal = prefs.getFloat("ical", CURRENT_CAL_DEFAULT);
  energyKwh = prefs.getDouble("kwh", 0.0);

  Serial.printf("Calibracao: tensao %.4f V/mV%s | corrente %.5f A/mV | energia %.4f kWh\n",
                (float)voltageCal, voltageCalibrated ? "" : " (PADRAO - calibre com o comando v)",
                (float)currentCal, energyKwh);
  printHelp();

  sampleQueue = xQueueCreate(QUEUE_LEN, sizeof(Sample));
  if (sampleQueue == NULL) {
    Serial.println("Sem memoria para a fila, reiniciando");
    delay(1000);
    ESP.restart();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  watchdogInit();
  xTaskCreatePinnedToCore(measureTask, "measure", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 1, NULL, 0);
}

void loop() {
  handleSerial();
  supervise();
  delay(20);
}
