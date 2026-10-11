# ESP Box Assistant

Tap-to-talk AI Q&A on the ESP32-S3-BOX-3, fully managed over the internet: AI provider, API keys, language, Wi-Fi networks and firmware updates are all changed from a webpage. The box never needs a laptop after the first flash.

```
 ESP32-S3-BOX-3  ──HTTPS──▶  Supabase Edge Function "api"  ──▶  OpenAI / Anthropic / Gemini / Groq /
   (thin client)               settings, keys (Vault), Wi-Fi list,      OpenRouter / ElevenLabs / Sarvam AI
                               pairing, firmware storage, Q&A log,
                               Hindi text rendering
        ▲                                   ▲
        │ OTA (signed URL)                  │ admin API
        │                                   │
 GitHub Actions build ─OIDC─▶ upload    Setup page: https://esp-box-control.vercel.app (web/index.html)
```

## Layout

| Path | What |
|---|---|
| `firmware/` | ESP-IDF 5.4 app: LVGL 9 UI, Wi-Fi manager, audio, OTA with rollback |
| `supabase/functions/api/index.ts` | Edge Function router: `/device/*`, `/admin/*`, `/ci/*` |
| `supabase/functions/api/providers.ts` | Speech-to-text, chat and text-to-speech adapters for every provider |
| `supabase/functions/api/render.ts` | Draws Hindi (and other non-ASCII) text as an RGB565 bitmap for the box |
| `supabase/migrations/` | Database schema |
| `web/index.html` | Setup page (single static file, deployed on Vercel) |
| `.github/scripts/publish.py` | Uploads a firmware build to the backend |
| `docs/firmware-workflow.yml` | The GitHub Actions workflow. Copy it to `.github/workflows/firmware.yml` (see below) |

## Setup checklist

1. **Add the build workflow.** GitHub doesn't let the automation token create workflow files, so this one step is manual: on GitHub, Add file → Create new file, name it `.github/workflows/firmware.yml`, paste the contents of `docs/firmware-workflow.yml`, commit. The first build starts automatically.
2. **Add API keys** on the setup page (API keys tab) for the providers you pick on the AI tab.
3. **First flash:** setup page → Updates → Prepare installer → Install on box (Chrome or Edge on a laptop, box on USB-C).
4. **Wi-Fi + pairing:** pick Wi-Fi on the box's screen, then enter the 6-digit code it shows under Boxes.

## How deploys work

1. Push to `main` (anything under `firmware/`).
2. GitHub Actions builds with `espressif/idf:v5.4.2`, version `0.1.<run number>`.
3. The workflow proves its identity with a GitHub OIDC token (no secrets stored in GitHub) and uploads `ota.bin` + `factory.bin` to the private `firmware` bucket.
4. Each box checks in every `poll_seconds`. If a newer build exists and auto-update is on, it downloads it, reboots, and confirms the new build once it reaches the server. If it can't reach the server for 3 minutes, or crashes, the bootloader rolls back to the previous version.

Failed builds show up on the setup page under Updates with the log.

Edge Function changes are deployed separately (Supabase dashboard, CLI `supabase functions deploy api --no-verify-jwt`, or ask Claude). The setup page is redeployed on Vercel from `web/`.

## Box behaviour

- **First boot:** no Wi-Fi saved → Wi-Fi screen on the touchscreen + hotspot `ESP-Box-XXXX` (open `http://192.168.4.1`).
- **Pairing:** shows a 6-digit code; enter it on the setup page under Boxes.
- **Wi-Fi changes:** add networks on the setup page (pushed to boxes at check-in), on the touchscreen (tap the top bar), or through the hotspot when nothing connects.
- **Asking:** tap the button, speak; it stops after about 1 s of silence (or tap again).

## Languages

Set on the AI tab.

| Setting | Screen | Voice |
|---|---|---|
| English | English text | English |
| Hinglish | Roman script ("Haan, Delhi India ki capital hai") | Same reply with Hindi words in Devanagari, English words in Latin, so Indian TTS voices pronounce it naturally |
| Hindi | Devanagari, drawn by the server | Hindi |
| Auto | Matches the language spoken | Matches the reply |

The box's display library (LVGL) can't shape Devanagari (matras and conjuncts break), so any reply with non-ASCII text is drawn server-side: HarfBuzz shapes it with Noto Sans Devanagari, resvg rasterises it, and the box receives an RGB565 bitmap from `/device/render`. Hinglish mode asks the model for two lines, `DISPLAY:` and `SPEECH:`, which the backend splits.

## Providers

| Slot | Providers |
|---|---|
| Brain (chat) | OpenAI, Anthropic, Google Gemini, Groq, OpenRouter, Sarvam AI (`sarvam-105b-conversations`) |
| Ears (speech-to-text) | OpenAI, Groq, ElevenLabs, Gemini, Sarvam AI (`saaras:v3`, uses `codemix` mode for Hinglish) |
| Voice (text-to-speech) | OpenAI, ElevenLabs, Gemini, Sarvam AI (`bulbul:v3` voices like `shubh`, `priya`; `bulbul:v4-flash` personas like `shubh_enhi_ads`) |

All audio to the box is 24 kHz 16-bit mono PCM. Model and voice fields are free text, so new models work without code changes.

## API

| Route | Caller | Auth |
|---|---|---|
| `POST /device/hello` | box check-in: pairing, config, Wi-Fi list, update offer | device id + token |
| `POST /device/ask` | WAV in → `{question, answer, speech?, render}` | `X-Device-Id`, `X-Device-Token` |
| `POST /device/tts` | text → PCM stream | same |
| `POST /device/render` | `{question, answer}` → `[w u16][h u16]` + RGB565 pixels | same |
| `POST /admin/*` | setup page | `Bearer <admin password>` |
| `POST /ci/*` | GitHub Actions | GitHub OIDC token, repo must be `Sahnijatin/esp-box-assistant`, publishing only from `main` |

## Local build

```bash
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
```

## Backend

- Project: `sxzeggllkdirvfnravfe` (Supabase, ap-south-1)
- API keys live in Supabase Vault; the device never receives them. Removing a key sets it to an empty value.
- Every table has RLS on with no policies; only the Edge Function (service role) touches data.
- Admin login is a bcrypt-hashed password in `admin_auth`, changeable from the setup page.

## Known limits

- Only Hindi/Latin glyphs are in the render font; other Indic scripts (Tamil, Bengali...) need another Noto font added in `render.ts`.
- Sarvam integration is built from Sarvam's API docs and still needs a first real test with a key.
- Wake word ("Hi ESP") is not in yet; trigger is tap-to-talk.
