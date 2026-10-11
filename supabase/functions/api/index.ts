// ESP Box Assistant API (single Supabase Edge Function)
//
//   /device/*  called by the box      auth: X-Device-Id + X-Device-Token (paired via 6-digit code)
//   /admin/*   called by setup page   auth: Bearer <session token from /admin/login>
//   /ci/*      called by GitHub CI    auth: GitHub Actions OIDC token (no stored secrets)

import { createClient } from "npm:@supabase/supabase-js@2.45.4";
import { createRemoteJWKSet, jwtVerify } from "npm:jose@5.9.6";
import { chat, Msg, speak, transcribe, TTS_SAMPLE_RATE } from "./providers.ts";

const ALLOWED_REPO = "Sahnijatin/esp-box-assistant";
const OIDC_AUDIENCE = "esp-box-assistant";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  { auth: { persistSession: false } },
);

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, content-type, x-device-id, x-device-token",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
};

const PROVIDERS = ["openai", "anthropic", "gemini", "groq", "openrouter", "elevenlabs", "sarvam"];

// ---------- language handling ----------

const LANG_RULES: Record<string, string> = {
  en: "Always reply in English.",
  hi: "Always reply in Hindi, written in Devanagari script.",
  hinglish: [
    "Reply in Hinglish: casual Hindi mixed with English, the way people talk in India.",
    "Format your reply as exactly two lines and nothing else:",
    "DISPLAY: the reply written fully in Roman (Latin) script, e.g. Haan, Delhi India ki capital hai.",
    "SPEECH: the same reply with Hindi words in Devanagari and English words in Latin script, e.g. हाँ, Delhi India की capital है।",
  ].join("\n"),
  auto: "Reply in the same language the user spoke. Write Hindi in Devanagari script.",
};

function systemPrompt(s: any): string {
  return `${s.system_prompt}\n\n${LANG_RULES[s.language] ?? ""}`.trim();
}

// Hinglish replies carry a Roman version for the screen and a mixed-script version for the voice
function splitReply(raw: string, language: string): { display: string; speech: string } {
  if (language !== "hinglish") return { display: raw, speech: raw };
  const d = raw.match(/DISPLAY:\s*([\s\S]*?)(?:\s*SPEECH:|$)/i);
  const sp = raw.match(/SPEECH:\s*([\s\S]*)$/i);
  const display = (d?.[1] ?? raw.replace(/SPEECH:[\s\S]*$/i, "")).trim();
  return { display, speech: (sp?.[1] ?? display).trim() };
}

// The box's built-in font only has basic Latin: swap typographic characters for plain ones
function plainPunct(t: string): string {
  return t.replace(/[‘’]/g, "'").replace(/[“”]/g, '"').replace(/[–—]/g, "-")
    .replace(/…/g, "...").replace(/₹/g, "Rs ").replace(/ /g, " ");
}

// Text the box's own font can't draw (it only has ASCII)
function needsRender(...texts: (string | null | undefined)[]): boolean {
  return texts.some((t) => t && /[^\u0000-\u007F]/.test(plainPunct(t)));
}

class HttpError extends Error {
  constructor(public status: number, message: string) { super(message); }
}

function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status, headers: { ...CORS, "Content-Type": "application/json" },
  });
}

async function sha256hex(s: string): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function pairCode(): string {
  const n = crypto.getRandomValues(new Uint32Array(1))[0] % 1_000_000;
  return n.toString().padStart(6, "0");
}

// deno-lint-ignore no-explicit-any
async function must(p: PromiseLike<{ data: any; error: any }>): Promise<any> {
  const { data, error } = await p;
  if (error) throw new HttpError(500, error.message);
  return data;
}

async function getSettings() {
  return await must(db.from("settings").select("*").eq("id", 1).single());
}

async function providerKey(provider: string): Promise<string> {
  const key = await must(db.rpc("get_provider_key", { p_provider: provider }));
  if (!key) throw new HttpError(400, `No API key saved for ${provider}. Add it on the setup page.`);
  return key as string;
}

// ======================= device =======================

async function deviceAuth(req: Request) {
  const id = req.headers.get("x-device-id") ?? "";
  const token = req.headers.get("x-device-token") ?? "";
  const row = await must(db.from("devices").select("*").eq("id", id).maybeSingle());
  if (!row || row.token_hash !== await sha256hex(token)) throw new HttpError(401, "unknown device");
  if (row.status !== "active") throw new HttpError(403, `device ${row.status}`);
  return row;
}

