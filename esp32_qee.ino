/*
  Projeto didático de proteção de rede + medição de tensão com ESP32
  Framework: Arduino-ESP32
  Bibliotecas: PubSubClient (Nick O'Leary), ArduinoJson (Benoit Blanchon)

  IMPORTANTE:
  - Não conecte tensão da rede elétrica diretamente ao ESP32.
  - Use fonte isolada de baixa tensão e condicionamento analógico apropriado.
  - GPIOs e polaridade dos módulos de relé devem ser conferidos na sua placa.
  - Este é um simulador didático, não um relé de proteção certificado.
*/
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>

const char* WIFI_SSID = "SEU_WIFI";
const char* WIFI_PASS = "SUA_SENHA";
const char* MQTT_HOST = "192.168.1.100"; // IP do computador com Mosquitto
const uint16_t MQTT_PORT = 1883;
const char* DEVICE_ID = "esp32-qee-01";

const int PIN_R1 = 25;
const int PIN_R2 = 26;
const int PIN_R3 = 27;
const int PIN_VOLTAGE = 34; // ADC1 no ESP32 clássico

// Ajuste conforme módulo: muitos relés são ativos em LOW.
const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

// Calibração didática: Vmedido = (ADC - offset) * voltsPorContagem.
// Ajuste com multímetro e circuito condicionador. Não é calibração metrológica.
const float ADC_COUNTS_PER_VOLT_INPUT = 250.0f; // EXEMPLO: counts por volt real, ajustar
const float ADC_OFFSET_COUNTS = 2048.0f;        // sinal AC centrado em meia escala
const float VOLTAGE_SCALE = 1.0f;               // escala do condicionador, ajustar
const uint32_t SAMPLE_PERIOD_US = 1000;         // 1 kHz para visualização didática
const uint32_t WAVE_PUBLISH_MS = 100;           // lote curto de amostras para gráfico
const int WAVE_BATCH = 20;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool r1Closed = true;  // true = energizado/contato fechado na abstração
volatile bool r2Closed = true;
volatile bool r3Closed = true;
volatile bool injectFailR2 = false;
volatile bool injectFailR3 = false;
volatile bool faultActiveR2 = false;
volatile bool faultActiveR3 = false;
volatile bool waveEnabled = true;

volatile uint32_t faultStartR1 = 0;
volatile uint32_t faultStartR2 = 0;
volatile uint32_t faultStartR3 = 0;
volatile bool faultActiveR1 = false;
volatile uint32_t lastSampleUs = 0;

float waveBuffer[WAVE_BATCH];
int waveCount = 0;
uint32_t lastWavePublish = 0;
uint32_t lastStatusPublish = 0;
uint32_t lastReconnectAttempt = 0;

String topic(const char* suffix) {
  return String("qee/") + DEVICE_ID + "/" + suffix;
}

void publishJson(const String& t, JsonDocument& doc, bool retained=false) {
  if (!mqtt.connected()) return;
  char payload[1024];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(t.c_str(), (const uint8_t*)payload, n, retained);
}

void publishEvent(const char* eventType, const char* relay, const char* reason,
                  uint32_t durationMs=0) {
  StaticJsonDocument<384> doc;
  doc["device"] = DEVICE_ID;
  doc["event"] = eventType;
  doc["relay"] = relay;
  doc["reason"] = reason;
  doc["duration_ms"] = durationMs;
  doc["uptime_ms"] = millis();
  doc["timestamp_ms"] = (uint64_t)millis(); // uptime; backend adiciona timestamp civil
  publishJson(topic("events"), doc, false);
}

void applyOutputs() {
  // A proteção lógica considera R1 como disjuntor geral e R2/R3 como ramais.
  digitalWrite(PIN_R1, r1Closed ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_R2, (r1Closed && r2Closed) ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_R3, (r1Closed && r3Closed) ? RELAY_ON : RELAY_OFF);
}

void publishStatus() {
  StaticJsonDocument<512> doc;
  doc["device"] = DEVICE_ID;
  doc["r1_closed"] = (bool)r1Closed;
  doc["r2_closed"] = (bool)r2Closed;
  doc["r3_closed"] = (bool)r3Closed;
  doc["fault_r2"] = (bool)faultActiveR2;
  doc["fault_r3"] = (bool)faultActiveR3;
  doc["inject_fail_r2"] = (bool)injectFailR2;
  doc["inject_fail_r3"] = (bool)injectFailR3;
  doc["uptime_ms"] = millis();
  publishJson(topic("status"), doc, true);
}

void tripBranch(int branch, const char* reason) {
  uint32_t now = millis();
  if (branch == 2) {
    if (injectFailR2) {
      // R2 falha em abrir; o disjuntor geral R1 abre como retaguarda.
      r1Closed = false;
      faultActiveR1 = true;
      faultStartR1 = now;
      applyOutputs();
      publishEvent("trip", "R1", "falha de atuação R2; proteção de retaguarda", 0);
    } else {
      r2Closed = false;
      faultActiveR2 = true;
      faultStartR2 = now;
      applyOutputs();
      publishEvent("trip", "R2", reason, 0);
    }
  } else if (branch == 3) {
    if (injectFailR3) {
      r1Closed = false;
      faultActiveR1 = true;
      faultStartR1 = now;
      applyOutputs();
      publishEvent("trip", "R1", "falha de atuação R3; proteção de retaguarda", 0);
    } else {
      r3Closed = false;
      faultActiveR3 = true;
      faultStartR3 = now;
      applyOutputs();
      publishEvent("trip", "R3", reason, 0);
    }
  }
  publishStatus();
}

