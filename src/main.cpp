#include <WiFi.h>
#include <PubSubClient.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <PID_v1.h>
#include <SPIFFS.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include <esp_pm.h>
#include <esp32/pm.h>
#include <ESPmDNS.h>

// === КОНФИГУРАЦИЯ ДЛЯ WEMOS S2 MINI ===
#define WIFI_SSID "Your_WiFi_SSID"
#define WIFI_PASS "Your_WiFi_Password"
#define MQTT_SERVER "192.168.1.10"
#define MQTT_PORT 1883
#define MQTT_CLIENT_ID "wemos_s2_heating_01"

// GPIO для Wemos S2 Mini:
#define SSR_PIN_A 20    // Контур A (не занят SPI, подходит для PWM)
#define SSR_PIN_B 21    // Контур B (не занят SPI, подходит для PWM)
#define ONE_WIRE_BUS 19 // DS18B20 (GPIO19 свободен)
#define LED_PIN 15      // Встроенная светодиодная лампа (GPIO15)

// ПИД-параметры
volatile double Kp = 15.0;
volatile double Ki = 0.05;
volatile double Kd = 2.0;

// Температурные лимиты
#define MAX_SAFE_TEMP 35.0
#define MIN_SETPOINT 15.0
#define MAX_SETPOINT 30.0
#define TEMP_HYSTERESIS 0.2

// PWM настройки
#define PWM_CHANNEL_A 0
#define PWM_CHANNEL_B 1
#define PWM_FREQ 0.5f        // 0.5 Гц = 2 сек цикл
#define PWM_BITS 8           // 0–255
#define PWM_SMOOTHING_STEP 5 // Шаг плавного изменения

// Тайминги
#define WATCHDOG_TIMEOUT 30
#define TEMP_READ_RETRIES 3
#define WIFI_RECONNECT_INTERVAL 30000
#define DIAGNOSTICS_INTERVAL 60000

// Общие параметры
const TickType_t UPDATE_INTERVAL = pdMS_TO_TICKS(5000);
const TickType_t PWM_UPDATE_INTERVAL = pdMS_TO_TICKS(100);
float lastOutputA = 0.0;
float lastOutputB = 0.0;
float maxChangePerCycle = 2.0f;

// === СТРУКТУРЫ ДАННЫХ ===
struct SystemState
{
    double currentTemp;
    double SetpointA;
    double SetpointB;
    double OutputA;
    double OutputB;
    bool systemEnabled;
    bool emergencyStop;
    String status;
    uint32_t lastSuccessfulTempRead;
    bool temperatureSensorFault;
    bool pidAActive;
    bool pidBActive;
};

struct SystemDiagnostics
{
    uint32_t systemUptime;
    uint32_t wifiDisconnects;
    uint32_t mqttDisconnects;
    uint32_t tempReadErrors;
    uint32_t pidCalculations;
    uint32_t pidCalculationFailures;
    String lastResetReason;
    String lastError;
    uint32_t freeHeap;
};

SystemState state = {0.0, 22.0, 22.0, 0.0, 0.0, true, false, "INIT", 0, false};
SystemDiagnostics diagnostics = {0, 0, 0, 0, 0, 0, "", "", 0};

// === СИНХРОНИЗАЦИЯ ===
SemaphoreHandle_t xMutexState;
QueueHandle_t xQueueTemp;
portMUX_TYPE pidMux = portMUX_INITIALIZER_UNLOCKED;

// === MQTT ===
WiFiClient espClient;
PubSubClient mqttClient(espClient);
uint32_t wifiLastReconnectAttempt = 0;