async function latestVersion(): Promise<string | null> {
  const latest = await must(db.from("firmware_releases").select("version").order("created_at", { ascending: false }).limit(1));
  return latest[0]?.version ?? null;
}

async function deviceConfig(row: any, clearOtaFailed = false) {
  const s = await getSettings();
  const wifi = await must(db.from("wifi_networks").select("ssid,password,priority").order("priority", { ascending: false }));
  let update = null;
  // A box-specific "Update now" wins; otherwise follow the automatic update setting
  let target: string | null = row.update_to ?? null;
  if (!target && s.auto_update) target = s.pinned_version ?? await latestVersion();
  if (target && target !== row.fw_version && target !== row.ota_failed) {
    const rel = await must(db.from("firmware_releases").select("*").eq("version", target).maybeSingle());
    if (rel) {
      const { data } = await db.storage.from("firmware").createSignedUrl(rel.ota_path, 900);
      if (data?.signedUrl) update = { version: rel.version, url: data.signedUrl, size: rel.size };
    }
  }
  return {
    volume: s.volume, brightness: s.brightness, poll_seconds: s.poll_seconds,
    wifi, update, ...(clearOtaFailed ? { clear_ota_failed: true } : {}),
  };
}

async function deviceHello(req: Request) {
  const b = await req.json();
  const id = String(b.id ?? "").toLowerCase();
  const token = String(b.token ?? "");
  if (!/^[0-9a-f]{12}$/.test(id) || token.length < 32 || token.length > 128) throw new HttpError(400, "bad id/token");
  const hash = await sha256hex(token);
  // Unauthenticated input: keep it to short strings and sane numbers
  const str = (v: unknown, max: number) => (typeof v === "string" && v ? v.slice(0, max) : null);
  const int = (v: unknown) => (typeof v === "number" && Number.isFinite(v) ? Math.round(v) : null);
  const telemetry: Record<string, unknown> = {
    fw_version: str(b.fw, 40), ip: str(b.ip, 45), ssid: str(b.ssid, 32),
    rssi: int(b.rssi), free_heap: int(b.heap),
    ota_failed: str(b.ota_failed, 40),
    last_seen: new Date().toISOString(),
  };
  const newPairing = () => ({
    pair_code: pairCode(),
    pair_expires: new Date(Date.now() + 30 * 60_000).toISOString(),
  });

  let row = await must(db.from("devices").select("*").eq("id", id).maybeSingle());
  if (!row) {
    // Anyone can say hello, so keep the pending list short: drop boxes that never
    // got paired and went quiet, then cap how many may wait at once.
    const stale = new Date(Date.now() - 24 * 3600_000).toISOString();
    await must(db.from("devices").delete().eq("status", "pending").lt("last_seen", stale));
    const { count, error } = await db.from("devices").select("id", { count: "exact", head: true }).eq("status", "pending");
    if (error) throw new HttpError(500, error.message);
    if ((count ?? 0) >= 10) throw new HttpError(429, "Too many boxes are waiting to be paired. Pair or remove them first.");
    const p = newPairing();
    await must(db.from("devices").insert({ id, token_hash: hash, status: "pending", ...p, ...telemetry }));
    return json({ status: "pending", pair_code: p.pair_code });
  }
  if (row.status === "blocked") return json({ status: "blocked" }, 403);
  if (row.token_hash !== hash && row.status === "active") {
    // Either the box was wiped or someone is using its id. Never re-pair a paired
    // box from here: flag it, and the owner removes it on the setup page to pair again.
    await must(db.from("devices").update({ token_conflict: telemetry.last_seen }).eq("id", id));
    return json({ status: "token_mismatch" }, 409);
  }
  if (row.token_hash !== hash) {
    // Not paired yet and the box made a new token (wiped before pairing): new code.
    const p = newPairing();
    await must(db.from("devices").update({ token_hash: hash, status: "pending", ...p, ...telemetry }).eq("id", id));
    return json({ status: "pending", pair_code: p.pair_code });
  }
  if (row.status === "pending") {
    let code = row.pair_code;
    const upd: any = { ...telemetry };
    if (!code || new Date(row.pair_expires) < new Date()) {
      Object.assign(upd, newPairing());
      code = upd.pair_code;
    }
    await must(db.from("devices").update(upd).eq("id", id));
    return json({ status: "pending", pair_code: code });
  }
  // "Retry update" on the setup page: tell the box to forget the failed version
  let clearOtaFailed = false;
  if (row.ota_retry) {
    if (telemetry.ota_failed) {
      telemetry.ota_failed = null;
      clearOtaFailed = true;
    } else {
      telemetry.ota_retry = false;   // the box has forgotten it
    }
  }
  // "Update now" is done once the box runs that version, or has given up on it
  if (row.update_to && (telemetry.fw_version === row.update_to || telemetry.ota_failed === row.update_to)) {
    telemetry.update_to = null;
  }
  row = { ...row, ...telemetry };
  await must(db.from("devices").update(telemetry).eq("id", id));
  return json({ status: "active", name: row.name, config: await deviceConfig(row, clearOtaFailed) });
}

