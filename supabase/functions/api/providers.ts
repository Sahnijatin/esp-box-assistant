// AI provider adapters. Everything the box needs goes through here, so the
// device never sees an API key and providers can be swapped from the setup page.

export type Msg = { role: "user" | "assistant"; content: string };

const TIMEOUT_MS = 45_000;

async function call(url: string, init: RequestInit): Promise<Response> {
  const res = await fetch(url, { ...init, signal: AbortSignal.timeout(TIMEOUT_MS) });
  if (!res.ok) {
    const body = (await res.text()).slice(0, 400);
    throw new Error(`${new URL(url).host} ${res.status}: ${body}`);
  }
  return res;
}

function b64(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(s);
}

function unb64(s: string): Uint8Array<ArrayBuffer> {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

const OPENAI_COMPAT: Record<string, string> = {
  openai: "https://api.openai.com/v1",
  groq: "https://api.groq.com/openai/v1",
  openrouter: "https://openrouter.ai/api/v1",
  sarvam: "https://api.sarvam.ai/v1",
};

// Setting value -> provider language codes
// en: English, hi: Hindi (Devanagari), hinglish: Hindi-English mix, auto: detect
function isoLang(language: string): string {
  return language === "en" ? "en" : language === "hi" || language === "hinglish" ? "hi" : "";
}
function sarvamLang(language: string, text = ""): string {
  if (language === "en") return "en-IN";
  if (language === "hi" || language === "hinglish") return "hi-IN";
  if (text) return /[ऀ-ॿ]/.test(text) ? "hi-IN" : "en-IN";
  return "unknown";
}

// ---------------- speech to text ----------------

export async function transcribe(
  provider: string, model: string, key: string, wav: Uint8Array<ArrayBuffer>, language: string,
): Promise<string> {
  const lang = isoLang(language);
  if (provider === "openai" || provider === "groq") {
    const form = new FormData();
    form.append("file", new Blob([wav], { type: "audio/wav" }), "audio.wav");
    form.append("model", model);
    if (lang) form.append("language", lang);
    const res = await call(`${OPENAI_COMPAT[provider]}/audio/transcriptions`, {
      method: "POST", headers: { Authorization: `Bearer ${key}` }, body: form,
    });
    return ((await res.json()).text ?? "").trim();
  }
  if (provider === "elevenlabs") {
    const form = new FormData();
    form.append("file", new Blob([wav], { type: "audio/wav" }), "audio.wav");
    form.append("model_id", model || "scribe_v1");
    if (lang) form.append("language_code", lang);
    const res = await call("https://api.elevenlabs.io/v1/speech-to-text", {
      method: "POST", headers: { "xi-api-key": key }, body: form,
    });
    return ((await res.json()).text ?? "").trim();
  }
  if (provider === "sarvam") {
    const form = new FormData();
    form.append("file", new Blob([wav], { type: "audio/wav" }), "audio.wav");
    form.append("model", model || "saaras:v3");
    form.append("language_code", sarvamLang(language));
    if ((model || "saaras:v3") === "saaras:v3") form.append("mode", language === "hinglish" ? "codemix" : "transcribe");
    const res = await call("https://api.sarvam.ai/speech-to-text", {
      method: "POST", headers: { "api-subscription-key": key }, body: form,
    });
    return ((await res.json()).transcript ?? "").trim();
  }
  if (provider === "gemini") {
    const res = await call(
      `https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`,
      {
        method: "POST",
        headers: { "Content-Type": "application/json", "x-goog-api-key": key },
        body: JSON.stringify({
          contents: [{
            parts: [
              { text: "Transcribe this audio exactly. Reply with only the transcript, nothing else. If there is no speech reply with an empty string." },
              { inline_data: { mime_type: "audio/wav", data: b64(wav) } },
            ],
          }],
        }),
      },
    );
    return geminiText(await res.json()).trim();
  }
  throw new Error(`Unknown speech-to-text provider: ${provider}`);
}

// ---------------- chat ----------------

function geminiText(j: any): string {
  const parts = j?.candidates?.[0]?.content?.parts ?? [];
  return parts.map((p: any) => p.text ?? "").join("");
}

export async function chat(
  provider: string, model: string, key: string, system: string, messages: Msg[],
): Promise<string> {
  if (provider in OPENAI_COMPAT) {
    const body: Record<string, unknown> = {
      model,
      messages: [{ role: "system", content: system }, ...messages],
    };
    if (provider === "openai") body.max_completion_tokens = 600;
    else if (provider === "sarvam") {
      body.max_tokens = 1500;          // reasoning tokens count against this
      body.reasoning_effort = "low";
    } else body.max_tokens = 400;
    const headers: Record<string, string> = {
      Authorization: `Bearer ${key}`, "Content-Type": "application/json",
    };
    if (provider === "openrouter") headers["X-Title"] = "ESP Box Assistant";
    if (provider === "sarvam") headers["api-subscription-key"] = key;
    const res = await call(`${OPENAI_COMPAT[provider]}/chat/completions`, {
      method: "POST", headers, body: JSON.stringify(body),
    });
    const j = await res.json();
    return (j.choices?.[0]?.message?.content ?? "").trim();
  }
  if (provider === "anthropic") {
    const res = await call("https://api.anthropic.com/v1/messages", {
      method: "POST",
      headers: {
        "x-api-key": key, "anthropic-version": "2023-06-01", "Content-Type": "application/json",
      },
      body: JSON.stringify({ model, max_tokens: 400, system, messages }),
    });
    const j = await res.json();
    return (j.content ?? []).filter((c: any) => c.type === "text").map((c: any) => c.text).join("").trim();
  }
  if (provider === "gemini") {
    const generationConfig: Record<string, unknown> = { maxOutputTokens: 800 };
    if (/flash/.test(model)) generationConfig.thinkingConfig = { thinkingBudget: 0 };
    const res = await call(
      `https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`,
      {
        method: "POST",
        headers: { "Content-Type": "application/json", "x-goog-api-key": key },
        body: JSON.stringify({
          systemInstruction: { parts: [{ text: system }] },
          contents: messages.map((m) => ({
            role: m.role === "assistant" ? "model" : "user",
            parts: [{ text: m.content }],
          })),
          generationConfig,
        }),
      },
    );
    return geminiText(await res.json()).trim();
  }
  throw new Error(`Unknown chat provider: ${provider}`);
}

// ---------------- text to speech ----------------
// Always returns raw PCM: 24 kHz, 16-bit little endian, mono.

export const TTS_SAMPLE_RATE = 24000;

// gpt-4o-*-tts models take a style instruction; tts-1 models take a speed number
function openaiSpeech(model: string, voice: string, text: string, speed: number) {
  const body: Record<string, unknown> = { model, voice: voice || "alloy", input: text, response_format: "pcm" };
  if (speed !== 1) {
    if (/^tts-1/.test(model)) body.speed = clamp(speed, 0.25, 4);
    else if (paceInstruction(speed)) body.instructions = paceInstruction(speed);
  }
  return body;
}

// Strips a WAV header if present, returning the raw PCM samples
function wavToPcm(b: Uint8Array<ArrayBuffer>): Uint8Array<ArrayBuffer> {
  if (b.length < 44 || String.fromCharCode(...b.subarray(0, 4)) !== "RIFF") return b;
  let p = 12;
  while (p + 8 <= b.length) {
    const id = String.fromCharCode(...b.subarray(p, p + 4));
    const len = b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | (b[p + 7] << 24);
    if (id === "data") return b.slice(p + 8, Math.min(b.length, p + 8 + len));
    p += 8 + len + (len & 1);
  }
  return b.slice(44);
}

// Speaking pace as words, for models that take a style prompt instead of a speed number
function paceInstruction(speed: number): string {
  if (speed <= 0.8) return "Speak slowly, at a calm and relaxed pace, with clear pauses between sentences.";
  if (speed < 0.95) return "Speak at a slightly slower, relaxed pace.";
  if (speed >= 1.25) return "Speak quickly and briskly.";
  if (speed > 1.05) return "Speak at a slightly faster pace.";
  return "";
}

const clamp = (v: number, lo: number, hi: number) => Math.min(Math.max(v, lo), hi);

// speed: 1.0 = normal; each provider gets it in its own form (and range)
export async function speak(
  provider: string, model: string, voice: string, key: string, text: string, language = "en", speed = 1,
): Promise<ReadableStream<Uint8Array> | Uint8Array<ArrayBuffer>> {
  if (!Number.isFinite(speed) || speed <= 0) speed = 1;
  if (provider === "sarvam") {
    const m = model || "bulbul:v3";
    const body: Record<string, unknown> = {
      text: text.slice(0, m === "bulbul:v2" ? 1500 : 2500),
      language_code: sarvamLang(language === "auto" ? "" : language, text),
      model: m,
      speech_sample_rate: TTS_SAMPLE_RATE,
      output_audio_codec: "wav",
    };
    if (voice) body.speaker = voice;
    if (speed !== 1) body.pace = clamp(speed, 0.5, 2);
    const res = await call("https://api.sarvam.ai/text-to-speech", {
      method: "POST",
      headers: { "api-subscription-key": key, "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    const j = await res.json();
    const parts: Uint8Array<ArrayBuffer>[] = (j.audios ?? []).map((a: string) => wavToPcm(unb64(a)));
    if (!parts.length) throw new Error("Sarvam TTS returned no audio");
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
  }
  if (provider === "openai") {
    const res = await call("https://api.openai.com/v1/audio/speech", {
      method: "POST",
      headers: { Authorization: `Bearer ${key}`, "Content-Type": "application/json" },
      body: JSON.stringify(openaiSpeech(model, voice, text, speed)),
    });
    return res.body!;
  }
  if (provider === "elevenlabs") {
    const res = await call(
      `https://api.elevenlabs.io/v1/text-to-speech/${encodeURIComponent(voice)}/stream?output_format=pcm_24000`,
      {
        method: "POST",
        headers: { "xi-api-key": key, "Content-Type": "application/json" },
        body: JSON.stringify({
          text, model_id: model || "eleven_flash_v2_5",
          ...(speed !== 1 ? { voice_settings: { speed: clamp(speed, 0.7, 1.2) } } : {}),
        }),
      },
    );
    return res.body!;
  }
  if (provider === "gemini") {
    const res = await call(
      `https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`,
      {
        method: "POST",
        headers: { "Content-Type": "application/json", "x-goog-api-key": key },
        body: JSON.stringify({
          // Gemini TTS takes style as part of the prompt
          contents: [{ parts: [{ text: paceInstruction(speed) ? `${paceInstruction(speed)} Say: ${text}` : text }] }],
          generationConfig: {
            responseModalities: ["AUDIO"],
            speechConfig: { voiceConfig: { prebuiltVoiceConfig: { voiceName: voice || "Kore" } } },
          },
        }),
      },
    );
    const j = await res.json();
    const data = j?.candidates?.[0]?.content?.parts?.find((p: any) => p.inlineData)?.inlineData?.data;
    if (!data) throw new Error("Gemini TTS returned no audio");
    return unb64(data);
  }
  throw new Error(`Unknown text-to-speech provider: ${provider}`);
}
