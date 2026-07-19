import { applyD1Migrations } from "cloudflare:test";
import { invalidateSystemJobsCache } from "../src/scheduler";
import { invalidateEnvironmentStateCache } from "../src/telemetry_history";

export async function resetD1TestDatabase(
  db: D1Database,
  migrations: Parameters<typeof applyD1Migrations>[1],
): Promise<void> {
  invalidateSystemJobsCache(db);
  invalidateEnvironmentStateCache(db);
  await applyD1Migrations(db, migrations);
  await db.batch([
    db.prepare("DELETE FROM job_runs"),
    db.prepare("DELETE FROM device_commands"),
    db.prepare("DELETE FROM device_configs"),
    db.prepare("DELETE FROM device_metrics"),
    db.prepare("DELETE FROM environment_samples"),
    db.prepare("DELETE FROM environment_buckets"),
    db.prepare("DELETE FROM octopus_sync_ranges"),
    db.prepare("DELETE FROM octopus_backfill_state"),
    db.prepare("DELETE FROM octopus_readings"),
    db.prepare("DELETE FROM device_heartbeats"),
    db.prepare("DELETE FROM current_state"),
  ]);
  await db.prepare(
    "UPDATE jobs SET next_run_at=0, lease_until=NULL, last_success_at=NULL, last_error=NULL, consecutive_failures=0",
  ).run();
}