async function deviceAsk(req: Request) {
  const [dev, s] = await Promise.all([deviceAuth(req), getSettings()]);
  const wav = new Uint8Array(await req.arrayBuffer());
  if (wav.length < 8000) return json({ error: "Recording too short" });
  const t0 = Date.now();
  let question = "";
  try {
    // Fetch the chat key and recent history while the audio is being transcribed
    const since = new Date(Date.now() - 10 * 60_000).toISOString();
    const llmKey = providerKey(s.llm_provider);
    const history = s.max_history > 0
      ? must(db.from("conversations").select("question,answer")
        .eq("device_id", dev.id).is("error", null).gte("created_at", since)
        .order("created_at", { ascending: false }).limit(s.max_history))
      : Promise.resolve([]);
    llmKey.catch(() => {});    // awaited below; avoid an unhandled rejection on early return
    history.catch(() => {});

    question = await transcribe(s.stt_provider, s.stt_model, await providerKey(s.stt_provider), wav, s.language);
    if (!question) return json({ question: "", answer: "", error: "I didn't catch that." });

    const hist = await history;
    const messages: Msg[] = [];
    for (const h of hist.reverse()) {
      const prev = s.language === "hinglish" ? `DISPLAY: ${h.answer}` : h.answer;
      messages.push({ role: "user", content: h.question }, { role: "assistant", content: prev });
    }
    messages.push({ role: "user", content: question });

    const raw = await chat(s.llm_provider, s.llm_model, await llmKey, systemPrompt(s), messages);
    const { display, speech } = splitReply(raw, s.language);
    await db.from("conversations").insert({
      device_id: dev.id, question, answer: display, llm_provider: s.llm_provider, llm_model: s.llm_model,
      latency_ms: Date.now() - t0,
    });
    const render = needsRender(question, display);
    return json({
      question: render ? question : plainPunct(question), answer: render ? display : plainPunct(display),
      speech: speech !== display ? speech : undefined,
      render,
    });
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    await db.from("conversations").insert({
      device_id: dev.id, question, error: msg.slice(0, 1000), llm_provider: s.llm_provider,
      llm_model: s.llm_model, latency_ms: Date.now() - t0,
    });
    return json({ question, error: msg.slice(0, 200) });
  }
}

async function ttsResponse(text: string, settings?: Promise<any>) {
  const s = await (settings ?? getSettings());
  const audio = await speak(s.tts_provider, s.tts_model, s.tts_voice, await providerKey(s.tts_provider), text.slice(0, 1500), s.language, Number(s.tts_speed ?? 1));
  return new Response(audio, {
    headers: { ...CORS, "Content-Type": "application/octet-stream", "X-Sample-Rate": String(TTS_SAMPLE_RATE) },
  });
}

// Draws question + answer as an RGB565 bitmap for scripts the box can't shape itself
async function deviceRender(req: Request) {
  await deviceAuth(req);
  const b = await req.json();
  const width = Math.min(Math.max(Number(b.width) || 300, 100), 320);
  const { renderBlocks } = await import("./render.ts");
  const out = await renderBlocks([
    { text: b.question ? `You: ${b.question}` : "", size: 14, color: "#9aa3b2" },
    { text: String(b.answer ?? ""), size: 17, color: "#e8eaf0" },
  ], width);
  return new Response(out, { headers: { ...CORS, "Content-Type": "application/octet-stream" } });
}

