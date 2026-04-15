#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiClient.h>
#include <FastBot.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <EEPROM.h>
#include <Ticker.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoOTA.h>
#include "boiler.h"

#define EEPROM_SIZE 512
#define TOKEN "8079276277:AAHrqQKTo3vp76bcX2ekPw59dwWxRvTaEHg"
#define ADMIN_CHAT_ID "-4647981556"
const char* CHAT_ID   = "619084238";
const char* WORKER    = "royal-river-71a9.dragonforceedge.workers.dev";

// --- Настройки реле ---
unsigned long lastRelayErrorTime = 0;
const unsigned long RELAY_ERROR_COOLDOWN = 5 * 60 * 1000;
unsigned long lastRelayMsgTime = 0;
const unsigned long RELAY_MSG_INTERVAL = 60000;

ESP8266WiFiMulti WiFiMulti;
WiFiClient client;
FastBot bot(TOKEN);
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Ticker autoReportTicker;
Ticker narodmonTicker;

// 🔧 UTF-8 → %XX
String urlEncodeUTF8(const String& text) {
    String enc; enc.reserve(text.length() * 3);
    char buf[4];
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') enc += c;
        else { sprintf(buf, "%%%02X", (uint8_t)c); enc += buf; }
    }
    return enc;
}

// Глобальный клиент для отправки
WiFiClientSecure tgClient;
bool tgClientReady = false;

bool sendTG(const String& msg, const String& chatId = "") {
    String targetChat = chatId.length() ? chatId : CHAT_ID;
    
    if (!tgClientReady) {
        tgClient.setInsecure();
        tgClient.setTimeout(10000);
        tgClientReady = true;
    }
    if (tgClient.connected()) tgClient.stop();

    if (!tgClient.connect(WORKER, 443)) {
        delay(200);
        if (!tgClient.connect(WORKER, 443)) return false;
    }

    // POST-запрос
    String body = "chat_id=" + targetChat + "&text=" + urlEncodeUTF8(msg);
    String path = "/bot" + String(TOKEN) + "/sendMessage";

    tgClient.print("POST " + path + " HTTP/1.1\r\n");
    tgClient.print("Host: " + String(WORKER) + "\r\n");
    tgClient.print("Content-Type: application/x-www-form-urlencoded\r\n");
    tgClient.print("Content-Length: " + String(body.length()) + "\r\n");
    tgClient.print("Connection: close\r\n\r\n");
    tgClient.print(body);
    tgClient.flush();
    
    delay(100); // Пауза для переключения SSL-стека
    yield();

    // Читаем ответ
    String response;
    response.reserve(2048);
    unsigned long lastByte = millis();
    unsigned long start = millis();
    
    while (!tgClient.available() && millis() - start < 5000) {
        delay(10); yield();
    }
    
    while (millis() - lastByte < 2000 && millis() - start < 12000) {
        while (tgClient.available()) {
            char c = tgClient.read();
            response += c;
            lastByte = millis();
            if (response.length() >= 2000) goto parse_response;
        }
        delay(10); yield();
    }
    
parse_response:
    tgClient.stop();
    delay(50);

    return response.indexOf("\"ok\":true") >= 0;
}

// === Переменные режима /work ===
bool workScheduled = false;
uint8_t workStartHour, workStartMinute;
uint8_t workEndHour, workEndMinute;

String wifiSSID = "";
String wifiPASS = "";
bool autoReportEnabled = false;
uint32_t reportInterval = 600;
volatile bool needToSendAutoReport = false;
bool sendtonm = false;
unsigned long startupTime = 0;
const unsigned long startupDelay = 5000;
long lastMessageID = 0;
long lastUpdateID = 0;
float Temperature; 
float Temperatur;       

#define EEPROM_LAST_UPDATE_ID 100

float tempSumForNarodMon = 0.0;
uint8_t narodMonMeasurementCount = 0;
unsigned long lastNarodMonMeasurementTime = 0;
const unsigned long NARODMON_MEAS_INTERVAL = 100000;

