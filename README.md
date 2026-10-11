# ESP Box Assistant

Tap-to-talk AI Q&A on the ESP32-S3-BOX-3, fully managed over the internet.

```
 ESP32-S3-BOX-3  ──HTTPS──▶  Supabase Edge Function "api"  ──▶  OpenAI / Anthropic / Gemini / Groq / OpenRouter / ElevenLabs
   (thin client)               settings, keys (Vault), Wi-Fi list,
                               pairing, firmware storage, Q&A log
        ▲                                   ▲
        │ OTA (signed URL)                  │ admin API
        │                                   │
 GitHub Actions build ─OIDC─▶ upload    Setup page (web/index.html on Vercel)
```

## Layout

| Path | What |
|---|---|
| `firmware/` | ESP-IDF 5.4 app (LVGL 9 UI, Wi-Fi manager, audio, OTA with rollback) |
| `supabase/functions/api/` | Edge Function: `/device/*`, `/admin/*`, `/ci/*` |
| `supabase/migrations/` | Database schema |
| `web/index.html` | Setup page (single static file) |
| `.github/workflows/firmware.yml` | Builds firmware on every push to `main` and publishes it |

## How deploys work

1. Push to `main` (anything under `firmware/`).
2. GitHub Actions builds with `espressif/idf:v5.4.2`, version `0.1.<run number>`.
3. The workflow proves its identity with a GitHub OIDC token (no secrets stored in GitHub) and uploads `ota.bin` + `factory.bin` to the private `firmware` bucket.
4. Each box checks in every `poll_seconds`. If a newer build exists and auto-update is on, it downloads it, reboots, and confirms the new build once it reaches the server. If it can't reach the server for 3 minutes, or crashes, the bootloader rolls back.

Failed builds show up on the setup page under Updates with the log.

## Box behaviour

- **First boot:** no Wi-Fi saved → Wi-Fi screen on the touchscreen + hotspot `ESP-Box-XXXX` (open `http://192.168.4.1`).
- **Pairing:** shows a 6-digit code; enter it on the setup page under Boxes.
- **Wi-Fi changes:** add networks on the setup page (pushed to boxes), on the touchscreen (tap the top bar), or through the hotspot when nothing connects.
- **Asking:** tap the button, speak, it stops after ~1 s of silence (or tap again).

## Local build

```bash
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
```

## Backend

- Project: `sxzeggllkdirvfnravfe` (Supabase, ap-south-1)
- API keys live in Supabase Vault; the device never receives them.
- Every table has RLS on with no policies; only the Edge Function (service role) touches data.
- Admin login is a bcrypt-hashed password in `admin_auth`, changeable from the page.