const char *TOPIC_SETPOINT_A = "homeassistant/climate/heating_zone_a/setpoint";
const char *TOPIC_ENABLED_A = "homeassistant/climate/heating_zone_a/enabled";
const char *TOPIC_SETPOINT_B = "homeassistant/climate/heating_zone_b/setpoint";
const char *TOPIC_ENABLED_B = "homeassistant/climate/heating_zone_b/enabled";
const char *TOPIC_CURRENT_TEMP = "homeassistant/sensor/heating_temp/state";
const char *TOPIC_TARGET_TEMP_A = "homeassistant/sensor/heating_target_a/state";
const char *TOPIC_TARGET_TEMP_B = "homeassistant/sensor/heating_target_b/state";
const char *TOPIC_POWER_A = "homeassistant/sensor/power_zone_a/state";
const char *TOPIC_POWER_B = "homeassistant/sensor/power_zone_b/state";
const char *TOPIC_STATUS = "homeassistant/sensor/heating_status/state";
const char *TOPIC_PID_KP = "homeassistant/climate/heating_zone_a/pid_kp";
const char *TOPIC_PID_KI = "homeassistant/climate/heating_zone_a/pid_ki";
const char *TOPIC_PID_KD = "homeassistant/climate/heating_zone_a/pid_kd";
const char *TOPIC_DIAGNOSTICS = "homeassistant/sensor/heating_diagnostics/state";
const char *TOPIC_EMERGENCY = "homeassistant/climate/heating_control/emergency";

// === ОБЪЕКТЫ ===
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
PID myPID_A(&state.currentTemp, &state.OutputA, &state.SetpointA, Kp, Ki, Kd, DIRECT);
PID myPID_B(&state.currentTemp, &state.OutputB, &state.SetpointB, Kp, Ki, Kd, DIRECT);

// === OTA ===
const char *OTA_PASSWORD = "your_ota_password_123";
String otaHostname = "wemos-s2-heating";

// === ФУНКЦИИ ===
void setupPWM();
void loadPIDParams();
void savePIDParams();
float readTemperature();
void connectToWiFi();
void publishStatus();
void publishDiagnostics();
void emergencyShutdown(String reason);
void resetEmergency();
void setCpuFrequencyCustom(uint8_t freq);
void setupOTA();
const char *resetReasonToString(esp_reset_reason_t reason);

TaskHandle_t hReadTemp, hCalcPID, hControlPWM, hMQTT, hOTA;

void setupPWM()
{
    ledcSetup(PWM_CHANNEL_A, PWM_FREQ, PWM_BITS);
    ledcSetup(PWM_CHANNEL_B, PWM_FREQ, PWM_BITS);

    ledcAttachPin(SSR_PIN_A, PWM_CHANNEL_A);
    ledcAttachPin(SSR_PIN_B, PWM_CHANNEL_B);

    ledcWrite(PWM_CHANNEL_A, 0);
    ledcWrite(PWM_CHANNEL_B, 0);

    Serial.println("✅ Два канала PWM инициализированы для Wemos S2 Mini");
}

void loadPIDParams()
{
    if (!SPIFFS.begin(true))
    {
        Serial.println("❌ Не удалось смонтировать SPIFFS");
        return;
    }

    if (!SPIFFS.exists("/pid_params.txt"))
    {
        Serial.println("⚠️ Файл параметров не найден, используем значения по умолчанию");
        return;
    }

    File file = SPIFFS.open("/pid_params.txt", "r");
    if (!file)
    {
        Serial.println("❌ Ошибка открытия файла параметров");
        return;
    }

    String line = file.readStringUntil('\n');
    file.close();

    int comma1 = line.indexOf(',');
    int comma2 = line.lastIndexOf(',');

    if (comma1 == -1 || comma2 == -1)
    {
        Serial.println("❌ Неверный формат файла параметров");
        return;
    }

    portENTER_CRITICAL(&pidMux);
    Kp = line.substring(0, comma1).toFloat();
    Ki = line.substring(comma1 + 1, comma2).toFloat();
    Kd = line.substring(comma2 + 1).toFloat();
    myPID_A.SetTunings(Kp, Ki, Kd);
    myPID_B.SetTunings(Kp, Ki, Kd);
    portEXIT_CRITICAL(&pidMux);

    Serial.printf("📂 Параметры ПИД загружены: Kp=%.1f Ki=%.2f Kd=%.1f\n", Kp, Ki, Kd);
}