void saveLastUpdateID() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(EEPROM_LAST_UPDATE_ID, lastUpdateID);
    EEPROM.commit();
    EEPROM.end();
}

void loadLastUpdateID() {
    EEPROM.begin(EEPROM_SIZE);
    long tmp = 0;
    EEPROM.get(EEPROM_LAST_UPDATE_ID, tmp);
    EEPROM.end();
    if (tmp < 0) lastUpdateID = 0;
    else lastUpdateID = tmp;
}

void sendMsg(String text, String chatId) {
  sendTG(text, chatId);
}

// === Защита от спама ===
int32_t lastUserChatID = 0;
unsigned long lastUserCommandTime = 0;
const unsigned long COMMAND_COOLDOWN_MS = 2000;

const long BOT_ID = 530615559;

void serialToTelegram(const String &message) {
    if (WiFi.status() == WL_CONNECTED) {
        sendMsg("📌 Лог: " + message, ADMIN_CHAT_ID);
    }
}

bool shouldSendError() {
  static unsigned long lastErrorMsg = 0;
  static unsigned long errorCount = 0;
  static unsigned long errorInterval = 60000;

  if (millis() - lastErrorMsg > errorInterval) {
    lastErrorMsg = millis();
    errorCount++;
    if (errorCount % 10 == 0) errorInterval += 120000;
    return true;
  }
  return false;
}

void reportError(const String &message) {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    if (WiFi.status() == WL_CONNECTED) {
        sendMsg("🚨 Ошибка: " + message + "\nID: " + mac, ADMIN_CHAT_ID);
    } else {
        serialToTelegram("🚨 Ошибка: " + message);
    }
}

void loadSettings() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(0, autoReportEnabled);
    EEPROM.get(4, reportInterval);
    char ssidBuf[32], passBuf[32];
    EEPROM.get(8, ssidBuf);
    EEPROM.get(40, passBuf);
    EEPROM.end();

    ssidBuf[31] = '\0'; passBuf[31] = '\0';
    wifiSSID = String(ssidBuf); wifiPASS = String(passBuf);

    if ((uint8_t)ssidBuf[0] == 0xFF || ssidBuf[0] == '\0' || wifiSSID.length() == 0) {
        wifiSSID = "Pogoda"; wifiPASS = "yeea6jxp";
        saveSettings();
    }
    if (reportInterval < 30 || reportInterval > 86400) reportInterval = 600;
}

void saveSettings() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(0, autoReportEnabled);
    EEPROM.put(4, reportInterval);
    char ssidBuf[32], passBuf[32];
    wifiSSID.toCharArray(ssidBuf, 32);
    wifiPASS.toCharArray(passBuf, 32);
    EEPROM.put(8, ssidBuf);
    EEPROM.put(40, passBuf);
    EEPROM.commit();
    EEPROM.end();
}

bool isValidWiFiString(const String &str) {
    return str.length() >= 1 && str.length() <= 32;
}

void sendSHT31Data() {
    if (narodMonMeasurementCount == 0) {
        float t = sht31.readTemperature();
        float h = sht31.readHumidity();
        if (isnan(t) || isnan(h)) { reportError("Нет данных для NarodMon"); return; }
        tempSumForNarodMon = t;
        narodMonMeasurementCount = 1;
    }
    float avgTemp = tempSumForNarodMon / narodMonMeasurementCount;
    float h = sht31.readHumidity();
    if (isnan(h)) { reportError("Ошибка влажности NarodMon"); return; }

    String data = "#ESPT" + WiFi.macAddress() + "\n";
    data.replace(":", "");
    data += "#Tempe#" + String(avgTemp, 2) + "\n";
    data += "#Vlazno#" + String(h, 2) + "\n##";

    for (int i = 0; i < 5; ++i) {
        if (client.connect("narodmon.ru", 8283)) {
            client.print(data); client.stop();
            tempSumForNarodMon = 0.0; narodMonMeasurementCount = 0;
            lastNarodMonMeasurementTime = millis();
            return;
        }
        delay(2000);
    }
    reportError("Не удалось отправить NarodMon");
}

