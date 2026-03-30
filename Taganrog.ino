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
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include "boiler.h"

#define EEPROM_SIZE 512
#define BOT_TOKEN "8079276277:AAHrqQKTo3vp76bcX2ekPw59dwWxRvTaEHg"
#define ADMIN_CHAT_ID "-4647981556"
// --- Для поиска boiler.local ---
unsigned long lastRelayErrorTime = 0; // ← НОВАЯ ПЕРЕМЕННАЯ: время последней ошибки реле
const unsigned long RELAY_ERROR_COOLDOWN = 5 * 60 * 1000; // 5 минут в мс
unsigned long lastRelayMsgTime = 0;         // время последнего сообщения про реле
const unsigned long RELAY_MSG_INTERVAL = 60000; // интервал между сообщениями, мс (например, 1 минута)

ESP8266WiFiMulti WiFiMulti;
WiFiClient client;
FastBot bot(BOT_TOKEN);
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Ticker autoReportTicker;
Ticker narodmonTicker;

// === Для одноразового режима /work ===
bool workScheduled = false;          // активен ли временной интервал
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
// Адрес в EEPROM для хранения lastMessageID (в пределах EEPROM_SIZE)
#define EEPROM_LAST_UPDATE_ID 100  // адрес в EEPROM (например 100–103)
// Глобальные переменные для усреднения
float tempSumForNarodMon = 0.0;
uint8_t narodMonMeasurementCount = 0;
unsigned long lastNarodMonMeasurementTime = 0;
const unsigned long NARODMON_MEAS_INTERVAL = 100000; // 100 секунд