void savePIDParams()
{
    File file = SPIFFS.open("/pid_params.txt", "w");
    if (!file)
    {
        Serial.println("❌ Не удалось сохранить параметры в SPIFFS");
        return;
    }

    portENTER_CRITICAL(&pidMux);
    file.printf("%.1f,%.2f,%.1f\n", Kp, Ki, Kd);
    portEXIT_CRITICAL(&pidMux);

    file.close();
    Serial.println("💾 Параметры ПИД сохранены в SPIFFS");
}

float readTemperature()
{
    for (int attempt = 0; attempt < TEMP_READ_RETRIES; attempt++)
    {
        sensors.requestTemperatures();
        float temp = sensors.getTempCByIndex(0);

        if (temp != DEVICE_DISCONNECTED_C && temp >= -20 && temp <= 60)
        {
            xSemaphoreTake(xMutexState, portMAX_DELAY);
            state.temperatureSensorFault = false;
            state.lastSuccessfulTempRead = millis();
            xSemaphoreGive(xMutexState);
            return temp;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    xSemaphoreTake(xMutexState, portMAX_DELAY);
    state.temperatureSensorFault = true;
    diagnostics.tempReadErrors++;
    xSemaphoreGive(xMutexState);

    Serial.println("❌ Ошибка чтения датчика температуры!");
    return state.currentTemp;
}

void connectToWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return;

    Serial.printf("📡 Подключение к WiFi %s", WIFI_SSID);

    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setSleep(true);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 15)
    {
        delay(1000);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\n✅ WiFi подключен");
        Serial.printf("IP адрес: %s\n", WiFi.localIP().toString().c_str());
    }
    else
    {
        Serial.println("\n❌ Не удалось подключиться к WiFi");
        diagnostics.wifiDisconnects++;
    }
}

void setCpuFrequencyCustom(uint8_t freq)
{
    static uint8_t currentFreq = 0;

    if (freq != currentFreq)
    {
        esp_pm_config_esp32_t config = {
            .max_freq_mhz = freq,
            .min_freq_mhz = freq,
            .light_sleep_enable = false};
        esp_pm_configure(&config);

        currentFreq = freq;
        Serial.printf("⚡ CPU frequency changed to %d MHz\n", freq);
    }
}

void emergencyShutdown(String reason)
{
    xSemaphoreTake(xMutexState, portMAX_DELAY);
    state.systemEnabled = false;
    state.emergencyStop = true;
    state.OutputA = 0;
    state.OutputB = 0;
    state.temperatureSensorFault = true;
    state.status = "EMERGENCY: " + reason;

    if (reason.length() > 128)
    {
        diagnostics.lastError = reason.substring(0, 128) + "...";
    }
    else
    {
        diagnostics.lastError = reason;
    }

    xSemaphoreGive(xMutexState);

    ledcWrite(PWM_CHANNEL_A, 0);
    ledcWrite(PWM_CHANNEL_B, 0);
    digitalWrite(LED_PIN, HIGH); // Мигание при аварии

    Serial.println("🚨 АВАРИЙНОЕ ОТКЛЮЧЕНИЕ: " + reason);
    mqttClient.publish(TOPIC_EMERGENCY, reason.c_str(), true);
}

void resetEmergency()
{
    xSemaphoreTake(xMutexState, portMAX_DELAY);
    state.emergencyStop = false;
    state.systemEnabled = true;
    state.status = "NORMAL";
    state.temperatureSensorFault = false;
    xSemaphoreGive(xMutexState);

    Serial.println("🔄 Аварийное состояние сброшено");
}

