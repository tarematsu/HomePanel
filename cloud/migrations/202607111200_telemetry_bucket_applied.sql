ALTER TABLE environment_samples ADD COLUMN bucket_applied INTEGER NOT NULL DEFAULT 1;
CREATE INDEX IF NOT EXISTS idx_environment_samples_pending_bucket
  ON environment_samples(device_id, bucket_applied, observed_at);
