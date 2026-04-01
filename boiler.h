#ifndef BOILER_H
#define BOILER_H

#include <ESP8266HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
// === Защита от сквозняка ===
#define TEMP_SAMPLE_INTERVAL_MS 5000UL
#define BOILER_AVG_WINDOW_MS 120000UL
#define TEMP_DRAFT_DROP_THRESHOLD 0.5f

float boilerSampleSum = 0.0f;
uint32_t boilerSampleCount = 0;
float boilerAvgTemp = NAN;
unsigned long lastAcceptedSampleAt = 0;
float lastAcceptedSampleTemp = NAN;

float narodMonWeightedSum = 0.0f;
unsigned long narodMonWeightedTime = 0;
float narodMonAvgTemp = NAN;
unsigned long lastSensorPoll = 0;
// === ВНЕШНИЕ ЗАВИСИМОСТИ (из sht_tag.ino) ===
extern void serialToTelegram(const String &message);
extern void reportError(const String &message);
extern const long BOT_ID;
extern long lastUserChatID;
extern unsigned long lastUserCommandTime;
extern const unsigned long COMMAND_COOLDOWN_MS;
extern bool shouldSendError();
extern bool workScheduled;          // активен ли временной интервал
extern uint8_t workStartHour, workStartMinute;
extern uint8_t workEndHour, workEndMinute;
// === Настройки ===
#define EEPROM_SIZE_BOILER 512
#define MAX_ZONES 8

// EEPROM адреса
#define EEPROM_THERMO_ENABLE 128
#define EEPROM_HYSTERESIS 132
#define EEPROM_ZONE_COUNT 136
#define EEPROM_WORK_DURATION 138  // 2 байта (uint16_t)
#define EEPROM_MIN_START_INTERVAL 140 // ← ЭТОГО НЕ ХВАТАЛО!
#define EEPROM_ZONES_START 144

struct TimeSlot {
  uint8_t startHour;
  uint8_t startMinute;
  uint8_t endHour;
  uint8_t endMinute;
  float temp;
};

// === Внешние объекты ===
extern Adafruit_SHT31 sht31;
extern FastBot bot;
// === Объявляем urlencode и sendMsg ===
String urlencode(const String &str);
bool sendMsg(String text, String chatId);
// === Глобальные переменные ===
bool thermostatEnabled = false;
bool workModeEnabled = false; // false = auto (расписание), true = work (22°C)
const float WORK_TEMP = 22.0;
float hysteresis = 0.5;
bool relayState = false;
// === Таймерные переменные ===
unsigned long relayStartTime = 0;         // время включения реле
unsigned long lastRelayTurnOffTime = 0;   // время последнего ВЫКЛЮЧЕНИЯ
unsigned long lastRelayOnTime = 0;        // ← НОВОЕ: время последнего УСПЕШНОГО ВКЛЮЧЕНИЯ
bool relayIsRunning = false;              // флаг: реле активно (включено и работает)
uint16_t workDurationMinutes = 4;         // макс. время работы (настраивается)
uint32_t minStartInterval = 10 * 60 * 1000UL;
uint16_t baseWorkDurationMinutes = 4;
uint32_t baseMinStartInterval = 10 * 60 * 1000UL;
bool temporaryBoostActive = false;
unsigned long temporaryBoostUntil = 0;
TimeSlot timeZones[MAX_ZONES];
uint8_t zoneCount = 0;
bool zonesChanged = false;

IPAddress relayIP(192, 168, 31, 200);  // статический адрес реле
bool relayFound = true;                // статус доступности реле

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3 * 3600, 60000);

// === Функции ===
void saveZones() {
  if (!zonesChanged) return;

  EEPROM.begin(EEPROM_SIZE_BOILER);
  EEPROM.put(EEPROM_ZONE_COUNT, zoneCount);
  for (int i = 0; i < zoneCount; i++) {
    EEPROM.put(EEPROM_ZONES_START + i * sizeof(TimeSlot), timeZones[i]);
  }
  EEPROM.commit();
  EEPROM.end();
  zonesChanged = false;
}