async function deviceTts(req: Request) {
  const settings = getSettings();
  settings.catch(() => {});
  await deviceAuth(req);
  const { text } = await req.json();
  if (!text) throw new HttpError(400, "no text");
  return await ttsResponse(String(text), settings);
}

// ======================= admin =======================

const SESSION_DAYS = 30;

function bearer(req: Request): string {
  return (req.headers.get("authorization") ?? "").replace(/^Bearer\s+/i, "");
}

function clientIp(req: Request): string {
  return req.headers.get("cf-connecting-ip") ??
    req.headers.get("x-forwarded-for")?.split(",")[0].trim() ?? "unknown";
}

// Checks the password with the per-IP / global failure limits
async function checkPassword(req: Request, password: string) {
  if (!password) throw new HttpError(401, "password required");
  const r = await must(db.rpc("admin_login", { p_password: password, p_ip: clientIp(req) }));
  if (r === "locked") throw new HttpError(429, "Too many wrong passwords. Try again in 15 minutes.");
  if (r !== "ok") throw new HttpError(401, "wrong password");
}

async function newSession(): Promise<string> {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  const token = [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
  const now = new Date();
  await must(db.from("admin_sessions").delete().lt("expires_at", now.toISOString()));
  await must(db.from("admin_sessions").insert({
    token_hash: await sha256hex(token),
    expires_at: new Date(now.getTime() + SESSION_DAYS * 86400_000).toISOString(),
  }));
  return token;
}

async function adminAuth(req: Request): Promise<string> {
  const token = bearer(req);
  if (!token) throw new HttpError(401, "login required");
  const hash = await sha256hex(token);
  const row = await must(db.from("admin_sessions").select("token_hash")
    .eq("token_hash", hash).gt("expires_at", new Date().toISOString()).maybeSingle());
  if (!row) throw new HttpError(401, "session expired, log in again");
  return hash;
}

const SETTING_FIELDS: Record<string, "str" | "int" | "num" | "bool" | "nstr"> = {
  llm_provider: "str", llm_model: "str", stt_provider: "str", stt_model: "str",
  tts_provider: "str", tts_model: "str", tts_voice: "str", tts_speed: "num", system_prompt: "str",
  language: "str", max_history: "int", volume: "int", brightness: "int",
  poll_seconds: "int", auto_update: "bool", pinned_version: "nstr",
};

async function admin(path: string, req: Request) {
  const body = req.method === "POST" ? await req.json().catch(() => ({})) : {};
  if (path === "/admin/login") {
    await checkPassword(req, String(body.password ?? ""));
    return json({ token: await newSession() });
  }
  const session = await adminAuth(req);

  switch (path) {
    case "/admin/state": {
      const [settings, keys, wifi, devices, releases, conversations, builds] = await Promise.all([
        getSettings(),
        must(db.from("provider_keys").select("provider,last4,updated_at")),
        must(db.from("wifi_networks").select("*").order("priority", { ascending: false })),
        must(db.from("devices").select("id,name,status,pair_code,fw_version,ota_failed,ota_retry,update_to,token_conflict,ip,ssid,rssi,free_heap,last_seen,created_at").order("created_at")),
        must(db.from("firmware_releases").select("*").order("created_at", { ascending: false }).limit(20)),
        must(db.from("conversations").select("*").order("created_at", { ascending: false }).limit(40)),
        must(db.from("build_logs").select("*").order("created_at", { ascending: false }).limit(5)),
      ]);
      return json({
        settings,
        keys: keys.filter((k: any) => k.last4 !== ""),
        wifi: wifi.map((w: any) => ({ ...w, password: w.password ? "••••••" : "" })),
        devices, releases, conversations,
        builds: builds.map((b: any) => ({ ...b, log: (b.log ?? "").slice(-6000) })),
      });
    }
    case "/admin/settings": {
      const upd: Record<string, unknown> = { updated_at: new Date().toISOString() };
      for (const [k, t] of Object.entries(SETTING_FIELDS)) {
        if (!(k in body)) continue;
        const v = body[k];
        if (t === "int") {
          const n = Math.round(Number(v));
          if (!Number.isFinite(n)) throw new HttpError(400, `${k} must be a number`);
          upd[k] = n;
        }
        else if (t === "num") {
          const n = Number(v);
          if (!Number.isFinite(n)) throw new HttpError(400, `${k} must be a number`);
          upd[k] = Math.round(n * 100) / 100;
        }
        else if (t === "bool") upd[k] = Boolean(v);
        else if (t === "nstr") upd[k] = v ? String(v).trim() : null;
        else upd[k] = String(v).trim();
      }
      return json(await must(db.from("settings").update(upd).eq("id", 1).select().single()));
    }
    case "/admin/key": {
      const provider = String(body.provider ?? "");
      if (!PROVIDERS.includes(provider)) throw new HttpError(400, "unknown provider");
      const key = String(body.key ?? "").trim();
      if (key.length > 400) throw new HttpError(400, "key too long");
      await must(db.rpc("set_provider_key", { p_provider: provider, p_key: key }));
      return json({ ok: true });
    }
    case "/admin/wifi": {
      const ssid = String(body.ssid ?? "").trim();
      if (!ssid || ssid.length > 32) throw new HttpError(400, "SSID must be 1-32 characters");
      const row: any = { ssid, priority: Math.round(Number(body.priority ?? 0)) };
      const pw = body.password;
      if (typeof pw === "string" && pw !== "••••••") {
        if (pw.length > 0 && (pw.length < 8 || pw.length > 63)) throw new HttpError(400, "Wi-Fi password must be 8-63 characters (or empty for open networks)");
        row.password = pw;
      }
      await must(db.from("wifi_networks").upsert(row, { onConflict: "ssid" }));
      return json({ ok: true });
    }
    case "/admin/wifi/delete": {
      await must(db.from("wifi_networks").delete().eq("ssid", String(body.ssid ?? "")));
      return json({ ok: true });
    }
    case "/admin/pair": {
      const code = String(body.code ?? "").replace(/\D/g, "");
      const rows = await must(db.from("devices").select("id,pair_expires").eq("status", "pending").eq("pair_code", code));
      const row = rows.find((r: any) => new Date(r.pair_expires) > new Date());
      if (!row) throw new HttpError(404, "No box is showing that code. Check the screen and try again.");
      const upd: any = { status: "active", pair_code: null, pair_expires: null };
      if (body.name) upd.name = String(body.name).slice(0, 40);
      await must(db.from("devices").update(upd).eq("id", row.id));
      return json({ ok: true, id: row.id });
    }
    case "/admin/device": {
      const upd: any = {};
      if (body.name) upd.name = String(body.name).slice(0, 40);
      if (["active", "blocked"].includes(body.status)) upd.status = body.status;
      if (body.clear_ota_failed) Object.assign(upd, { ota_failed: null, ota_retry: true });
      if (body.clear_conflict) upd.token_conflict = null;
      if (body.update_now) {
        const v = await latestVersion();
        if (!v) throw new HttpError(404, "No firmware has been built yet.");
        const dev = await must(db.from("devices").select("ota_failed").eq("id", String(body.id)).maybeSingle());
        upd.update_to = v;
        // Asking again for a version that failed on this box means "try it again"
        if (dev?.ota_failed === v) Object.assign(upd, { ota_failed: null, ota_retry: true });
      }
      if (!Object.keys(upd).length) throw new HttpError(400, "nothing to change");
      await must(db.from("devices").update(upd).eq("id", String(body.id)));
      return json({ ok: true });
    }
    case "/admin/device/delete": {
      await must(db.from("devices").delete().eq("id", String(body.id)));
      return json({ ok: true });
    }
    case "/admin/test": {
      const s = await getSettings();
      const t0 = Date.now();
      const raw = await chat(s.llm_provider, s.llm_model, await providerKey(s.llm_provider), systemPrompt(s),
        [{ role: "user", content: String(body.text || "Say hello in one sentence.") }]);
      const { display, speech } = splitReply(raw, s.language);
      return json({ answer: display, speech, ms: Date.now() - t0 });
    }
    case "/admin/tts": {
      return await ttsResponse(String(body.text || "Hello, I am your ESP box assistant."));
    }
    case "/admin/flash": {
      const rels = await must(db.from("firmware_releases").select("*").not("factory_path", "is", null)
        .order("created_at", { ascending: false }).limit(1));
      const rel = rels[0];
      if (!rel) throw new HttpError(404, "No firmware has been built yet.");
      const { data, error } = await db.storage.from("firmware").createSignedUrl(rel.factory_path, 3600);
      if (error) throw new HttpError(500, error.message);
      return json({
        version: rel.version,
        manifest: {
          name: "ESP Box Assistant", version: rel.version, new_install_prompt_erase: true,
          builds: [{ chipFamily: "ESP32-S3", parts: [{ path: data.signedUrl, offset: 0 }] }],
        },
      });
    }
    case "/admin/logout": {
      await must(db.from("admin_sessions").delete().eq("token_hash", session));
      return json({ ok: true });
    }
    case "/admin/password": {
      const pw = String(body.password ?? "");
      if (pw.length < 10) throw new HttpError(400, "Use at least 10 characters");
      await checkPassword(req, String(body.current ?? ""));
      await must(db.rpc("set_admin_password", { p_password: pw }));
      // Log out every other browser
      await must(db.from("admin_sessions").delete().neq("token_hash", session));
      return json({ ok: true });
    }
  }
  throw new HttpError(404, "not found");
}

// ======================= CI (GitHub Actions OIDC) =======================

const JWKS = createRemoteJWKSet(new URL("https://token.actions.githubusercontent.com/.well-known/jwks"));
const VERSION_RE = /^[0-9A-Za-z._-]{1,40}$/;

async function ci(path: string, req: Request) {
  const token = (req.headers.get("authorization") ?? "").replace(/^Bearer\s+/i, "");
  let claims: any;
  try {
    ({ payload: claims } = await jwtVerify(token, JWKS, {
      issuer: "https://token.actions.githubusercontent.com", audience: OIDC_AUDIENCE,
    }));
  } catch (_) {
    throw new HttpError(401, "invalid CI token");
  }
  if (claims.repository !== ALLOWED_REPO) throw new HttpError(403, "repo not allowed");
  const body = await req.json();
  const onMain = claims.ref === "refs/heads/main";

  if (path === "/ci/log") {
    await must(db.from("build_logs").insert({
      run_id: String(body.run_id ?? ""), version: String(body.version ?? ""),
      status: String(body.status ?? ""), log: String(body.log ?? "").slice(-20000),
    }));
    return json({ ok: true });
  }
  if (!onMain) throw new HttpError(403, "only main branch can publish firmware");
  const version = String(body.version ?? "");
  if (!VERSION_RE.test(version)) throw new HttpError(400, "bad version");

  if (path === "/ci/upload-url") {
    const file = String(body.file ?? "");
    if (!["ota.bin", "factory.bin"].includes(file)) throw new HttpError(400, "bad file");
    const { data, error } = await db.storage.from("firmware").createSignedUploadUrl(`${version}/${file}`, { upsert: true });
    if (error) throw new HttpError(500, error.message);
    return json({ url: data.signedUrl });
  }
  if (path === "/ci/release") {
    await must(db.from("firmware_releases").upsert({
      version, git_sha: String(body.git_sha ?? ""), ota_path: `${version}/ota.bin`,
      factory_path: `${version}/factory.bin`, size: Number(body.size) || null,
      notes: String(body.notes ?? "").slice(0, 500),
    }, { onConflict: "version" }));
    return json({ ok: true });
  }
  throw new HttpError(404, "not found");
}

// ======================= router =======================

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response(null, { headers: CORS });
  const path = new URL(req.url).pathname.replace(/^\/(functions\/v1\/)?api/, "") || "/";
  try {
    if (path === "/device/hello") return await deviceHello(req);
    if (path === "/device/ask") return await deviceAsk(req);
    if (path === "/device/tts") return await deviceTts(req);
    if (path === "/device/render") return await deviceRender(req);
    if (path.startsWith("/admin/")) return await admin(path, req);
    if (path.startsWith("/ci/")) return await ci(path, req);
    if (path === "/health") return json({ ok: true });
    throw new HttpError(404, "not found");
  } catch (e) {
    if (e instanceof HttpError) return json({ error: e.message }, e.status);
    console.error(e);
    return json({ error: e instanceof Error ? e.message : String(e) }, 500);
  }
});