void restoreAll() {
  uint32_t now = millis();
  if (faultActiveR1) {
    publishEvent("restore", "R1", "religamento comandado após atuação de retaguarda", now - faultStartR1);
    faultActiveR1 = false;
  }
  if (faultActiveR2) {
    publishEvent("restore", "R2", "religamento comandado", now - faultStartR2);
    faultActiveR2 = false;
  }
  if (faultActiveR3) {
    publishEvent("restore", "R3", "religamento comandado", now - faultStartR3);
    faultActiveR3 = false;
  }
  r1Closed = r2Closed = r3Closed = true;
  applyOutputs();
  publishEvent("restore", "ALL", "religamento geral", 0);
  publishStatus();
}

void mqttCallback(char* topicRaw, byte* payload, unsigned int length) {
  String receivedTopic(topicRaw);
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length)) return;

  String cmd = doc["cmd"] | "";
  String faultType = doc["fault_type"] | "genérica";
  String network = doc["network"] | "R2";
  bool protectionWorks = doc["protection_works"] | true;

  if (cmd == "simulate_fault") {
    if (network == "R2") {
      injectFailR2 = !protectionWorks;
      tripBranch(2, faultType.c_str());
    } else if (network == "R3") {
      injectFailR3 = !protectionWorks;
      tripBranch(3, faultType.c_str());
    }
  } else if (cmd == "restore_all") {
    injectFailR2 = injectFailR3 = false;
    restoreAll();
  } else if (cmd == "reset_fail_injection") {
    injectFailR2 = injectFailR3 = false;
    publishStatus();
  } else if (cmd == "set_relay") {
    // Comando manual somente para demonstração. Mantém intertravamento lógico.
    String relay = doc["relay"] | "";
    bool close = doc["closed"] | true;
    if (relay == "R1") {
      r1Closed = close;
      if (!close) {
        r2Closed = false; r3Closed = false;
        if (!faultActiveR1) { faultActiveR1 = true; faultStartR1 = millis(); }
      } else {
        if (faultActiveR1) {
          publishEvent("restore", "R1", "religamento manual", millis()-faultStartR1);
          faultActiveR1 = false;
        }
        r2Closed = true; r3Closed = true;
      }
    } else if (relay == "R2" && r1Closed) {
      r2Closed = close;
      if (!close) { faultActiveR2 = true; faultStartR2 = millis(); }
      else if (faultActiveR2) {
        publishEvent("restore", "R2", "religamento manual", millis()-faultStartR2);
        faultActiveR2 = false;
      }
    } else if (relay == "R3" && r1Closed) {
      r3Closed = close;
      if (!close) { faultActiveR3 = true; faultStartR3 = millis(); }
      else if (faultActiveR3) {
        publishEvent("restore", "R3", "religamento manual", millis()-faultStartR3);
        faultActiveR3 = false;
      }
    }
    applyOutputs();
    publishStatus();
  }
}

void connectMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastReconnectAttempt < 2000) return;
  lastReconnectAttempt = millis();
  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(clientId.c_str())) {
    mqtt.subscribe(topic("cmd").c_str());
    publishStatus();
    publishEvent("online", "ESP32", "conectado ao broker", 0);
  }
}

void voltageTask(void* parameter) {
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    if (waveEnabled) {
      uint32_t nowUs = micros();
      if ((uint32_t)(nowUs - lastSampleUs) >= SAMPLE_PERIOD_US) {
        lastSampleUs = nowUs;
        int raw = analogRead(PIN_VOLTAGE);
        // Converte em tensão aproximada de entrada do condicionador.
        float v = ((float)raw - ADC_OFFSET_COUNTS) / ADC_COUNTS_PER_VOLT_INPUT;
        v *= VOLTAGE_SCALE;
        if (waveCount < WAVE_BATCH) waveBuffer[waveCount++] = v;
      }
      if (waveCount >= WAVE_BATCH && millis() - lastWavePublish >= WAVE_PUBLISH_MS) {
        lastWavePublish = millis();
        StaticJsonDocument<768> doc;
        doc["device"] = DEVICE_ID;
        doc["sample_rate_hz"] = 1000000.0f / SAMPLE_PERIOD_US;
        JsonArray a = doc.createNestedArray("samples");
        for (int i=0; i<waveCount; i++) a.add(waveBuffer[i]);
        publishJson(topic("wave"), doc, false);
        waveCount = 0;
      }
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_R1, OUTPUT); pinMode(PIN_R2, OUTPUT); pinMode(PIN_R3, OUTPUT);
  digitalWrite(PIN_R1, RELAY_ON);
  digitalWrite(PIN_R2, RELAY_ON);
  digitalWrite(PIN_R3, RELAY_ON);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VOLTAGE, ADC_11db);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);

  // Arduino-ESP32 clássico normalmente tem 2 núcleos; no core 0 fica a amostragem.
  xTaskCreatePinnedToCore(voltageTask, "voltageTask", 4096, nullptr, 2, nullptr, 0);
  applyOutputs();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    connectMqtt();
    mqtt.loop();
  }
  if (millis() - lastStatusPublish >= 1000) {
    lastStatusPublish = millis();
    publishStatus();
  }
  delay(2);
}
