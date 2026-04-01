#ifndef FASTBOTT_H
#define FASTBOTT_H

#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

class FastBott {
public:
  bool sendMessage(const String& token, const String& chatId, const String& text, uint16_t timeoutMs = 10000) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    String encodedText = urlencode(text);
    String encodedChatId = urlencode(chatId);
    String url = "https://api.telegram.org/bot" + token + "/sendMessage?chat_id=" + encodedChatId + "&text=" + encodedText;

    http.setTimeout(timeoutMs);
    if (!http.begin(secureClient, url)) return false;
    int code = http.GET();
    http.end();
    return code == HTTP_CODE_OK;
  }

private:
  String urlencode(const String& str) {
    String encoded = "";
    const char* cstr = str.c_str();
    for (size_t i = 0; i < strlen(cstr); i++) {
      unsigned char c = cstr[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.' || c == '~') {
        encoded += static_cast<char>(c);
      } else {
        char buf[4];
        sprintf(buf, "%%%02X", c);
        encoded += buf;
      }
    }
    return encoded;
  }
};

#endif
