# Переключение Telegram / Cloudflare Worker без правки FastBot

Ниже — способ переключать endpoint **в самом sketch**, не меняя библиотеку `FastBot`.

## 1) Добавьте helper

Подключите файл `src/EndpointSelector.h` в ваш проект и в основном `.ino` добавьте:

```cpp
#define BOT_USE_CLOUDFLARE true
#define BOT_CLOUDFLARE_BASE "https://your-worker.example.workers.dev"
#include "EndpointSelector.h"
```

Если хотите ходить напрямую в Telegram API:

```cpp
#define BOT_USE_CLOUDFLARE false
#include "EndpointSelector.h"
```

## 2) Замените `sendMsg(...)`

Вместо:

```cpp
void sendMsg(String text, String chatId) {
  bot.sendMessage(urlencode(text), chatId);
}
```

Используйте:

```cpp
void sendMsg(const String& text, const String& chatId) {
  const bool ok = sendMessageViaSelectedBackend(BOT_TOKEN, chatId, text);
  if (!ok) {
    Serial.println("sendMsg failed");
  }
}
```

## 3) Что это даёт

- Один и тот же код sketch.
- Переключение backend через `#define`.
- Нет изменений внутри библиотеки `FastBot`.

## Важно про `bot.tick()`

Этот helper переключает HTTP endpoint для `sendMessage`.

Если вам нужно переключать **ещё и polling** (`getUpdates`, который выполняет `bot.tick()`),
то это возможно только если текущая версия `FastBot` уже умеет задавать base URL извне
(через API/макрос). Если нет — для полного переключения polling тоже нужен либо:

1. форк FastBot с внешней настройкой base URL,
2. либо свой polling через `HTTPClient` + `ArduinoJson`.
