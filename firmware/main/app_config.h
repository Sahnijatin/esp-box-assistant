#pragma once

// Backend (Supabase Edge Function). Everything else (AI provider, keys,
// Wi-Fi list, volume, firmware updates) is configured from the setup page.
#define API_BASE            "https://sxzeggllkdirvfnravfe.supabase.co/functions/v1/api"

#define SOFTAP_PREFIX       "ESP-Box-"
#define SETUP_IP            "192.168.4.1"
#define SETUP_URL           "https://esp-box-control.vercel.app"   // setup webpage

#define REC_SAMPLE_RATE     16000
#define REC_MAX_SECONDS     12
#define TTS_SAMPLE_RATE     24000

#define DEFAULT_VOLUME      70
#define DEFAULT_BRIGHTNESS  80
