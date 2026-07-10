import { applyD1Migrations } from "cloudflare:test";

export async function resetD1TestDatabase(
  db: D1Database,
  migrations: Parameters<typeof applyD1Migrations>[1],
): Promise<void> {
  await applyD1Migrations(db, migrations);
  await db.batch([
    db.prepare("DELETE FROM job_runs"),
    db.prepare("DELETE FROM device_commands"),
    db.prepare("DELETE FROM device_configs"),
    db.prepare("DELETE FROM device_metrics"),
    db.prepare("DELETE FROM environment_samples"),
    db.prepare("DELETE FROM environment_buckets"),
    db.prepare("DELETE FROM device_heartbeats"),
    db.prepare("DELETE FROM current_state"),
  ]);
  await db.prepare(
    "UPDATE jobs SET next_run_at=0, lease_until=NULL, last_success_at=NULL, last_error=NULL, consecutive_failures=0",
  ).run();
}
