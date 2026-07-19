CREATE INDEX IF NOT EXISTS idx_environment_buckets_time_device
  ON environment_buckets(bucket_at, device_id);

INSERT OR REPLACE INTO schema_meta(key, value)
VALUES ('schema_version', '20260719-environment-bucket-time-index');