void publishStatus()
{
    xSemaphoreTake(xMutexState, portMAX_DELAY);

    if (mqttClient.connected())
    {
        mqttClient.publish(TOPIC_CURRENT_TEMP, String(state.currentTemp, 2).c_str(), true);
        mqttClient.publish(TOPIC_TARGET_TEMP_A, String(state.SetpointA, 1).c_str(), true);
        mqttClient.publish(TOPIC_TARGET_TEMP_B, String(state.SetpointB, 1).c_str(), true);
        mqttClient.publish(TOPIC_POWER_A, String(state.OutputA, 1).c_str(), true);
        mqttClient.publish(TOPIC_POWER_B, String(state.OutputB, 1).c_str(), true);
        mqttClient.publish(TOPIC_STATUS, state.status.c_str(), true);

        portENTER_CRITICAL(&pidMux);
        mqttClient.publish(TOPIC_PID_KP, String(Kp, 1).c_str(), true);
        mqttClient.publish(TOPIC_PID_KI, String(Ki, 2).c_str(), true);
        mqttClient.publish(TOPIC_PID_KD, String(Kd, 1).c_str(), true);
        portEXIT_CRITICAL(&pidMux);
    }

    xSemaphoreGive(xMutexState);
}

void publishDiagnostics()
{
// ИСПРАВЛЕНО: Подавляем ложное предупреждение
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    StaticJsonDocument<1024> doc;
#pragma GCC diagnostic pop

    xSemaphoreTake(xMutexState, portMAX_DELAY);
    doc["uptime"] = millis() / 1000;
    doc["wifi_disconnects"] = diagnostics.wifiDisconnects;
    doc["mqtt_disconnects"] = diagnostics.mqttDisconnects;
    doc["temp_errors"] = diagnostics.tempReadErrors;
    doc["pid_calculations"] = diagnostics.pidCalculations;
    doc["pid_calculations_fails"] = diagnostics.pidCalculationFailures;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["cpu_frequency"] = ESP.getCpuFreqMHz();
    doc["temperature_fault"] = state.temperatureSensorFault;
    doc["emergency_stop"] = state.emergencyStop;
    doc["last_error"] = diagnostics.lastError;
    doc["last_reset_reason"] = diagnostics.lastResetReason.c_str();
    xSemaphoreGive(xMutexState);

    char buffer[1024];
    serializeJson(doc, buffer);

    if (mqttClient.connected())
    {
        mqttClient.publish(TOPIC_DIAGNOSTICS, buffer, true);
    }
}

