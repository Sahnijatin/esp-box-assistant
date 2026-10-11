-- ESP Box Assistant backend schema
-- Every table has RLS enabled with no policies: only the edge function
-- (service_role) can read or write. The browser never talks to tables directly.

create extension if not exists pgcrypto with schema extensions;

-- Global assistant settings (single row)
create table public.settings (
  id            int primary key default 1 check (id = 1),
  llm_provider  text not null default 'openai',
  llm_model     text not null default 'gpt-4o-mini',
  stt_provider  text not null default 'openai',
  stt_model     text not null default 'gpt-4o-mini-transcribe',
  tts_provider  text not null default 'openai',
  tts_model     text not null default 'gpt-4o-mini-tts',
  tts_voice     text not null default 'alloy',
  system_prompt text not null default 'You are a helpful voice assistant running on a small desk device. Answer in 1 to 3 short sentences of plain spoken English. No markdown, no lists, no emojis.',
  language      text not null default 'en',
  max_history   int  not null default 6,
  volume        int  not null default 70 check (volume between 0 and 100),
  brightness    int  not null default 80 check (brightness between 5 and 100),
  poll_seconds  int  not null default 30 check (poll_seconds between 10 and 3600),
  auto_update   boolean not null default true,
  pinned_version text,
  updated_at    timestamptz not null default now()
);
insert into public.settings (id) values (1);

-- Provider API keys: values live encrypted in Supabase Vault, this table only keeps a reference.
-- Removing a key = setting it to an empty string.
create table public.provider_keys (
  provider   text primary key,
  secret_id  uuid not null,
  last4      text not null,
  updated_at timestamptz not null default now()
);

-- Wi-Fi networks pushed to every box
create table public.wifi_networks (
  ssid       text primary key,
  password   text not null default '',
  priority   int  not null default 0,
  created_at timestamptz not null default now()
);

-- Boxes
create table public.devices (
  id           text primary key,              -- MAC address hex
  name         text not null default 'ESP Box',
  token_hash   text not null,
  status       text not null default 'pending' check (status in ('pending','active','blocked')),
  pair_code    text,
  pair_expires timestamptz,
  fw_version   text,
  ota_failed   text,
  ip           text,
  ssid         text,
  rssi         int,
  free_heap    int,
  last_seen    timestamptz,
  created_at   timestamptz not null default now()
);

-- Firmware builds uploaded by CI
create table public.firmware_releases (
  version      text primary key,
  git_sha      text,
  ota_path     text not null,
  factory_path text,
  size         int,
  notes        text,
  created_at   timestamptz not null default now()
);

-- Q&A log
create table public.conversations (
  id          bigint generated always as identity primary key,
  device_id   text references public.devices(id) on delete cascade,
  question    text,
  answer      text,
  llm_provider text,
  llm_model   text,
  latency_ms  int,
  error       text,
  created_at  timestamptz not null default now()
);
create index conversations_device_time on public.conversations (device_id, created_at desc);

-- CI build logs (so failures are visible without GitHub access)
create table public.build_logs (
  id         bigint generated always as identity primary key,
  run_id     text,
  version    text,
  status     text,
  log        text,
  created_at timestamptz not null default now()
);

-- Admin password (bcrypt)
create table public.admin_auth (
  id            int primary key default 1 check (id = 1),
  password_hash text not null
);

alter table public.settings          enable row level security;
alter table public.provider_keys     enable row level security;
alter table public.wifi_networks     enable row level security;
alter table public.devices           enable row level security;
alter table public.firmware_releases enable row level security;
alter table public.conversations     enable row level security;
alter table public.build_logs        enable row level security;
alter table public.admin_auth        enable row level security;

-- ---------- functions (service_role only) ----------

create or replace function public.verify_admin(p_password text)
returns boolean language sql security definer set search_path = public, extensions as $$
  select exists (select 1 from public.admin_auth
                 where password_hash = extensions.crypt(p_password, password_hash));
$$;

create or replace function public.set_admin_password(p_password text)
returns void language sql security definer set search_path = public, extensions as $$
  insert into public.admin_auth (id, password_hash)
  values (1, extensions.crypt(p_password, extensions.gen_salt('bf', 10)))
  on conflict (id) do update set password_hash = excluded.password_hash;
$$;

create or replace function public.set_provider_key(p_provider text, p_key text)
returns void language plpgsql security definer set search_path = public, vault as $$
declare v_id uuid;
begin
  select secret_id into v_id from public.provider_keys where provider = p_provider;
  if v_id is null then
    v_id := vault.create_secret(p_key, 'provider_key_' || p_provider);
    insert into public.provider_keys (provider, secret_id, last4) values (p_provider, v_id, right(p_key, 4));
  else
    perform vault.update_secret(v_id, p_key);
    update public.provider_keys set last4 = right(p_key, 4), updated_at = now() where provider = p_provider;
  end if;
end $$;

create or replace function public.get_provider_key(p_provider text)
returns text language sql security definer set search_path = public, vault as $$
  select s.decrypted_secret from public.provider_keys k
  join vault.decrypted_secrets s on s.id = k.secret_id
  where k.provider = p_provider;
$$;


revoke all on function public.verify_admin(text)              from public, anon, authenticated;
revoke all on function public.set_admin_password(text)        from public, anon, authenticated;
revoke all on function public.set_provider_key(text, text)    from public, anon, authenticated;
revoke all on function public.get_provider_key(text)          from public, anon, authenticated;
grant execute on function public.verify_admin(text)           to service_role;
grant execute on function public.set_admin_password(text)     to service_role;
grant execute on function public.set_provider_key(text, text) to service_role;
grant execute on function public.get_provider_key(text)       to service_role;

-- Private bucket for firmware images (downloaded via short-lived signed URLs)
insert into storage.buckets (id, name, public, file_size_limit)
values ('firmware', 'firmware', false, 16777216)
on conflict (id) do nothing;