void loadZones() {
  EEPROM.begin(EEPROM_SIZE_BOILER);
  EEPROM.get(EEPROM_ZONE_COUNT, zoneCount);
  if (zoneCount > MAX_ZONES) zoneCount = 0;
  if (zoneCount == 0) zonesChanged = false;
  for (int i = 0; i < zoneCount; i++) {
    EEPROM.get(EEPROM_ZONES_START + i * sizeof(TimeSlot), timeZones[i]);
  }
  EEPROM.end();
}

float getCurrentTargetTemp() {
    // 🔥 РЕЖИМ "РАБОТА" — приоритет над расписанием
   
    // --- СТАНДАРТНОЕ РАСПИСАНИЕ ---
    if (zoneCount == 0 || workModeEnabled) {
        return 22.0;
    }

    // Принудительно обновляем время
    timeClient.update();

    int currentHour = timeClient.getHours();
    int currentMinute = timeClient.getMinutes();

    // Если время не синхронизировано — возвращаем первую зону (но с защитой)
    if (currentHour < 0 || currentHour > 23 || currentMinute < 0 || currentMinute > 59) {
        // Защита от битого temp
        if (timeZones[0].temp >= 5.0 && timeZones[0].temp <= 30.0) {
            return timeZones[0].temp;
        }
        return 22.0;
    }

    int current = currentHour * 60 + currentMinute;

    for (int i = 0; i < zoneCount; i++) {
        int start = timeZones[i].startHour * 60 + timeZones[i].startMinute;
        int end   = timeZones[i].endHour   * 60 + timeZones[i].endMinute;

        bool matches = false;
        if (start < end) {
            matches = (current >= start && current < end);
        } else {
            matches = (current >= start || current < end);
        }

        if (matches) {
            // Защита от битого значения
            if (timeZones[i].temp >= 5.0 && timeZones[i].temp <= 30.0) {
                return timeZones[i].temp;
            }
            return 22.0;
        }
    }

    // Ни одна зона не подошла — возвращаем первую с защитой
    if (timeZones[0].temp >= 5.0 && timeZones[0].temp <= 30.0) {
        return timeZones[0].temp;
    }
    return 22.0;
}
bool sendRelayCommand(bool on) {
  static uint8_t failCount = 0;

  if (WiFi.status() != WL_CONNECTED) {
    if (shouldSendError()) {
      serialToTelegram("❌ Wi-Fi отключён. Невозможно отправить команду.");
    }
    return false;
  }

  String url = "http://" + relayIP.toString() + "/" + (on ? "on" : "off");
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    http.end();
    failCount = 0;
    if (!relayFound) {
      relayFound = true;
      serialToTelegram("✅ Реле снова доступно: " + relayIP.toString());
    }
    return true;
  } else {
    failCount++;
    if (shouldSendError()) {
      serialToTelegram("🚨 Ошибка: реле не отвечает (" + String(httpCode) + ") IP: " + relayIP.toString());
    }
    if (failCount >= 3) {
      relayFound = false;
      serialToTelegram("⛔ Потеряна связь с реле после 3 неудачных попыток!");
      failCount = 0;
    }
    http.end();
    return false;
  }
}

uint16_t getEffectiveWorkDurationMinutes() {
  return temporaryBoostActive ? 14 : baseWorkDurationMinutes;
}

uint32_t getEffectiveMinStartInterval() {
  return temporaryBoostActive ? 60 * 1000UL : baseMinStartInterval;
}

void activateTemporaryBoost() {
  temporaryBoostActive = true;
  temporaryBoostUntil = millis() + 60UL * 60UL * 1000UL;
}

void maybeFinishTemporaryBoost() {
  if (!temporaryBoostActive) return;
  if ((long)(millis() - temporaryBoostUntil) >= 0) {
    temporaryBoostActive = false;
    sendMsg("✅ Временные настройки завершены: длительность и пауза возвращены к пользовательским.", String(lastUserChatID));
  }
}