enum TelegramEndpointMode : uint8_t {
  TELEGRAM_ENDPOINT_API = 0,
  TELEGRAM_ENDPOINT_CF = 1
};

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
    lastUpdateID = tmp;
    EEPROM.end();
}
String urlencode(const String& str) {
  String encoded = "";
  const char *cstr = str.c_str();

  for (size_t i = 0; i < strlen(cstr); i++) {
    unsigned char c = cstr[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

TelegramEndpointMode activeTelegramEndpoint = TELEGRAM_ENDPOINT_API;
TelegramEndpointMode manualTelegramEndpoint = TELEGRAM_ENDPOINT_API;
bool telegramEndpointManualMode = false;
bool isEndpointNotifyInProgress = false;
unsigned long lastTelegramEndpointCheck = 0;
const unsigned long TELEGRAM_ENDPOINT_CHECK_INTERVAL_MS = 60000;

bool isHttpsHostReachable(const char* host, uint16_t port = 443) {
  WiFiClientSecure testClient;
  testClient.setInsecure();
  testClient.setTimeout(2500);
  bool connected = testClient.connect(host, port);
  testClient.stop();
  return connected;
}

bool sendViaCloudflareWorker(const String& text, const String& chatId) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  String url = "https://royal-river-71a9.dragonforceedge.workers.dev/bot" + String(BOT_TOKEN) +
               "/sendMessage?chat_id=" + chatId +
               "&text=" + urlencode(text);

  http.setTimeout(5000);
  if (!http.begin(secureClient, url)) {
    return false;
  }

  int httpCode = http.GET();
  bool success = (httpCode > 0 && httpCode < 400);
  http.end();
  return success;
}

void sendViaTelegramApi(const String& text, const String& chatId) {
  bot.sendMessage(urlencode(text), chatId);
}

void notifyEndpointChange(TelegramEndpointMode fromMode, TelegramEndpointMode toMode, bool manualSwitch) {
  if (isEndpointNotifyInProgress || WiFi.status() != WL_CONNECTED) return;
  isEndpointNotifyInProgress = true;

  String fromText = (fromMode == TELEGRAM_ENDPOINT_API) ? "api.telegram.org" : "Cloudflare Worker";
  String toText = (toMode == TELEGRAM_ENDPOINT_API) ? "api.telegram.org" : "Cloudflare Worker";
  String reason = manualSwitch ? "ручной выбор" : "автоматическое переключение";
  String msg = "🔀 Сервер отправки переключен: " + fromText + " → " + toText + " (" + reason + ")";

  if (toMode == TELEGRAM_ENDPOINT_CF) {
    if (!sendViaCloudflareWorker(msg, ADMIN_CHAT_ID)) {
      sendViaTelegramApi(msg, ADMIN_CHAT_ID);
    }
  } else {
    sendViaTelegramApi(msg, ADMIN_CHAT_ID);
  }

  isEndpointNotifyInProgress = false;
}

void refreshTelegramEndpoint(bool force = false) {
  if (telegramEndpointManualMode) {
    activeTelegramEndpoint = manualTelegramEndpoint;
    return;
  }

  if (!force && (millis() - lastTelegramEndpointCheck < TELEGRAM_ENDPOINT_CHECK_INTERVAL_MS)) return;
  lastTelegramEndpointCheck = millis();

  TelegramEndpointMode prevMode = activeTelegramEndpoint;
  bool apiOk = isHttpsHostReachable("api.telegram.org");
  bool cfOk = isHttpsHostReachable("royal-river-71a9.dragonforceedge.workers.dev");

  if (apiOk) activeTelegramEndpoint = TELEGRAM_ENDPOINT_API;
  else if (cfOk) activeTelegramEndpoint = TELEGRAM_ENDPOINT_CF;

  if (prevMode != activeTelegramEndpoint) {
    Serial.println(activeTelegramEndpoint == TELEGRAM_ENDPOINT_API
      ? "✅ Telegram endpoint switched to api.telegram.org"
      : "✅ Telegram endpoint switched to Cloudflare Worker");
    notifyEndpointChange(prevMode, activeTelegramEndpoint, false);
  }
}

void sendMsg(String text, String chatId) {
  refreshTelegramEndpoint();

  if (activeTelegramEndpoint == TELEGRAM_ENDPOINT_CF) {
    if (!sendViaCloudflareWorker(text, chatId)) {
      if (telegramEndpointManualMode) {
        sendViaTelegramApi(text, chatId);
        return;
      }
      TelegramEndpointMode prevMode = activeTelegramEndpoint;
      activeTelegramEndpoint = TELEGRAM_ENDPOINT_API;
      notifyEndpointChange(prevMode, activeTelegramEndpoint, false);
      sendViaTelegramApi(text, chatId);
    }
    return;
  }

  sendViaTelegramApi(text, chatId);
}
// === Защита от дубликатов и спама ===
long lastUserChatID = 0;
unsigned long lastUserCommandTime = 0;
const unsigned long COMMAND_COOLDOWN_MS = 2000; // 2 секунды между командами одного пользователя

// === ID вашего бота — ОБЯЗАТЕЛЬНО ЗАМЕНИТЕ НА СВОЙ! ===
const long BOT_ID = 530615559; // ← ЗАМЕНИТЕ НА СВОЙ ID ИЗ @BotFather!
// --- Перенаправление Serial.println() в Telegram ---
void serialToTelegram(const String &message) {
    // Отправляем в Telegram, но только если WiFi есть — чтобы не спамить при отключении
    if (WiFi.status() == WL_CONNECTED) {
        sendMsg("📌 Лог: " + message, ADMIN_CHAT_ID);
    }
    // Если WiFi нет — не шлём, чтобы не засорять очередь
}
bool shouldSendError() {
  static unsigned long lastErrorMsg = 0;
  static unsigned long errorCount = 0;
  static unsigned long errorInterval = 60000; // старт — 1 минута

  if (millis() - lastErrorMsg > errorInterval) {
    lastErrorMsg = millis();
    errorCount++;

    if (errorCount % 10 == 0) {
      errorInterval += 120000; // каждые 10 ошибок +2 мин
    }

    return true;
  }
  return false;
}
void reportError(const String &message)
{
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    if (WiFi.status() == WL_CONNECTED)
    {
        sendMsg("🚨 Ошибка: " + message + "\nID: " + mac, ADMIN_CHAT_ID);
    }
    else
    {
        serialToTelegram("🚨 Ошибка: " + message);
        serialToTelegram("ID: " + mac);
    }
}

void loadSettings()
{
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(0, autoReportEnabled);
    EEPROM.get(4, reportInterval);
    char ssidBuf[32], passBuf[32];
    EEPROM.get(8, ssidBuf);
    EEPROM.get(40, passBuf);
    EEPROM.end();

    ssidBuf[31] = '\0';
    passBuf[31] = '\0';

    Serial.print("📦 RAW ssidBuf: ");
    for (int i = 0; i < 32; i++)
    {
        Serial.print((uint8_t)ssidBuf[i], HEX);
        Serial.print(" ");
    }
    Serial.println();

    Serial.print("📦 RAW passBuf: ");
    for (int i = 0; i < 32; i++)
    {
        Serial.print((uint8_t)passBuf[i], HEX);
        Serial.print(" ");
    }
    Serial.println();

    wifiSSID = String(ssidBuf);
    wifiPASS = String(passBuf);

    if ((uint8_t)ssidBuf[0] == 0xFF || ssidBuf[0] == '\0' || wifiSSID.length() == 0)
    {
        wifiSSID = "Pogoda";
        wifiPASS = "yeea6jxp";
        saveSettings();
    }

    if (reportInterval < 30 || reportInterval > 86400)
    {
        reportInterval = 600;
    }
}

void saveSettings()
{
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

bool isValidWiFiString(const String &str)
{
    return str.length() >= 1 && str.length() <= 32;
}

void sendSHT31Data()
{
    if (narodMonMeasurementCount == 0)
    {
        // Нет данных — всё равно попробуем прочитать "свежие", чтобы не молчать
        float t = sht31.readTemperature();
        float h = sht31.readHumidity();
        if (isnan(t) || isnan(h))
        {
            reportError("Нет данных для отправки на NarodMon");
            return;
        }
        // Отправим текущие, но это fallback
        tempSumForNarodMon = t;
        narodMonMeasurementCount = 1;
    }

    float avgTemp = tempSumForNarodMon / narodMonMeasurementCount;
    float h = sht31.readHumidity(); // влажность берём актуальную на момент отправки

    if (isnan(h))
    {
        reportError("Не удалось прочитать влажность при отправке на NarodMon");
        return;
    }

    String data = "#ESPT" + WiFi.macAddress() + "\n";
    data.replace(":", "");
    data += "#Tempe#" + String(avgTemp, 2) + "\n";
    data += "#Vlazno#" + String(h, 2) + "\n##";

    const int maxRetries = 5;
    bool success = false;
    for (int i = 0; i < maxRetries; ++i)
    {
        if (client.connect("narodmon.ru", 8283))
        {
            client.print(data);
            client.stop();
            success = true;
            break;
        }
        delay(2000);
    }

    if (success)
    {
        // ✅ Успешно — сбрасываем накопленное
        tempSumForNarodMon = 0.0;
        narodMonMeasurementCount = 0;
        lastNarodMonMeasurementTime = millis(); // начинаем новый цикл
    }
    else
    {
        reportError("Не удалось отправить данные на NarodMon.");
        // ❌ Не сбрасываем — попробуем отправить накопленное снова в следующий раз
    }
}

void sendToTelegram(String cid)
{
    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    if (isnan(t) || isnan(h))
    {
        reportError("Ошибка чтения SHT31 при отчёте в Telegram.");
        return;
    }

    String msg = "🌡 Температура: " + String(t, 2) + " °C\n💧 Влажность: " + String(h, 2) + " %";
    sendMsg(msg, cid);
}

void setupOTA()
{
    ArduinoOTA.setHostname("ESP-Tag");
    ArduinoOTA.setPassword("12345678");
    ArduinoOTA.onError([](ota_error_t error)
    {
        reportError("Ошибка OTA: " + String(error));
    });
    ArduinoOTA.begin();
}

void autoReportTick()
{
    needToSendAutoReport = true;
}

void narodmonTick()
{
    sendtonm = true;
}

void setup()
{
    Serial.begin(115200);
    delay(3000);
    serialToTelegram("🔧 Старт setup()...");
    startupTime = millis();
    loadSettings();
    loadLastUpdateID();
    serialToTelegram("📶 Попытка подключения к Wi-Fi:");
    serialToTelegram("SSID: " + wifiSSID);
    serialToTelegram("PASS: " + wifiPASS);
    WiFi.mode(WIFI_STA);
    WiFiMulti.addAP(wifiSSID.c_str(), wifiPASS.c_str());
    bool connected = false;
    if (WiFiMulti.run() == WL_CONNECTED)
    {
        connected = true;
    }
    else
    {
      delay(120000);
      if (WiFiMulti.run() == WL_CONNECTED)
      {
        connected = true;
      }
    }
    if (!connected)
    {
        WiFiMulti = ESP8266WiFiMulti();
        WiFiMulti.addAP("Pogoda", "yeea6jxp");
        for (int i = 0; i < 5; i++)
        {
            if (WiFiMulti.run() == WL_CONNECTED)
            {
                connected = true;
                break;
            }
            delay(15000);
        }
    }
    if (!connected)
    {
        reportError("Не удалось подключиться к Wi-Fi. Работа в оффлайне.");
    }
    else
    {
        refreshTelegramEndpoint(true);
        delay(5000);
        sendMsg("🤖 Устройство перезапущено", "-1001819803857");
        sendMsg("🤖 Device has been restarted", "-1001819803857");
        narodmonTicker.attach(303, narodmonTick);
        // Синхронизация NTP
    timeClient.begin();
    unsigned long startWait = millis();
    while (!timeClient.update() && millis() - startWait < 10000) {
        delay(100);
    }
    }
    if (!sht31.begin(0x44))
    {
        reportError("Не найден датчик SHT31!");
    }
    if (autoReportEnabled)
    {
        autoReportTicker.attach(reportInterval, autoReportTick);
    }
    bot.attach([](FB_msg& msg)
{
    // --- 2. Коулдаун: не реагируем чаще, чем раз в 2 секунды на одного пользователя ---
   if (msg.chatID.toInt() == lastUserChatID && millis() - lastUserCommandTime < COMMAND_COOLDOWN_MS) {
        return;
    }

    // --- 3. Защита от дубликатов по message_id ---
    if (msg.messageID == lastMessageID) return;
    lastUpdateID = msg.update_id;
    saveLastUpdateID();

    // --- 4. Сохраняем отправителя и время команды ---
    lastUserChatID = msg.chatID.toInt();
    lastUserCommandTime = millis();

    // --- 5. Ждём 5 сек после старта ---
    if (millis() - startupTime < startupDelay) return;

    // --- 6. Игнорируем группы (если нужно) ---
    if (msg.chatID == "-1001819803857") return;

    String cid = msg.chatID;
    String rawText = msg.text;
    String cmd = rawText;
    cmd.toLowerCase();

    // --- ОСНОВНЫЕ КОМАНДЫ ---
    if (cmd == "/autoon")
    {
        autoReportEnabled = true;
        autoReportTicker.attach(reportInterval, autoReportTick);
        saveSettings();
        sendMsg("Автоотчёт включён", cid);
    }
    else if (cmd == "/autooff")
    {
        autoReportEnabled = false;
        autoReportTicker.detach();
        saveSettings();
        sendMsg("Автоотчёт выключен", cid);
    }
    else if (cmd.startsWith("/setinterval "))
    {
        int val = rawText.substring(13).toInt();
        if (val >= 30)
        {
            reportInterval = val;
            if (autoReportEnabled)
            {
                autoReportTicker.detach();
                autoReportTicker.attach(reportInterval, autoReportTick);
            }
            saveSettings();
            sendMsg("Интервал установлен: " + String(reportInterval) + " сек.", cid);
        }
        else
        {
            sendMsg("Минимум 30 секунд", cid);
        }
    }
    else if (cmd.startsWith("/setwifi "))
    {
        int sp = rawText.indexOf(' ', 9);
        if (sp != -1)
        {
            String ssid = rawText.substring(9, sp);
            String pass = rawText.substring(sp + 1);
            if (!isValidWiFiString(ssid) || !isValidWiFiString(pass))
            {
                sendMsg("Неверный формат SSID/PASSWORD", cid);
                return;
            }
            WiFi.begin(ssid.c_str(), pass.c_str());
            for (int i = 0; i < 10; i++)
            {
                if (WiFi.status() == WL_CONNECTED)
                {
                    wifiSSID = ssid;
                    wifiPASS = pass;
                    saveSettings();
                    sendMsg("✅ Успешно. Перезагрузка...", cid);
                    delay(5000);
                    unsigned long start = millis();
                    while (millis() - start < 3000)
                    {
                        bot.tick();
                        delay(10);
                    }
                    ESP.restart();
                }
                delay(10000);
            }
            sendMsg("❌ Не удалось подключиться. Проверь данные.", cid);
        }
        else
        {
            sendMsg("Используй: /setwifi SSID PASSWORD", cid);
        }
    }
    else if (cmd == "/status")
{
    // --- Аптайм ---
    unsigned long uptimeMillis = millis();
    unsigned long seconds = uptimeMillis / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;
    seconds %= 60;
    minutes %= 60;
    hours %= 24;
    String uptimeMsg = "⏱ Аптайм: ";
    if (days > 0) uptimeMsg += String(days) + " д ";
    if (hours > 0 || days > 0) uptimeMsg += String(hours) + " ч ";
    if (minutes > 0 || hours > 0 || days > 0) uptimeMsg += String(minutes) + " м ";
    uptimeMsg += String(seconds) + " с";

    // --- Датчик ---
    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    if (isnan(t) || isnan(h))
    {
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
    // Обновляем время (даже если не синхронизировано)
timeClient.update();
int ho = timeClient.getHours();
int m = timeClient.getMinutes();

// Проверяем, валидное ли время (если не синхронизировано — будет 0:00 или мусор)
String timeStr = (ho >= 0 && ho <= 23 && m >= 0 && m <= 59)
    ? String(ho) + ":" + (m < 10 ? "0" : "") + String(m)
    : "не синхронизировано";

status += "   • Текущее время: " + timeStr + "\n\n";

    // --- Статус термостата (всегда) ---
    if (thermostatEnabled)
    {
        status += "🔥 Термостат: **включён**\n";
    }
    else
    {
        status += "❄️ Термостат: **выключен**\n";
    }
  status += "   • Режим: " + String(workModeEnabled ? "РАБОТА" : "АВТО") + "\n";
    // --- Вся "хуйня" про котёл — ТОЛЬКО если термостат включён ---
    if (thermostatEnabled)
    {
        float currentTarget = getCurrentTargetTemp();
        status += "   • Цель: **" + String(currentTarget, 1) + " °C**\n";

        // Время последнего включения
        String lastOnStr = "никогда";
        if (lastRelayOnTime > 0)
        {
            unsigned long elapsed = millis() - lastRelayOnTime;
            unsigned long secs = elapsed / 1000;
            unsigned long mins = secs / 60;
            unsigned long hrs = mins / 60;
            secs %= 60;
            mins %= 60;
            if (hrs > 0) lastOnStr = String(hrs) + " ч " + String(mins) + " м назад";
            else if (mins > 0) lastOnStr = String(mins) + " м " + String(secs) + " с назад";
            else lastOnStr = String(secs) + " с назад";
        }
        // Время последнего выключения
        String lastOffStr = "никогда";
        if (lastRelayTurnOffTime > 0)
        {
            unsigned long elapsed = millis() - lastRelayTurnOffTime;
            unsigned long secs = elapsed / 1000;
            unsigned long mins = secs / 60;
            unsigned long hrs = mins / 60;
            secs %= 60;
            mins %= 60;
            if (hrs > 0) lastOffStr = String(hrs) + " ч " + String(mins) + " м назад";
            else if (mins > 0) lastOffStr = String(mins) + " м " + String(secs) + " с назад";
            else lastOffStr = String(secs) + " с назад";
        }
        
        status += "   • Последнее включение: **" + lastOnStr + "**\n";
        status += "   • Последнее выключение: **" + lastOffStr + "**\n";

        // Реле
        status += "   • Реле: " + String(relayState ? "включено" : "выключено") + "\n";

        // Гистерезис
        status += "   • Гистерезис: " + String(hysteresis, 1) + " °C\n";

        // Макс. время работы
        status += "   • Макс. работа: " + String(workDurationMinutes) + " мин\n";
        
        // Мин. пауза между включениями 
        status += "   • Мин. пауза: " + String(minStartInterval / 60000UL) + " мин\n";
        
        // Зоны
        status += "   • Зон в расписании: " + String(zoneCount) + "\n";
    }

    sendMsg(status, cid);
}
  else if (cmd == "/reboot")
{
    if (millis() - startupTime < 30000) {
        sendMsg("⏱ Ещё рано, подождите немного...", cid);
        return;
    }

    sendMsg("🔄 Перезагрузка через 3 секунды...", cid);
    delay(500);             // дать телеге успеть отправить
    bot.tick();             // принудительно обработать очередь FastBot

    WiFi.disconnect(true);  // полностью отключаем Wi-Fi
    delay(100);             // ждём немного
    ESP.restart();          // перезапуск без подвисания
}

    else if (cmd == "/help")
    {
        String helpMsg = "🆘 Доступные команды:\n\n";
        helpMsg += "/status — статус устройства, аптайм, температура, влажность\n";
        helpMsg += "/autoon — включить автоотчёт в Telegram\n";
        helpMsg += "/autooff — отключить автоотчёт\n";
        helpMsg += "/setinterval N — установить интервал автоотчёта (в секундах, минимум 30)\n";
        helpMsg += "/setwifi SSID PASSWORD — задать Wi-Fi\n";
        helpMsg += "/tgserver auto|api|cf — выбор сервера отправки Telegram\n";
        helpMsg += "/reboot — перезагрузка устройства\n";
        helpMsg += "/help — показать эту справку\n\n";
        helpMsg += "🔥 Термостат:\n";
        helpMsg += "/thermo on/off — включить/выключить термостат\n";
        helpMsg += "/addzone с ЧЧ:ММ до ЧЧ:ММ ТЕМП  — добавить зону (например: 7:00-22:00 22.5)\n";
        helpMsg += "/delzone N — удалить зону по номеру\n";
        helpMsg += "/zones — показать расписание\n";
        helpMsg += "/sethyst 0.5 — гистерезис (0.1–2.0 °C)";
        helpMsg += "/setworktime N — макс. время работы реле (1–60 мин)\n";
        helpMsg += "/setminpause N — мин. пауза между включениями (1–60 мин)\n";
        helpMsg += "/clearzones - удалить все зоны из расписания\n";
        sendMsg(helpMsg, cid);
    }
else if (cmd == "/tgserver" || cmd == "/tgserver status") {
    String mode = telegramEndpointManualMode ? "MANUAL" : "AUTO";
    String endpoint = (activeTelegramEndpoint == TELEGRAM_ENDPOINT_API) ? "api.telegram.org" : "Cloudflare Worker";
    sendMsg("🌐 Режим отправки: " + mode + "\nТекущий сервер: " + endpoint, cid);
}
else if (cmd == "/tgserver auto") {
    telegramEndpointManualMode = false;
    refreshTelegramEndpoint(true);
    sendMsg("✅ Режим Telegram-сервера: AUTO", cid);
}
else if (cmd == "/tgserver api") {
    TelegramEndpointMode previous = activeTelegramEndpoint;
    telegramEndpointManualMode = true;
    manualTelegramEndpoint = TELEGRAM_ENDPOINT_API;
    activeTelegramEndpoint = TELEGRAM_ENDPOINT_API;
    sendMsg("✅ Режим Telegram-сервера: MANUAL (api.telegram.org)", cid);
    if (previous != activeTelegramEndpoint) {
      notifyEndpointChange(previous, activeTelegramEndpoint, true);
    }
}
else if (cmd == "/tgserver cf") {
    TelegramEndpointMode previous = activeTelegramEndpoint;
    telegramEndpointManualMode = true;
    manualTelegramEndpoint = TELEGRAM_ENDPOINT_CF;
    activeTelegramEndpoint = TELEGRAM_ENDPOINT_CF;
    sendMsg("✅ Режим Telegram-сервера: MANUAL (Cloudflare Worker)", cid);
    if (previous != activeTelegramEndpoint) {
      notifyEndpointChange(previous, activeTelegramEndpoint, true);
    }
}
else if (cmd == "/forceon") {
    bool success = sendRelayCommand(true);
    if (success) {
        relayState = true;
        sendMsg("✅ ФОРСИРОВАННОЕ ВКЛЮЧЕНИЕ: команда успешно отправлена", cid);
    } else {
        sendMsg("❌ Реле не отвечает (force ON)", cid);
    }
}

else if (cmd == "/forceoff") {
    bool success = sendRelayCommand(false);
    if (success) {
        relayState = false;
        sendMsg("✅ ФОРСИРОВАННОЕ ВЫКЛЮЧЕНИЕ: команда успешно отправлена", cid);
    } else {
        sendMsg("❌ Реле не отвечает (force OFF)", cid);
    }
}

    // === Команды термостата (из boiler.h) ===
    else if (cmd == "/thermo on")
    {
        thermostatEnabled = true;
        EEPROM.begin(EEPROM_SIZE_BOILER);
        EEPROM.put(EEPROM_THERMO_ENABLE, thermostatEnabled);
        EEPROM.commit();
        EEPROM.end();
        sendMsg("✅ Термостат включён", cid);
    }
    else if (cmd == "/thermo off")
    {
        thermostatEnabled = false;
        sendRelayCommand(false);
        relayState = false;
        EEPROM.begin(EEPROM_SIZE_BOILER);
        EEPROM.put(EEPROM_THERMO_ENABLE, thermostatEnabled);
        EEPROM.commit();
        EEPROM.end();
        sendMsg("❌ Термостат выключен", cid);
    }
    else if (cmd.startsWith("/sethyst "))
    {
        float val = rawText.substring(9).toFloat();
        if (val >= 0.1 && val <= 2.0)
        {
            hysteresis = val;
            EEPROM.begin(EEPROM_SIZE_BOILER);
            EEPROM.put(EEPROM_HYSTERESIS, hysteresis);
            EEPROM.commit();
            EEPROM.end();
            sendMsg("Гистерезис: " + String(hysteresis, 1) + " °C", cid);
        }
        else
        {
            sendMsg("0.1–2.0 °C", cid);
        }
    }
   else if (cmd == "/zones") {
    String msg = "⏰ Расписание (" + String(zoneCount) + "):\n";
    for (int i = 0; i < zoneCount; i++) {
        String start = String(timeZones[i].startHour) + ":" + 
                      (timeZones[i].startMinute < 10 ? "0" : "") + String(timeZones[i].startMinute);
        String end = String(timeZones[i].endHour) + ":" + 
                    (timeZones[i].endMinute < 10 ? "0" : "") + String(timeZones[i].endMinute);
        msg += String(i+1) + ". " + start + "–" + end + " → " + String(timeZones[i].temp, 1) + "°C\n";
    }
    if (zoneCount == 0) msg += "Нет зон. Пример: /addzone 7:00-22:00 22.5";
    sendMsg(msg, cid);
}
    else if (cmd.startsWith("/addzone ")) {
    if (zoneCount >= MAX_ZONES) {
        sendMsg("⛔ Лимит зон: " + String(MAX_ZONES), cid);
        return;
    }

    String data = rawText.substring(9);
    int sep = data.indexOf(' ');
    if (sep == -1) {
        sendMsg("📌 Формат: /addzone ЧЧ:ММ-ЧЧ:ММ ТЕМП (например: 7:00-22:00 22.5)", cid);
        return;
    }

    String timeRange = data.substring(0, sep);
    float temp = data.substring(sep + 1).toFloat();

    if (temp < 5 || temp > 30) {
        sendMsg("🌡 Температура должна быть от 5 до 30°C", cid);
        return;
    }

    int dash = timeRange.indexOf('-');
    if (dash == -1) {
        sendMsg("⏰ Используйте формат ЧЧ:ММ-ЧЧ:ММ", cid);
        return;
    }

    String startStr = timeRange.substring(0, dash);
    String endStr   = timeRange.substring(dash + 1);

    // Парсим начало
    int colon1 = startStr.indexOf(':');
    if (colon1 == -1) { sendMsg("Неверный формат времени", cid); return; }
    int h1 = startStr.substring(0, colon1).toInt();
    int m1 = startStr.substring(colon1 + 1).toInt();

    // Парсим конец
    int colon2 = endStr.indexOf(':');
    if (colon2 == -1) { sendMsg("Неверный формат времени", cid); return; }
    int h2 = endStr.substring(0, colon2).toInt();
    int m2 = endStr.substring(colon2 + 1).toInt();

    // Валидация
    if (h1 < 0 || h1 > 23 || m1 < 0 || m1 > 59 ||
        h2 < 0 || h2 > 23 || m2 < 0 || m2 > 59) {
        sendMsg("🕒 Время должно быть в диапазоне 00:00–23:59", cid);
        return;
    }

    // Проверка на пересечение с существующими зонами (опционально)
    // (можно пропустить для простоты)

    // Добавляем зону
    timeZones[zoneCount++] = { (uint8_t)h1, (uint8_t)m1, (uint8_t)h2, (uint8_t)m2, temp };
    zonesChanged = true;
    saveZones();

    String startFmt = String(h1) + ":" + (m1 < 10 ? "0" : "") + String(m1);
    String endFmt   = String(h2) + ":" + (m2 < 10 ? "0" : "") + String(m2);
    sendMsg("✅ Добавлен интервал: " + startFmt + "–" + endFmt + " → " + String(temp, 1) + "°C", cid);
}
    else if (cmd.startsWith("/delzone "))
    {
        int idx = rawText.substring(9).toInt() - 1;
        if (idx < 0 || idx >= zoneCount)
        {
            sendMsg("❌ Такой зоны нет", cid);
            return;
        }

        for (int i = idx; i < zoneCount - 1; i++)
        {
            timeZones[i] = timeZones[i + 1];
        }
        zoneCount--;
        zonesChanged = true; // <-- пометили изменение
        saveZones();
        sendMsg("🗑 Удалено", cid);
    }
    else if (cmd == "/clearzones") {
    zoneCount = 0;
    zonesChanged = true;
    saveZones();
    sendMsg("🗑 Все зоны удалены", cid);
}
    else if (cmd.startsWith("/setworktime ")) {
    int val = rawText.substring(13).toInt();
    if (val >= 1 && val <= 60) {
        extern uint16_t workDurationMinutes;
        extern void saveWorkDuration();
        workDurationMinutes = val;
        saveWorkDuration();
        sendMsg("⏱ Макс. время работы: " + String(workDurationMinutes) + " мин", cid);
    } else {
        sendMsg("Укажите от 1 до 60 минут", cid);
    }
}
else if (cmd.startsWith("/setminpause ")) {
    int val = rawText.substring(13).toInt();
    if (val >= 1 && val <= 60) {
        minStartInterval = val * 60 * 1000UL; // переводим минуты в миллисекунды
        saveMinStartInterval();
        sendMsg("⏸ Мин. пауза между включениями: " + String(val) + " мин", cid);
    } else {
        sendMsg("Укажите от 1 до 60 минут", cid);
    }
}
else if (cmd.startsWith("/work ")) {
    String interval = rawText.substring(6); // получаем "19:00-08:00"
    int dash = interval.indexOf('-');
    if (dash == -1) {
        sendMsg("📌 Формат: /work ЧЧ:ММ-ЧЧ:ММ", cid);
        return;
    }

    String startStr = interval.substring(0, dash);
    String endStr   = interval.substring(dash + 1);

    int colon1 = startStr.indexOf(':');
    int colon2 = endStr.indexOf(':');
    if (colon1 == -1 || colon2 == -1) {
        sendMsg("📌 Формат времени неверен", cid);
        return;
    }

    uint8_t h1 = startStr.substring(0, colon1).toInt();
    uint8_t m1 = startStr.substring(colon1 + 1).toInt();
    uint8_t h2 = endStr.substring(0, colon2).toInt();
    uint8_t m2 = endStr.substring(colon2 + 1).toInt();

    if (h1 > 23 || m1 > 59 || h2 > 23 || m2 > 59) {
        sendMsg("🕒 Время должно быть в диапазоне 00:00–23:59", cid);
        return;
    }

    // сохраняем интервал
    workStartHour = h1;
    workStartMinute = m1;
    workEndHour = h2;
    workEndMinute = m2;
    workScheduled = true;
    workModeEnabled = true; // включаем режим работы

    sendMsg("✅ Включён режим РАБОТА (22°C) с " + startStr + " до " + endStr, cid);
}

else if (cmd == "/auto") {
    workModeEnabled = false;
    sendMsg("✅ Режим: АВТО (по расписанию)", cid);
}
    else
    {
        sendMsg("❓ Неизвестная команда. Напишите /help", cid);
    }
});
    setupOTA();
    setupBoilerCommands();
}
void loop()
{
    bot.tick();
    if (needToSendAutoReport)
    {
        needToSendAutoReport = false;
        String tgid = "619084238";
        sendToTelegram(tgid);
    }
      // === Накопление измерений для NarodMon каждые 100 сек ===
    if (WiFi.status() == WL_CONNECTED && 
        millis() - lastNarodMonMeasurementTime >= NARODMON_MEAS_INTERVAL && 
        narodMonMeasurementCount < 3)
    {
        float t = sht31.readTemperature();
        if (!isnan(t))
        {
            tempSumForNarodMon += t;
            narodMonMeasurementCount++;
            lastNarodMonMeasurementTime = millis();
        }
        else
        {
            reportError("Ошибка чтения SHT31 при фоновом измерении для NarodMon");
        }
    }
    if (sendtonm)
    {
        sendtonm = false;
        sendSHT31Data();
    }
    ArduinoOTA.handle();
    boilerLoop();
     if (WiFi.status() != WL_CONNECTED)
    {
        serialToTelegram("📡 Wi-Fi разорван. Попытка переподключения...");
        WiFiMulti.run(); // Попробует подключиться снова
        delay(1000);
    }
}
