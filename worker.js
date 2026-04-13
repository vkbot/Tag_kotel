/**
 * Cloudflare Worker Telegram webhook bot (Dashboard-friendly).
 *
 * Works even when deployed from Cloudflare Dashboard editor (no Wrangler required).
 *
 * ENV/Vars (Settings -> Variables):
 * - BOT_TOKEN (required)
 * - ADMIN_CHAT_ID (required for auto reports)
 * - WEBHOOK_SECRET (required)
 * - DEVICE_SECRET (optional, protects /device)
 * - SETUP_KEY (optional, protects /setup and /deleteWebhook)
 * - DEFAULT_REPORT_INTERVAL_MINUTES (optional, default 10)
 *
 * Optional KV binding:
 * - BOT_STATE (KV namespace). If missing, Worker uses in-memory fallback (non-persistent).
 */

const TELEGRAM_API = 'https://api.telegram.org';
const memoryStore = new Map();

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method === 'GET' && url.pathname === '/') {
      return json({
        ok: true,
        service: 'telegram-webhook-worker',
        hint: 'Use /setup to auto-configure Telegram webhook',
      });
    }

    if (request.method === 'GET' && url.pathname === '/setup') {
      if (!isSetupAuthorized(url, env)) return new Response('forbidden', { status: 403 });
      const result = await setWebhook(env, url.origin);
      return json(result, { status: result.ok ? 200 : 500 });
    }

    if (request.method === 'GET' && url.pathname === '/deleteWebhook') {
      if (!isSetupAuthorized(url, env)) return new Response('forbidden', { status: 403 });
      const result = await callTelegram(env, 'deleteWebhook', { drop_pending_updates: true });
      return json(result, { status: result.ok ? 200 : 500 });
    }

    if (request.method === 'POST' && url.pathname === '/webhook') {
      const secret = request.headers.get('X-Telegram-Bot-Api-Secret-Token');
      if (!env.WEBHOOK_SECRET || secret !== env.WEBHOOK_SECRET) {
        return new Response('forbidden', { status: 403 });
      }

      const update = await request.json().catch(() => null);
      if (!update) return new Response('bad request', { status: 400 });

      ctx.waitUntil(handleTelegramUpdate(update, env));
      return json({ ok: true });
    }

    if (request.method === 'POST' && url.pathname === '/device') {
      if (env.DEVICE_SECRET) {
        const auth = request.headers.get('Authorization');
        if (auth !== `Bearer ${env.DEVICE_SECRET}`) {
          return new Response('forbidden', { status: 403 });
        }
      }

      const payload = await request.json().catch(() => null);
      if (!payload) return new Response('bad request', { status: 400 });

      const result = await handleDeviceWebhook(payload, env);
      return json(result);
    }

    // Device can poll pending commands (e.g., relay on/off)
    if (request.method === 'GET' && url.pathname === '/device/commands') {
      if (env.DEVICE_SECRET) {
        const auth = request.headers.get('Authorization');
        if (auth !== `Bearer ${env.DEVICE_SECRET}`) {
          return new Response('forbidden', { status: 403 });
        }
      }
      const commands = await loadCommands(env);
      return json({ ok: true, commands });
    }

    // Device acknowledges command queue processing
    if (request.method === 'POST' && url.pathname === '/device/commands/ack') {
      if (env.DEVICE_SECRET) {
        const auth = request.headers.get('Authorization');
        if (auth !== `Bearer ${env.DEVICE_SECRET}`) {
          return new Response('forbidden', { status: 403 });
        }
      }
      await saveCommands(env, []);
      return json({ ok: true });
    }

    return new Response('not found', { status: 404 });
  },
};

function isSetupAuthorized(url, env) {
  if (!env.SETUP_KEY) return true;
  return url.searchParams.get('key') === env.SETUP_KEY;
}

async function setWebhook(env, origin) {
  const payload = {
    url: `${origin}/webhook`,
    secret_token: env.WEBHOOK_SECRET,
    drop_pending_updates: true,
    allowed_updates: ['message'],
  };
  return callTelegram(env, 'setWebhook', payload);
}