float getBoilerAverageTemp() {
  return boilerAvgTemp;
}

float getNarodMonAverageTemp() {
  return narodMonAvgTemp;
}

void resetNarodMonAverage() {
  narodMonWeightedSum = 0.0f;
  narodMonWeightedTime = 0;
}

void updateAverageTemperature() {
  unsigned long now = millis();
  if (now - lastSensorPoll < TEMP_SAMPLE_INTERVAL_MS) return;
  lastSensorPoll = now;

  float t = sht31.readTemperature();
  if (isnan(t)) return;

  if (!isnan(lastAcceptedSampleTemp) && (lastAcceptedSampleTemp - t) >= TEMP_DRAFT_DROP_THRESHOLD) {
    return; // защита от сквозняка: резкий провал не учитываем
  }

  if (!isnan(lastAcceptedSampleTemp) && lastAcceptedSampleAt > 0) {
    unsigned long dt = now - lastAcceptedSampleAt;
    boilerSampleSum += t;
    boilerSampleCount++;
    boilerAvgTemp = boilerSampleCount > 0 ? boilerSampleSum / (float)boilerSampleCount : t;
    narodMonWeightedSum += lastAcceptedSampleTemp * (float)dt;
    narodMonWeightedTime += dt;
    narodMonAvgTemp = narodMonWeightedTime > 0 ? narodMonWeightedSum / (float)narodMonWeightedTime : t;
  } else {
    boilerSampleSum = t;
    boilerSampleCount = 1;
    boilerAvgTemp = t;
    narodMonAvgTemp = t;
  }

  lastAcceptedSampleTemp = t;
  lastAcceptedSampleAt = now;
}

void updateRelay() {
  // Если термостат выключен — выключаем реле
  if (!thermostatEnabled) {
    if (relayIsRunning || relayState) {
      bool success = sendRelayCommand(false);
      if (success) {
        relayState = false;
        relayIsRunning = false;
        lastRelayTurnOffTime = millis();
        sendMsg("🛑 Термостат выключен — реле ОТКЛЮЧЕНО", String(lastUserChatID));
      }
    }
    return;
  }

 // Для выключения — мгновенная температура
float instantTemp = sht31.readTemperature();
if (isnan(instantTemp)) return;

// Для включения — средняя температура за 2 мин
float t = boilerAvgTemp;
if (isnan(t)) t = instantTemp; // fallback при отсутствии истории

  float target = getCurrentTargetTemp();
  uint16_t effectiveWorkDuration = getEffectiveWorkDurationMinutes();
  uint32_t effectiveMinStartInterval = getEffectiveMinStartInterval();

  // === Режим: реле ВКЛЮЧЕНО ===
  if (relayIsRunning) {
    bool shouldTurnOff = false;
    String reason = "";

    // Условие 1: достигли целевой температуры (без гистерезиса!)
    if (t >= target) {
      shouldTurnOff = true;
      reason = "достигнута цель";
    }
    // Условие 2: истекло максимальное время работы
    else if (millis() - relayStartTime >= effectiveWorkDuration * 60 * 1000UL) {
      shouldTurnOff = true;
      reason = "таймер (" + String(effectiveWorkDuration) + " мин)";
    }

    if (shouldTurnOff) {
      bool success = sendRelayCommand(false);
      if (success) {
        relayState = false;
        relayIsRunning = false;
        lastRelayTurnOffTime = millis();
        sendMsg(
          "❄️ Реле выключено: " + reason + ". T=" + String(t, 1) + "°C, цель=" + String(target, 1) + "°C",
          String(lastUserChatID)
        );
      } else {
        if (shouldSendError()) {
          serialToTelegram("⚠️ Не удалось выключить реле. Причина: " + reason);
        }
      }
    }
    return;
  }

  // === Режим: реле ВЫКЛЮЧЕНО ===
  bool cooldownPassed = (millis() - lastRelayTurnOffTime >= effectiveMinStartInterval);
  bool needsHeat = (t < target - hysteresis); // гистерезис ТОЛЬКО при включении

  if (cooldownPassed && needsHeat) {
    bool success = sendRelayCommand(true);
    if (success) {
      relayState = true;
      relayIsRunning = true;
      relayStartTime = millis();
      lastRelayOnTime = millis();
      sendMsg(
        "🔥 Включено: T=" + String(t, 1) + "°C < " + String(target - hysteresis, 1) + "°C",
        String(lastUserChatID)
      );
    } else {
      if (shouldSendError()) {
        serialToTelegram("⚠️ Не удалось включить реле. T=" + String(t,1) + ", цель=" + String(target,1));
      }
    }
  }
}
void boilerLoop() {
  updateAverageTemperature(); // <-- добавлено
  maybeFinishTemporaryBoost();
  static unsigned long lastUpdate = 0;
  static unsigned long lastSave = 0;

  if (millis() - lastUpdate > 10000) {
    lastUpdate = millis();
    updateRelay();
  }

  if (zonesChanged && millis() - lastSave > 60000) {
    saveZones();
    lastSave = millis();
  }
  if (workScheduled && workModeEnabled) {
    timeClient.update();
    int nowMinutes = timeClient.getHours() * 60 + timeClient.getMinutes();
    int startMinutes = workStartHour * 60 + workStartMinute;
    int endMinutes   = workEndHour * 60 + workEndMinute;

    bool intervalOver = false;
    if (startMinutes <= endMinutes) {
        // обычный случай
        intervalOver = (nowMinutes >= endMinutes);
    } else {
        // через полночь
        intervalOver = (nowMinutes >= endMinutes && nowMinutes < startMinutes);
    }

    if (intervalOver) {
        workModeEnabled = false;   // выключаем режим работы
        workScheduled = false;     // интервал завершён
        sendMsg("✅ Временной интервал работы окончен. Переключаемся в АВТО", String(lastUserChatID));
    }
}

}

