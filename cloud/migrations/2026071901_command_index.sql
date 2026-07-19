CREATE INDEX IF NOT EXISTS idx_device_commands_dedupe ON device_commands(device_id, command, payload, id DESC, expires_at) WHERE completed_at IS NULL;

INSERT OR REPLACE INTO schema_meta(key, value) VALUES ('schema_version', '20260719-command-index');
