#!/usr/bin/env bash
set -euo pipefail

# Usage:
# BOT_TOKEN=xxx \
# ADMIN_CHAT_ID=123456 \
# WEBHOOK_SECRET=super-secret \
# WORKER_URL=https://tag-kotel-worker.<subdomain>.workers.dev \
# ./scripts/cf_setup.sh

: "${BOT_TOKEN:?BOT_TOKEN is required}"
: "${ADMIN_CHAT_ID:?ADMIN_CHAT_ID is required}"
: "${WEBHOOK_SECRET:?WEBHOOK_SECRET is required}"
: "${WORKER_URL:?WORKER_URL is required}"

if ! command -v wrangler >/dev/null 2>&1; then
  echo "wrangler CLI is not installed. Install: npm i -g wrangler"
  exit 1
fi

echo "[1/4] Create KV namespace (if needed):"
echo "    wrangler kv namespace create BOT_STATE"
echo "    wrangler kv namespace create BOT_STATE --preview"

echo "[2/4] Set secrets"
printf '%s' "$BOT_TOKEN" | wrangler secret put BOT_TOKEN
printf '%s' "$ADMIN_CHAT_ID" | wrangler secret put ADMIN_CHAT_ID
printf '%s' "$WEBHOOK_SECRET" | wrangler secret put WEBHOOK_SECRET

echo "[3/4] Deploy worker"
wrangler deploy

echo "[4/4] Configure Telegram webhook"
curl -sS "https://api.telegram.org/bot${BOT_TOKEN}/setWebhook" \
  -H 'Content-Type: application/json' \
  -d "{\"url\":\"${WORKER_URL}/webhook\",\"secret_token\":\"${WEBHOOK_SECRET}\",\"drop_pending_updates\":true}"

echo
echo "Done. Webhook URL: ${WORKER_URL}/webhook"
echo "Device ingest URL: ${WORKER_URL}/device"