void setupBoilerCommands() {
  EEPROM.begin(EEPROM_SIZE_BOILER);
  EEPROM.get(EEPROM_THERMO_ENABLE, thermostatEnabled);
  EEPROM.get(EEPROM_HYSTERESIS, hysteresis);
  if (hysteresis < 0.1 || hysteresis > 2.0) hysteresis = 0.5;

  // Загрузка времени работы
  EEPROM.get(EEPROM_WORK_DURATION, workDurationMinutes);
  if (workDurationMinutes < 1 || workDurationMinutes > 60) {
    workDurationMinutes = 4;
    EEPROM.put(EEPROM_WORK_DURATION, workDurationMinutes);
  }
  baseWorkDurationMinutes = workDurationMinutes;

  // Загрузка минимального интервала между включениями
  EEPROM.get(EEPROM_MIN_START_INTERVAL, minStartInterval);
  if (minStartInterval < 1 * 60 * 1000UL || minStartInterval > 60 * 60 * 1000UL) {
    minStartInterval = 10 * 60 * 1000UL; // 10 минут по умолчанию
    EEPROM.put(EEPROM_MIN_START_INTERVAL, minStartInterval);
  }
  baseMinStartInterval = minStartInterval;

  EEPROM.commit();
  EEPROM.end();

  loadZones();
}
void saveWorkDuration() {
  EEPROM.begin(EEPROM_SIZE_BOILER);
  EEPROM.put(EEPROM_WORK_DURATION, workDurationMinutes);
  EEPROM.commit();
  EEPROM.end();
}
void saveMinStartInterval() {
    EEPROM.begin(EEPROM_SIZE_BOILER);
    EEPROM.put(EEPROM_MIN_START_INTERVAL, minStartInterval);
    EEPROM.commit();
    EEPROM.end();
}
#endif // BOILER_H
