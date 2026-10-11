-- Hardening: admin sessions + login rate limit, safe re-pairing, retryable updates

-- A box whose stored token doesn't match (wiped, or someone spoofing its id).
-- Active boxes are never re-paired automatically: the setup page shows this and
-- the owner removes the box to pair it again.
alter table public.devices add column if not exists token_conflict timestamptz;
-- Set from the setup page: the box should forget its failed update and try again
alter table public.devices add column if not exists ota_retry boolean not null default false;

-- Setup page sessions: the browser keeps a revocable token, never the password
create table if not exists public.admin_sessions (
  token_hash text primary key,
  created_at timestamptz not null default now(),
  expires_at timestamptz not null
);

-- Failed admin logins per client IP, plus one '*' row counting all of them
create table if not exists public.admin_login_failures (
  ip           text primary key,
  failures     int not null default 0,
  window_start timestamptz not null default now()
);

alter table public.admin_sessions       enable row level security;
alter table public.admin_login_failures enable row level security;

-- 'ok', 'bad' or 'locked'. Limits: 10 failures per IP and 50 overall per 15 minutes.
-- A lockout only blocks new logins; existing sessions keep working.
create or replace function public.admin_login(p_password text, p_ip text)
returns text language plpgsql security definer set search_path = public, extensions as $$
declare
  v_window constant interval := interval '15 minutes';
  v_key text;
begin
  if exists (select 1 from public.admin_login_failures
             where window_start > now() - v_window
               and ((ip = p_ip and failures >= 10) or (ip = '*' and failures >= 50))) then
    return 'locked';
  end if;
  if public.verify_admin(p_password) then
    delete from public.admin_login_failures where ip = p_ip;
    return 'ok';
  end if;
  foreach v_key in array array[p_ip, '*'] loop
    insert into public.admin_login_failures as f (ip, failures, window_start)
    values (v_key, 1, now())
    on conflict (ip) do update set
      failures     = case when f.window_start > now() - v_window then f.failures + 1 else 1 end,
      window_start = case when f.window_start > now() - v_window then f.window_start else now() end;
  end loop;
  return 'bad';
end $$;

revoke all on function public.admin_login(text, text) from public, anon, authenticated;
grant execute on function public.admin_login(text, text) to service_role;
