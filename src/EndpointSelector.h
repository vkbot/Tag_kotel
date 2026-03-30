#pragma once

#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

// Переключение транспорта Telegram API без правок FastBot.
// Используйте в sketch:
//   #define BOT_USE_CLOUDFLARE true
//   #define BOT_CLOUDFLARE_BASE "https://your-worker.example.workers.dev"

#ifndef BOT_USE_CLOUDFLARE
#define BOT_USE_CLOUDFLARE false
#endif

#ifndef BOT_CLOUDFLARE_BASE
#define BOT_CLOUDFLARE_BASE ""
#endif

enum class BotBackend : uint8_t {
  Telegram,
  CloudflareWorker,
};

inline BotBackend activeBackend() {
  return BOT_USE_CLOUDFLARE ? BotBackend::CloudflareWorker : BotBackend::Telegram;
}

inline String urlEncode(const String &src) {
  String out;
  out.reserve(src.length() * 3);

  for (size_t i = 0; i < src.length(); i++) {
    const uint8_t c = static_cast<uint8_t>(src[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }

  return out;
}

inline String buildBotUrl(const String &token, const String &method) {
  if (activeBackend() == BotBackend::CloudflareWorker) {
    // Ожидается, что worker принимает путь /bot<TOKEN>/<method>
    // Пример: https://<worker>/bot123456:ABC/sendMessage
    return String(BOT_CLOUDFLARE_BASE) + "/bot" + token + "/" + method;
  }

  return "https://api.telegram.org/bot" + token + "/" + method;
}

inline bool sendMessageViaSelectedBackend(const String &token,
                                          const String &chatId,
                                          const String &text,
                                          uint16_t timeoutMs = 12000) {
  if (token.length() == 0 || chatId.length() == 0) {
    return false;
  }

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
  client->setInsecure();

  HTTPClient http;
  http.setTimeout(timeoutMs);

  const String url = buildBotUrl(token, "sendMessage");
  if (!http.begin(*client, url)) {
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const String body = "chat_id=" + chatId + "&text=" + urlEncode(text);
  const int code = http.POST(body);
  http.end();

  return code >= 200 && code < 300;
}