void sendToTelegram(String cid) {
    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    if (isnan(t) || isnan(h)) return;
    String msg = "🌡 Температура: " + String(t, 2) + " °C\n💧 Влажность: " + String(h, 2) + " %";
    sendMsg(msg, cid);
}

void setupOTA() {
    ArduinoOTA.setHostname("ESP-Tag");
    ArduinoOTA.setPassword("12345678");
    ArduinoOTA.onError([](ota_error_t error) { reportError("Ошибка OTA: " + String(error)); });
    ArduinoOTA.begin();
}

void autoReportTick() { needToSendAutoReport = true; }
void narodmonTick() { sendtonm = true; }

void setup() {  
    Serial.begin(115200);
    tgClient.setBufferSizes(2048, 2048);
    bot.setBufferSizes(1024, 1024);
    delay(2000);
    
    startupTime = millis();
    loadSettings();
    loadLastUpdateID();
    
    bot.setOffset(lastUpdateID); // Восстановление оффсета
    
    WiFi.mode(WIFI_STA);
    WiFiMulti.addAP(wifiSSID.c_str(), wifiPASS.c_str());
    
    bool connected = false;
    if (WiFiMulti.run() == WL_CONNECTED) connected = true;
    else {
        delay(120000);
        if (WiFiMulti.run() == WL_CONNECTED) connected = true;
    }
    if (!connected) {
        WiFiMulti = ESP8266WiFiMulti();
        WiFiMulti.addAP("Pogoda", "yeea6jxp");
        for (int i = 0; i < 5; i++) {
            if (WiFiMulti.run() == WL_CONNECTED) { connected = true; break; }
            delay(15000);
        }
    }
    if (!connected) reportError("Нет Wi-Fi. Работа в оффлайне.");
    else {
        delay(2000);
        sendMsg("🤖 Устройство перезапущено", "-1001819803857");
        narodmonTicker.attach(303, narodmonTick);
        timeClient.begin();
        unsigned long startWait = millis();
        while (!timeClient.update() && millis() - startWait < 10000) { delay(100); yield(); }
    }
    
    if (!sht31.begin(0x44)) reportError("Нет датчика SHT31!");
    if (autoReportEnabled) autoReportTicker.attach(reportInterval, autoReportTick);
    
    bot.attach([](FB_msg& msg) {
        if (msg.chatID.toInt() == lastUserChatID && millis() - lastUserCommandTime < COMMAND_COOLDOWN_MS) return;
        if (msg.messageID == lastMessageID) return;
        
        lastMessageID = msg.messageID;
        lastUpdateID = msg.update_id + 1;
        saveLastUpdateID();
        
        lastUserChatID = msg.chatID.toInt();
        lastUserCommandTime = millis();
        
        if (millis() - startupTime < startupDelay) return;
        if (msg.chatID == "-1001819803857") return;

        String cid = msg.chatID;
        String rawText = msg.text;
        String cmd = rawText;
        cmd.toLowerCase();

        if (cmd == "/autoon") {
            autoReportEnabled = true;
            autoReportTicker.attach(reportInterval, autoReportTick);
            saveSettings();
            sendMsg("Автоотчёт включён", cid);
        }
        else if (cmd == "/autooff") {
            autoReportEnabled = false;
            autoReportTicker.detach();
            saveSettings();
            sendMsg("Автоотчёт выключен", cid);
        }
        else if (cmd.startsWith("/setinterval ")) {
            int val = rawText.substring(13).toInt();
            if (val >= 30) {
                reportInterval = val;
                if (autoReportEnabled) {
                    autoReportTicker.detach();
                    autoReportTicker.attach(reportInterval, autoReportTick);
                }
                saveSettings();
                sendMsg("Интервал установлен: " + String(reportInterval) + " сек.", cid);
            } else sendMsg("Минимум 30 секунд", cid);
        }
        else if (cmd.startsWith("/setwifi ")) {
            int sp = rawText.indexOf(' ', 9);
            if (sp != -1) {
                String ssid = rawText.substring(9, sp);
                String pass = rawText.substring(sp + 1);
                if (!isValidWiFiString(ssid) || !isValidWiFiString(pass)) {
                    sendMsg("Неверный формат SSID/PASSWORD", cid); return;
                }
                WiFi.begin(ssid.c_str(), pass.c_str());
                for (int i = 0; i < 10; i++) {
                    if (WiFi.status() == WL_CONNECTED) {
                        wifiSSID = ssid; wifiPASS = pass;
                        saveSettings();
                        sendMsg("✅ Успешно. Перезагрузка...", cid);
                        delay(5000);
                        unsigned long start = millis();
                        while (millis() - start < 3000) { bot.tick(); delay(10); }
                        ESP.restart();
                    }
                    delay(10000);
                }
                sendMsg("❌ Не удалось подключиться. Проверь данные.", cid);
            } else sendMsg("Используй: /setwifi SSID PASSWORD", cid);
        }
        else if (cmd == "/status") {
            // --- Аптайм ---
            unsigned long uptimeMillis = millis();
            unsigned long seconds = uptimeMillis / 1000;
            unsigned long minutes = seconds / 60;
            unsigned long hours = minutes / 60;
            unsigned long days = hours / 24;
            seconds %= 60; minutes %= 60; hours %= 24;
            String uptimeMsg = "⏱ Аптайм: ";
            if (days > 0) uptimeMsg += String(days) + " д ";
            if (hours > 0 || days > 0) uptimeMsg += String(hours) + " ч ";
            if (minutes > 0 || hours > 0 || days > 0) uptimeMsg += String(minutes) + " м ";
            uptimeMsg += String(seconds) + " с";

            // --- Датчик ---
            float t = sht31.readTemperature();
            float h = sht31.readHumidity();
            if (isnan(t) || isnan(h)) {
                reportError("Ошибка чтения SHT31 при /status.");
                sendMsg("❌ Ошибка датчика", cid);
                return;
            }

            // --- Базовый статус (всегда) ---
            String status = "📟 **ПОЛНЫЙ СТАТУС** 📟\n\n";
            status += "🌡 Температура: " + String(t, 2) + " °C\n";
            status += "💧 Влажность: " + String(h, 2) + " %\n";
            status += "🌐 IP: " + WiFi.localIP().toString() + "\n";
            status += uptimeMsg + "\n";
            
            // Обновляем время
            timeClient.update();
            int ho = timeClient.getHours();
            int m = timeClient.getMinutes();
            String timeStr = (ho >= 0 && ho <= 23 && m >= 0 && m <= 59)
                ? String(ho) + ":" + (m < 10 ? "0" : "") + String(m)
                : "не синхронизировано";

            status += "   • Текущее время: " + timeStr + "\n\n";

            // --- Статус термостата (всегда) ---
            if (thermostatEnabled) {
                status += "🔥 Термостат: **включён**\n";
            } else {
                status += "❄️ Термостат: **выключен**\n";
            }
            status += "   • Режим: " + String(workModeEnabled ? "РАБОТА" : "АВТО") + "\n";
            
            // --- Детали котла ---
            if (thermostatEnabled) {
                float currentTarget = getCurrentTargetTemp();
                status += "   • Цель: **" + String(currentTarget, 1) + " °C**\n";

                // Время последнего включения
                String lastOnStr = "никогда";
                if (lastRelayOnTime > 0) {
                    unsigned long elapsed = millis() - lastRelayOnTime;
                    unsigned long secs = elapsed / 1000;
                    unsigned long mins = secs / 60;
                    unsigned long hrs = mins / 60;
                    secs %= 60; mins %= 60;
                    if (hrs > 0) lastOnStr = String(hrs) + " ч " + String(mins) + " м назад";
                    else if (mins > 0) lastOnStr = String(mins) + " м " + String(secs) + " с назад";
                    else lastOnStr = String(secs) + " с назад";
                }
                // Время последнего выключения
                String lastOffStr = "никогда";
                if (lastRelayTurnOffTime > 0) {
                    unsigned long elapsed = millis() - lastRelayTurnOffTime;
                    unsigned long secs = elapsed / 1000;
                    unsigned long mins = secs / 60;
                    unsigned long hrs = mins / 60;
                    secs %= 60; mins %= 60;
                    if (hrs > 0) lastOffStr = String(hrs) + " ч " + String(mins) + " м назад";
                    else if (mins > 0) lastOffStr = String(mins) + " м " + String(secs) + " с назад";
                    else lastOffStr = String(secs) + " с назад";
                }
                
                status += "   • Последнее включение: **" + lastOnStr + "**\n";
                status += "   • Последнее выключение: **" + lastOffStr + "**\n";
                status += "   • Реле: " + String(relayState ? "включено" : "выключено") + "\n";
                status += "   • Гистерезис: " + String(hysteresis, 1) + " °C\n";
                status += "   • Макс. работа: " + String(workDurationMinutes) + " мин\n";
                status += "   • Мин. пауза: " + String(minStartInterval / 60000UL) + " мин\n";
                status += "   • Зон в расписании: " + String(zoneCount) + "\n";
            }
            sendMsg(status, cid);
        }
        else if (cmd == "/reboot") {
            if (millis() - startupTime < 30000) { sendMsg("⏱ Ещё рано, подождите немного...", cid); return; }
            sendMsg("🔄 Перезагрузка через 3 секунды...", cid);
            delay(500); bot.tick();
            WiFi.disconnect(true); delay(100);
            ESP.restart();
        }
        else if (cmd == "/help") {
            String helpMsg = "🆘 Доступные команды:\n\n";
            helpMsg += "/status — статус устройства, аптайм, температура, влажность\n";
            helpMsg += "/autoon — включить автоотчёт в Telegram\n";
            helpMsg += "/autooff — отключить автоотчёт\n";
            helpMsg += "/setinterval N — установить интервал автоотчёта (в секундах, минимум 30)\n";
            helpMsg += "/setwifi SSID PASSWORD — задать Wi-Fi\n";
            helpMsg += "/reboot — перезагрузка устройства\n";
            helpMsg += "/help — показать эту справку\n\n";
            helpMsg += "🔥 Термостат:\n";
            helpMsg += "/thermo on/off — включить/выключить термостат\n";
            helpMsg += "/addzone с ЧЧ:ММ до ЧЧ:ММ ТЕМП  — добавить зону (например: 7:00-22:00 22.5)\n";
            helpMsg += "/delzone N — удалить зону по номеру\n";
            helpMsg += "/zones — показать расписание\n";
            helpMsg += "/sethyst 0.5 — гистерезис (0.1–2.0 °C)\n";
            helpMsg += "/setworktime N — макс. время работы реле (1–60 мин)\n";
            helpMsg += "/setminpause N — мин. пауза между включениями (1–60 мин)\n";
            helpMsg += "/clearzones - удалить все зоны из расписания\n";
            sendMsg(helpMsg, cid);
        }
        else if (cmd == "/forceon") {
            bool success = sendRelayCommand(true);
            if (success) { relayState = true; sendMsg("✅ ФОРСИРОВАННОЕ ВКЛЮЧЕНИЕ: команда успешно отправлена", cid); } 
            else sendMsg("❌ Реле не отвечает (force ON)", cid);
        }
        else if (cmd == "/forceoff") {
            bool success = sendRelayCommand(false);
            if (success) { relayState = false; sendMsg("✅ ФОРСИРОВАННОЕ ВЫКЛЮЧЕНИЕ: команда успешно отправлена", cid); } 
            else sendMsg("❌ Реле не отвечает (force OFF)", cid);
        }
        else if (cmd == "/thermo on") {
            thermostatEnabled = true;
            EEPROM.begin(EEPROM_SIZE_BOILER);
            EEPROM.put(EEPROM_THERMO_ENABLE, thermostatEnabled);
            EEPROM.commit(); EEPROM.end();
            sendMsg("✅ Термостат включён", cid);
        }
        else if (cmd == "/thermo off") {
            thermostatEnabled = false;
            sendRelayCommand(false); relayState = false;
            EEPROM.begin(EEPROM_SIZE_BOILER);
            EEPROM.put(EEPROM_THERMO_ENABLE, thermostatEnabled);
            EEPROM.commit(); EEPROM.end();
            sendMsg("❌ Термостат выключен", cid);
        }
        else if (cmd.startsWith("/sethyst ")) {
            float val = rawText.substring(9).toFloat();
            if (val >= 0.1 && val <= 2.0) {
                hysteresis = val;
                EEPROM.begin(EEPROM_SIZE_BOILER);
                EEPROM.put(EEPROM_HYSTERESIS, hysteresis);
                EEPROM.commit(); EEPROM.end();
                sendMsg("Гистерезис: " + String(hysteresis, 1) + " °C", cid);
            } else sendMsg("0.1–2.0 °C", cid);
        }
        else if (cmd == "/zones") {
            String msg = "⏰ Расписание (" + String(zoneCount) + "):\n";
            for (int i = 0; i < zoneCount; i++) {
                String start = String(timeZones[i].startHour) + ":" + (timeZones[i].startMinute < 10 ? "0" : "") + String(timeZones[i].startMinute);
                String end = String(timeZones[i].endHour) + ":" + (timeZones[i].endMinute < 10 ? "0" : "") + String(timeZones[i].endMinute);
                msg += String(i+1) + ". " + start + "–" + end + " → " + String(timeZones[i].temp, 1) + "°C\n";
            }
            if (zoneCount == 0) msg += "Нет зон. Пример: /addzone 7:00-22:00 22.5";
            sendMsg(msg, cid);
        }
        else if (cmd.startsWith("/addzone ")) {
            if (zoneCount >= MAX_ZONES) { sendMsg("⛔ Лимит зон: " + String(MAX_ZONES), cid); return; }
            String data = rawText.substring(9);
            int sep = data.indexOf(' ');
            if (sep == -1) { sendMsg("📌 Формат: /addzone ЧЧ:ММ-ЧЧ:ММ ТЕМП (например: 7:00-22:00 22.5)", cid); return; }
            String timeRange = data.substring(0, sep);
            float temp = data.substring(sep + 1).toFloat();
            if (temp < 5 || temp > 30) { sendMsg("🌡 Температура должна быть от 5 до 30°C", cid); return; }
            int dash = timeRange.indexOf('-');
            if (dash == -1) { sendMsg("⏰ Используйте формат ЧЧ:ММ-ЧЧ:ММ", cid); return; }
            String startStr = timeRange.substring(0, dash);
            String endStr = timeRange.substring(dash + 1);
            int colon1 = startStr.indexOf(':');
            if (colon1 == -1) { sendMsg("Неверный формат времени", cid); return; }
            int h1 = startStr.substring(0, colon1).toInt();
            int m1 = startStr.substring(colon1 + 1).toInt();
            int colon2 = endStr.indexOf(':');
            if (colon2 == -1) { sendMsg("Неверный формат времени", cid); return; }
            int h2 = endStr.substring(0, colon2).toInt();
            int m2 = endStr.substring(colon2 + 1).toInt();
            if (h1 < 0 || h1 > 23 || m1 < 0 || m1 > 59 || h2 < 0 || h2 > 23 || m2 < 0 || m2 > 59) {
                sendMsg("🕒 Время должно быть в диапазоне 00:00–23:59", cid); return;
            }
            timeZones[zoneCount++] = { (uint8_t)h1, (uint8_t)m1, (uint8_t)h2, (uint8_t)m2, temp };
            zonesChanged = true; saveZones();
            String startFmt = String(h1) + ":" + (m1 < 10 ? "0" : "") + String(m1);
            String endFmt = String(h2) + ":" + (m2 < 10 ? "0" : "") + String(m2);
            sendMsg("✅ Добавлен интервал: " + startFmt + "–" + endFmt + " → " + String(temp, 1) + "°C", cid);
        }
        else if (cmd.startsWith("/delzone ")) {
            int idx = rawText.substring(9).toInt() - 1;
            if (idx < 0 || idx >= zoneCount) { sendMsg("❌ Такой зоны нет", cid); return; }
            for (int i = idx; i < zoneCount - 1; i++) timeZones[i] = timeZones[i + 1];
            zoneCount--; zonesChanged = true; saveZones();
            sendMsg("🗑 Удалено", cid);
        }
        else if (cmd == "/clearzones") {
            zoneCount = 0; zonesChanged = true; saveZones();
            sendMsg("🗑 Все зоны удалены", cid);
        }
        else if (cmd.startsWith("/setworktime ")) {
            int val = rawText.substring(13).toInt();
            if (val >= 1 && val <= 60) {
                extern uint16_t workDurationMinutes; extern void saveWorkDuration();
                workDurationMinutes = val; saveWorkDuration();
                sendMsg("⏱ Макс. время работы: " + String(workDurationMinutes) + " мин", cid);
            } else sendMsg("Укажите от 1 до 60 минут", cid);
        }
        else if (cmd.startsWith("/setminpause ")) {
            int val = rawText.substring(13).toInt();
            if (val >= 1 && val <= 60) {
                minStartInterval = val * 60 * 1000UL;
                saveMinStartInterval();
                sendMsg("⏸ Мин. пауза между включениями: " + String(val) + " мин", cid);
            } else sendMsg("Укажите от 1 до 60 минут", cid);
        }
        else if (cmd.startsWith("/work ")) {
            String interval = rawText.substring(6);
            int dash = interval.indexOf('-');
            if (dash == -1) { sendMsg("📌 Формат: /work ЧЧ:ММ-ЧЧ:ММ", cid); return; }
            String startStr = interval.substring(0, dash);
            String endStr = interval.substring(dash + 1);
            int colon1 = startStr.indexOf(':');
            int colon2 = endStr.indexOf(':');
            if (colon1 == -1 || colon2 == -1) { sendMsg("📌 Формат времени неверен", cid); return; }
            uint8_t h1 = startStr.substring(0, colon1).toInt();
            uint8_t m1 = startStr.substring(colon1 + 1).toInt();
            uint8_t h2 = endStr.substring(0, colon2).toInt();
            uint8_t m2 = endStr.substring(colon2 + 1).toInt();
            if (h1 > 23 || m1 > 59 || h2 > 23 || m2 > 59) { sendMsg("🕒 Время должно быть в диапазоне 00:00–23:59", cid); return; }
            workStartHour = h1; workStartMinute = m1; workEndHour = h2; workEndMinute = m2;
            workScheduled = true; workModeEnabled = true;
            sendMsg("✅ Включён режим РАБОТА (22°C) с " + startStr + " до " + endStr, cid);
        }
        else if (cmd == "/auto") {
            workModeEnabled = false;
            sendMsg("✅ Режим: АВТО (по расписанию)", cid);
        }
        else {
            sendMsg("❓ Неизвестная команда. Напишите /help", cid);
        }
    });
    
    setupOTA();
    setupBoilerCommands();
}

void loop() {
    bot.tick();
    if (needToSendAutoReport) {
        needToSendAutoReport = false;
        sendToTelegram("619084238");
    }
    if (WiFi.status() == WL_CONNECTED && millis() - lastNarodMonMeasurementTime >= NARODMON_MEAS_INTERVAL && narodMonMeasurementCount < 3) {
        float t = sht31.readTemperature();
        if (!isnan(t)) { tempSumForNarodMon += t; narodMonMeasurementCount++; lastNarodMonMeasurementTime = millis(); }
    }
    if (sendtonm) { sendtonm = false; sendSHT31Data(); }
    ArduinoOTA.handle();
    boilerLoop();
    if (WiFi.status() != WL_CONNECTED) {
        serialToTelegram("📡 Wi-Fi lost. Reconnecting...");
        WiFiMulti.run();
        delay(1000);
    }
    yield();
}