# Tag_kotel + Cloudflare Worker + Telegram Webhook (всё в одном месте)

Ниже полный набор: деплой через Dashboard, деплой через Wrangler, настройка webhook, формат данных от устройства и пример для ESP8266.

---

## 1) Что лежит в репозитории

- `worker.js` — основной Worker (webhook Telegram + endpoint для устройства).
- `wrangler.toml` — конфиг для CLI-деплоя.
- `scripts/cf_setup.sh` — автоматизация деплоя/секретов через Wrangler.
- `examples/device_client.ino` — пример, как ESP8266 отправляет телеметрию и забирает команды.

---

## 2) Быстрый путь (Cloudflare Dashboard, без CLI)

1. Cloudflare → Workers & Pages → Create Worker.
2. Вставьте содержимое `worker.js`.
3. Нажмите **Deploy**.
4. В Settings → Variables добавьте:
   - `BOT_TOKEN` (обязательно)
   - `WEBHOOK_SECRET` (обязательно)
   - `ADMIN_CHAT_ID` (желательно)
   - `DEVICE_SECRET` (желательно)
   - `SETUP_KEY` (опционально)
   - `DEFAULT_REPORT_INTERVAL_MINUTES` (опционально)
5. Снова **Deploy**.

### Включение webhook

Откройте в браузере:
- `https://<your-worker>.workers.dev/setup`
- или `https://<your-worker>.workers.dev/setup?key=<SETUP_KEY>` (если задан `SETUP_KEY`)

---

## 3) CLI путь (Wrangler)

```bash
BOT_TOKEN="<telegram_bot_token>" \
ADMIN_CHAT_ID="<chat_id>" \
WEBHOOK_SECRET="<random_secret>" \
WORKER_URL="https://<your-worker>.workers.dev" \
./scripts/cf_setup.sh
```

---

## 4) Telegram команды

- `/help`
- `/status`
- `/setinterval <мин>`
- `/autoon`
- `/autooff`
- `/relay on|off`

---

## 5) API для устройства

### Отправка телеметрии
`POST /device`

Пример JSON:
```json
{
  "temperature": 23.41,
  "humidity": 45.2,
  "relayState": true,
  "timestamp": "2026-04-13T12:00:00Z"
}
```

Если задан `DEVICE_SECRET`, передавайте header:
`Authorization: Bearer <DEVICE_SECRET>`

### Получение команд
`GET /device/commands`

### Подтверждение выполнения (очистка очереди)
`POST /device/commands/ack`

---

## 6) Пример для ESP8266

Смотрите `examples/device_client.ino`.

В нём уже есть:
- `sendTelemetry(...)`
- `pollCommands()`
- подтверждение очереди через `/device/commands/ack`

---

## 7) Важно про хранение состояния

Рекомендуется подключить KV binding `BOT_STATE`.
Если KV не подключён — используется in-memory fallback (данные могут теряться при перезапусках worker).