async function handleTelegramUpdate(update, env) {
  const message = update.message;
  if (!message?.text) return;

  const chatId = String(message.chat.id);
  const text = message.text.trim();
  if (!text.startsWith('/')) return;

  const [command, ...args] = text.split(/\s+/);

  switch (command.toLowerCase()) {
    case '/start':
    case '/help':
      await sendMessage(
        env,
        chatId,
        [
          '🆘 Команды:',
          '/status — показать последнее состояние',
          '/setinterval <мин> — интервал авто-отчёта (1-1440)',
          '/autoon — включить авто-отчёт',
          '/autooff — выключить авто-отчёт',
          '/relay on|off — добавить команду реле в очередь',
        ].join('\n')
      );
      return;

    case '/status': {
      const state = await loadState(env);
      await sendMessage(env, chatId, formatStatus(state));
      return;
    }

    case '/setinterval': {
      const value = Number(args[0]);
      if (!Number.isInteger(value) || value < 1 || value > 1440) {
        await sendMessage(env, chatId, '❌ Укажите целое число минут: 1..1440');
        return;
      }
      const state = await loadState(env);
      state.reportIntervalMinutes = value;
      await saveState(env, state);
      await sendMessage(env, chatId, `✅ Интервал обновлён: ${value} мин`);
      return;
    }

    case '/autoon': {
      const state = await loadState(env);
      state.autoReportEnabled = true;
      await saveState(env, state);
      await sendMessage(env, chatId, '✅ Авто-отчёт включён');
      return;
    }

    case '/autooff': {
      const state = await loadState(env);
      state.autoReportEnabled = false;
      await saveState(env, state);
      await sendMessage(env, chatId, '✅ Авто-отчёт выключен');
      return;
    }

    case '/relay': {
      const mode = (args[0] || '').toLowerCase();
      if (mode !== 'on' && mode !== 'off') {
        await sendMessage(env, chatId, '❌ Использование: /relay on|off');
        return;
      }
      await enqueueCommand(env, { type: 'relay', value: mode, ts: Date.now() });
      await sendMessage(env, chatId, `📨 Команда реле поставлена в очередь: ${mode}`);
      return;
    }

    default:
      await sendMessage(env, chatId, '❓ Неизвестная команда. /help');
  }
}

async function handleDeviceWebhook(payload, env) {
  const state = await loadState(env);

  state.lastTelemetry = {
    temperature: asNumber(payload.temperature),
    humidity: asNumber(payload.humidity),
    relayState: payload.relayState ?? null,
    timestamp: payload.timestamp || new Date().toISOString(),
  };

  await saveState(env, state);

  const now = Date.now();
  const lastReportTs = Number(state.lastReportTs || 0);
  const intervalMs = Number(state.reportIntervalMinutes || 10) * 60_000;
  const shouldReport = state.autoReportEnabled && now - lastReportTs >= intervalMs;

  if (shouldReport && env.ADMIN_CHAT_ID) {
    await sendMessage(env, env.ADMIN_CHAT_ID, formatStatus(state));
    state.lastReportTs = now;
    await saveState(env, state);
  }

  return { ok: true };
}

function asNumber(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function formatStatus(state) {
  const t = state.lastTelemetry?.temperature;
  const h = state.lastTelemetry?.humidity;
  const relay = state.lastTelemetry?.relayState;
  const ts = state.lastTelemetry?.timestamp;

  return [
    '📟 Статус',
    `🌡 Температура: ${t == null ? 'нет данных' : `${t.toFixed(2)} °C`}`,
    `💧 Влажность: ${h == null ? 'нет данных' : `${h.toFixed(2)} %`}`,
    `🔌 Реле: ${relay == null ? 'нет данных' : relay ? 'включено' : 'выключено'}`,
    `⏱ Обновление: ${ts || 'нет данных'}`,
    `🔁 Авто-отчёт: ${state.autoReportEnabled ? 'вкл' : 'выкл'} (${state.reportIntervalMinutes} мин)`,
  ].join('\n');
}

async function enqueueCommand(env, cmd) {
  const commands = await loadCommands(env);
  commands.push(cmd);
  await saveCommands(env, commands.slice(-100));
}

async function loadCommands(env) {
  const raw = await readStore(env, 'commands');
  if (!raw) return [];
  try {
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

async function saveCommands(env, commands) {
  await writeStore(env, 'commands', JSON.stringify(commands));
}

async function loadState(env) {
  const raw = await readStore(env, 'state');
  const defaults = {
    autoReportEnabled: false,
    reportIntervalMinutes: Number(env.DEFAULT_REPORT_INTERVAL_MINUTES || 10),
    lastTelemetry: null,
    lastReportTs: 0,
  };

  if (!raw) return defaults;

  try {
    return { ...defaults, ...JSON.parse(raw) };
  } catch {
    return defaults;
  }
}

async function saveState(env, state) {
  await writeStore(env, 'state', JSON.stringify(state));
}

async function readStore(env, key) {
  if (env.BOT_STATE && typeof env.BOT_STATE.get === 'function') {
    return env.BOT_STATE.get(key);
  }
  return memoryStore.get(key) ?? null;
}

async function writeStore(env, key, value) {
  if (env.BOT_STATE && typeof env.BOT_STATE.put === 'function') {
    await env.BOT_STATE.put(key, value);
    return;
  }
  memoryStore.set(key, value);
}

async function sendMessage(env, chatId, text) {
  if (!env.BOT_TOKEN) throw new Error('BOT_TOKEN is not set');
  await callTelegram(env, 'sendMessage', { chat_id: chatId, text });
}

async function callTelegram(env, method, payload) {
  if (!env.BOT_TOKEN) throw new Error('BOT_TOKEN is not set');

  const response = await fetch(`${TELEGRAM_API}/bot${env.BOT_TOKEN}/${method}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });

  const data = await response.json().catch(() => null);
  if (!response.ok || !data?.ok) {
    const message = data ? JSON.stringify(data) : await response.text();
    throw new Error(`Telegram API error (${method}): ${response.status} ${message}`);
  }
  return data;
}

function json(value, init = {}) {
  return new Response(JSON.stringify(value), {
    ...init,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      ...(init.headers || {}),
    },
  });
}