void callback(char *topic, byte *payload, unsigned int length)
{
    String message;
    for (int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }

    Serial.printf("📥 Получено: %s -> %s\n", topic, message.c_str());

    if (strcmp(topic, TOPIC_SETPOINT_A) == 0)
    {
        float newTemp = message.toFloat();
        if (newTemp >= MIN_SETPOINT && newTemp <= MAX_SETPOINT)
        {
            xSemaphoreTake(xMutexState, portMAX_DELAY);
            state.SetpointA = newTemp;
            xSemaphoreGive(xMutexState);
            Serial.printf("🎯 Зона A: новая температура %.1f°C\n", state.SetpointA);
        }
    }
    else if (strcmp(topic, TOPIC_ENABLED_A) == 0)
    {
        if (message == "RESET")
        {
            resetEmergency();
        }
        else
        {
            xSemaphoreTake(xMutexState, portMAX_DELAY);
            state.systemEnabled = (message == "ON" || message == "true");
            xSemaphoreGive(xMutexState);
            Serial.printf("🔄 Зона A: %s\n", state.systemEnabled ? "ВКЛЮЧЕНА" : "ОТКЛЮЧЕНА");
        }
    }
    else if (strcmp(topic, TOPIC_SETPOINT_B) == 0)
    {
        float newTemp = message.toFloat();
        if (newTemp >= MIN_SETPOINT && newTemp <= MAX_SETPOINT)
        {
            xSemaphoreTake(xMutexState, portMAX_DELAY);
            state.SetpointB = newTemp;
            xSemaphoreGive(xMutexState);
            Serial.printf("🎯 Зона B: новая температура %.1f°C\n", state.SetpointB);
        }
    }
    else if (strcmp(topic, TOPIC_ENABLED_B) == 0)
    {
        if (message == "RESET")
        {
            resetEmergency();
        }
        else
        {
            xSemaphoreTake(xMutexState, portMAX_DELAY);
            state.systemEnabled = (message == "ON" || message == "true");
            xSemaphoreGive(xMutexState);
            Serial.printf("🔄 Зона B: %s\n", state.systemEnabled ? "ВКЛЮЧЕНА" : "ОТКЛЮЧЕНА");
        }
    }
    else if (strcmp(topic, TOPIC_PID_KP) == 0)
    {
        double newKp = message.toFloat();
        if (newKp > 0 && newKp <= 50)
        {
            portENTER_CRITICAL(&pidMux);
            Kp = newKp;
            myPID_A.SetTunings(Kp, Ki, Kd);
            myPID_B.SetTunings(Kp, Ki, Kd);
            portEXIT_CRITICAL(&pidMux);
            savePIDParams();
            Serial.printf("🔧 Kp обновлен: %.1f\n", Kp);
        }
    }
    else if (strcmp(topic, TOPIC_PID_KI) == 0)
    {
        double newKi = message.toFloat();
        if (newKi >= 0 && newKi <= 5)
        {
            portENTER_CRITICAL(&pidMux);
            Ki = newKi;
            myPID_A.SetTunings(Kp, Ki, Kd);
            myPID_B.SetTunings(Kp, Ki, Kd);
            portEXIT_CRITICAL(&pidMux);
            savePIDParams();
            Serial.printf("🔧 Ki обновлен: %.3f\n", Ki);
        }
    }
    else if (strcmp(topic, TOPIC_PID_KD) == 0)
    {
        double newKd = message.toFloat();
        if (newKd >= 0 && newKd <= 20)
        {
            portENTER_CRITICAL(&pidMux);
            Kd = newKd;
            myPID_A.SetTunings(Kp, Ki, Kd);
            myPID_B.SetTunings(Kp, Ki, Kd);
            portEXIT_CRITICAL(&pidMux);
            savePIDParams();
            Serial.printf("🔧 Kd обновлен: %.1f\n", Kd);
        }
    }
}

// === ЗАДАЧИ FreeRTOS ===
void vTaskReadTemp(void *pvParameters)
{
    while (1)
    {
        esp_task_wdt_reset();

        float temp = readTemperature();

        xSemaphoreTake(xMutexState, portMAX_DELAY);
        state.currentTemp = temp;

        if (state.currentTemp > MAX_SAFE_TEMP && !state.emergencyStop)
        {
            xSemaphoreGive(xMutexState);
            emergencyShutdown("Перегрев: " + String(state.currentTemp, 1) + "°C");
            xSemaphoreTake(xMutexState, portMAX_DELAY);
        }

        if (state.temperatureSensorFault && millis() - state.lastSuccessfulTempRead > 300000)
        {
            xSemaphoreGive(xMutexState);
            emergencyShutdown("Неисправность датчика температуры");
            xSemaphoreTake(xMutexState, portMAX_DELAY);
        }

        xSemaphoreGive(xMutexState);

        vTaskDelay(UPDATE_INTERVAL);
    }
}

