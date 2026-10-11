-- Speaking pace for the voice: 1.0 = the provider's normal speed
alter table public.settings add column if not exists tts_speed real not null default 1.0
  check (tts_speed between 0.5 and 2.0);
