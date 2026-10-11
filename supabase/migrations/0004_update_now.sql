-- "Update now" from the setup page: version this box should install at its next
-- check-in, even when automatic updates are off. Cleared once it runs it (or gives up).
alter table public.devices add column if not exists update_to text;