void vTaskCalculatePID(void *pvParameters)
{
    myPID_A.SetMode(AUTOMATIC);
    myPID_A.SetOutputLimits(0, 100);
    myPID_A.SetSampleTime(5000);
    myPID_B.SetMode(AUTOMATIC);
    myPID_B.SetOutputLimits(0, 100);
    myPID_B.SetSampleTime(5000);

    while (1)
    {
        esp_task_wdt_reset();

        xSemaphoreTake(xMutexState, portMAX_DELAY);

        if (state.systemEnabled && !state.emergencyStop && !state.temperatureSensorFault)
        {
            // Рассчитываем мощность для каждой зоны
            bool computedA, computedB;

            // КРИТИЧЕСКИ ВАЖНО: Защищаем вызов Compute() той же секцией, что и изменение параметров
            portENTER_CRITICAL(&pidMux);
            computedA = myPID_A.Compute();
            computedB = myPID_B.Compute();
            portEXIT_CRITICAL(&pidMux);

            if (!computedA && !computedB)
            {
                // Не было вычисления - возможно, сбой
                diagnostics.pidCalculationFailures++;

                // Попробуем восстановить состояние
                portENTER_CRITICAL(&pidMux);
                myPID_A.SetMode(AUTOMATIC);
                myPID_B.SetMode(AUTOMATIC);
                portEXIT_CRITICAL(&pidMux);
            }

            if (computedA || computedB)
            {
                diagnostics.pidCalculations++;

                // Ограничение суммарной мощности (не более 100%)
                if (state.OutputA + state.OutputB > 100.0)
                {
                    float total = state.OutputA + state.OutputB;
                    state.OutputA = state.OutputA * 100.0 / total;
                    state.OutputB = state.OutputB * 100.0 / total;
                }

                // Плавное изменение мощности
                float deltaA = state.OutputA - lastOutputA;
                float deltaB = state.OutputB - lastOutputB;

                if (deltaA > maxChangePerCycle)
                {
                    state.OutputA = lastOutputA + maxChangePerCycle;
                }
                else if (deltaA < -maxChangePerCycle)
                {
                    state.OutputA = lastOutputA - maxChangePerCycle;
                }

                if (deltaB > maxChangePerCycle)
                {
                    state.OutputB = lastOutputB + maxChangePerCycle;
                }
                else if (deltaB < -maxChangePerCycle)
                {
                    state.OutputB = lastOutputB - maxChangePerCycle;
                }

                lastOutputA = state.OutputA;
                lastOutputB = state.OutputB;

                state.status = "REGULATING";
            }
        }
        else
        {
            state.OutputA = 0;
            state.OutputB = 0;
            state.status = state.emergencyStop ? "EMERGENCY" : (state.systemEnabled ? "STANDBY" : "OFF");
        }

        xSemaphoreGive(xMutexState);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void vTaskControlPWM(void *pvParameters)
{
    uint8_t currentPWM_A = 0;
    uint8_t currentPWM_B = 0;
    uint32_t lastCycleStart = millis();

    while (1)
    {
        esp_task_wdt_reset();

        xSemaphoreTake(xMutexState, portMAX_DELAY);

        // Интерлейвинг PWM: 1 секунда между контурами
        const uint32_t CYCLE_PERIOD_MS = 2000; // 2 секунды
        uint32_t timeInCycle = (millis() - lastCycleStart) % CYCLE_PERIOD_MS;

        // Получаем целевые значения мощности
        uint8_t targetPWM_A = (uint8_t)(state.OutputA * 2.55f); // 0–100% → 0–255
        uint8_t targetPWM_B = (uint8_t)(state.OutputB * 2.55f);

        // Контур A работает в первой половине цикла
        if (timeInCycle < CYCLE_PERIOD_MS / 2)
        {
            // Контур A активен
            if (targetPWM_A > currentPWM_A)
            {
                currentPWM_A = min(static_cast<uint8_t>(currentPWM_A + PWM_SMOOTHING_STEP), targetPWM_A);
            }
            else if (targetPWM_A < currentPWM_A)
            {
                currentPWM_A = max(static_cast<uint8_t>(currentPWM_A - PWM_SMOOTHING_STEP), targetPWM_A);
            }

            // Физически отключаем контур B, но НЕ СБРАСЫВАЕМ его состояние
            ledcWrite(PWM_CHANNEL_A, currentPWM_A);
            ledcWrite(PWM_CHANNEL_B, 0);
        }
        else
        {
            // Контур B активен
            if (targetPWM_B > currentPWM_B)
            {
                currentPWM_B = min(static_cast<uint8_t>(currentPWM_B + PWM_SMOOTHING_STEP), targetPWM_B);
            }
            else if (targetPWM_B < currentPWM_B)
            {
                currentPWM_B = max(static_cast<uint8_t>(currentPWM_B - PWM_SMOOTHING_STEP), targetPWM_B);
            }

            // Физически отключаем контур A, но НЕ СБРАСЫВАЕМ его состояние
            ledcWrite(PWM_CHANNEL_B, currentPWM_B);
            ledcWrite(PWM_CHANNEL_A, 0);
        }

        xSemaphoreGive(xMutexState);

        vTaskDelay(PWM_UPDATE_INTERVAL);
    }
}

void vTaskMQTTClient(void *pvParameters)
{
    while (1)
    {
        esp_task_wdt_reset();

        if (mqttClient.connected())
        {
            setCpuFrequencyCustom(240);
        }
        else
        {
            setCpuFrequencyCustom(80);
        }

        if (WiFi.status() != WL_CONNECTED)
        {
            if (millis() - wifiLastReconnectAttempt > WIFI_RECONNECT_INTERVAL)
            {
                connectToWiFi();
                wifiLastReconnectAttempt = millis();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!mqttClient.connected())
        {
            Serial.print("⏳ Подключение к MQTT...");
            if (mqttClient.connect(MQTT_CLIENT_ID))
            {
                Serial.println(" ✅ Успешно!");
                mqttClient.subscribe(TOPIC_SETPOINT_A);
                mqttClient.subscribe(TOPIC_ENABLED_A);
                mqttClient.subscribe(TOPIC_SETPOINT_B);
                mqttClient.subscribe(TOPIC_ENABLED_B);
                mqttClient.subscribe(TOPIC_PID_KP);
                mqttClient.subscribe(TOPIC_PID_KI);
                mqttClient.subscribe(TOPIC_PID_KD);
                publishStatus();
            }
            else
            {
                Serial.printf("❌ ошибка, код=%d\n", mqttClient.state());
                diagnostics.mqttDisconnects++;
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
        else
        {
            bool wasConnected = mqttClient.connected();
            mqttClient.loop();

            if (!wasConnected && mqttClient.connected())
            {
                Serial.println("✅ MQTT переподключен!");
                publishStatus();
            }

            static uint32_t lastPublish = 0;
            static uint32_t lastDiagnostics = 0;

            if (millis() - lastPublish > 10000)
            {
                publishStatus();
                lastPublish = millis();
            }

            if (millis() - lastDiagnostics > DIAGNOSTICS_INTERVAL)
            {
                publishDiagnostics();
                lastDiagnostics = millis();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void vTaskOTA(void *pvParameters)
{
    ArduinoOTA.setHostname(otaHostname.c_str());
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]()
                       {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else
      type = "filesystem";

    Serial.printf("Начало обновления: %s\n", type.c_str()); });

    ArduinoOTA.onEnd([]()
                     { Serial.println("\nОбновление завершено"); });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                          { Serial.printf("Прогресс: %u%%\n", (progress / (total / 100))); });

    ArduinoOTA.onError([](ota_error_t error)
                       {
    Serial.printf("Ошибка OTA: %d\n", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Ошибка аутентификации");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Ошибка начала обновления");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Ошибка подключения");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Ошибка приёма");
    else if (error == OTA_END_ERROR) Serial.println("Ошибка завершения"); });

    ArduinoOTA.begin();

    while (1)
    {
        esp_task_wdt_reset();
        ArduinoOTA.handle();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// === ИНИЦИАЛИЗАЦИЯ ===
void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n=== 🚀 Wemos S2 Mini Термостат с двумя контурами ===");
    Serial.println("Версия: 9.0 (Интерлейвинг PWM, FreeRTOS, OTA)");

    // Определяем причину перезагрузки
    esp_reset_reason_t resetReason = esp_reset_reason();
    diagnostics.lastResetReason = resetReasonToString(resetReason);
    Serial.print("Перезагрузка по причине: ");
    Serial.println(diagnostics.lastResetReason);

    // Инициализация пинов
    pinMode(SSR_PIN_A, OUTPUT);
    pinMode(SSR_PIN_B, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(SSR_PIN_A, LOW);
    digitalWrite(SSR_PIN_B, LOW);
    digitalWrite(LED_PIN, LOW);

    // Инициализация датчика
    sensors.begin();
    sensors.setResolution(12);

    // Инициализация FreeRTOS объектов
    xMutexState = xSemaphoreCreateMutex();
    xQueueTemp = xQueueCreate(10, sizeof(float));

    if (!xMutexState || !xQueueTemp)
    {
        Serial.println("❌ Ошибка создания объектов синхронизации!");
        while (1)
            ;
    }

    // Загрузка параметров
    loadPIDParams();

    // Инициализация PID
    myPID_A.SetMode(AUTOMATIC);
    myPID_A.SetOutputLimits(0, 100);
    myPID_A.SetSampleTime(5000);
    myPID_B.SetMode(AUTOMATIC);
    myPID_B.SetOutputLimits(0, 100);
    myPID_B.SetSampleTime(5000);
    myPID_A.SetTunings(Kp, Ki, Kd);
    myPID_B.SetTunings(Kp, Ki, Kd);

    // Инициализация PWM
    setupPWM();

    // Настройка Watchdog
    esp_task_wdt_init(WATCHDOG_TIMEOUT, true);
    esp_task_wdt_add(NULL);

    // Подключение к WiFi
    connectToWiFi();

    // Настройка MQTT
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(callback);
    mqttClient.setBufferSize(1024);

    // Настройка OTA
    setupOTA();

    // Создание задач
    xTaskCreate(vTaskReadTemp, "ReadTemp", 4096, NULL, 2, &hReadTemp);
    xTaskCreate(vTaskCalculatePID, "CalcPID", 4096, NULL, 3, &hCalcPID);
    xTaskCreate(vTaskControlPWM, "ControlPWM", 4096, NULL, 3, &hControlPWM);
    xTaskCreate(vTaskMQTTClient, "MQTTClient", 8192, NULL, 2, &hMQTT);
    xTaskCreate(vTaskOTA, "OTA", 8192, NULL, 1, &hOTA);

    // Добавляем все задачи в Watchdog
    esp_task_wdt_add(hReadTemp);
    esp_task_wdt_add(hCalcPID);
    esp_task_wdt_add(hControlPWM);
    esp_task_wdt_add(hMQTT);
    esp_task_wdt_add(hOTA);

    Serial.println("✅ Все задачи запущены. Система готова.");
    digitalWrite(LED_PIN, HIGH); // Светодиод горит при нормальной работе
}

void setupOTA()
{
    MDNS.begin(otaHostname.c_str());
    Serial.printf("🌐 OTA доступен по http://%s.local/update\n", otaHostname.c_str());
}

const char *resetReasonToString(esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_UNKNOWN:
        return "UNKNOWN";
    case ESP_RST_POWERON:
        return "POWERON_RESET";
    case ESP_RST_SW:
        return "SW_RESET";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT_RESET";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT_RESET";
    case ESP_RST_WDT:
        return "WDT_RESET";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP_RESET";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT_RESET";
    case ESP_RST_SDIO:
        return "SDIO_RESET";
    default:
        return "INVALID_REASON";
    }
}

void loop()
{
    esp_task_wdt_reset();

    // Мигание LED для индикации работы
    static uint32_t lastBlink = 0;
    if (millis() - lastBlink > 1000)
    {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        lastBlink = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}