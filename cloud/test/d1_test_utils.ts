import { applyD1Migrations } from "cloudflare:test";
import { invalidateSystemJobsCache } from "../src/scheduler";

export async function resetD1TestDatabase(
  db: D1Database,
  migrations: Parameters<typeof applyD1Migrations>[1],
): Promise<void> {
  invalidateSystemJobsCache(db);
  await applyD1Migrations(db, migrations);
  await db.batch([
    db.prepare("DELETE FROM job_runs"),
    db.prepare("DELETE FROM job_events"),
    db.prepare("DELETE FROM device_commands"),
    db.prepare("DELETE FROM device_configs"),
    db.prepare("DELETE FROM device_metrics"),
    db.prepare("DELETE FROM octopus_sync_state"),
    db.prepare("DELETE FROM octopus_daily_totals"),
    db.prepare("DELETE FROM device_heartbeats"),
    db.prepare("DELETE FROM current_state"),
  ]);
  await db.prepare(
    `UPDATE sync_manifest SET
       dashboard_version=0,
       environment_version=0,
       environment_fetched_at=0,
       radar_version=0,
       switchbot_version=0,
       stationhead_version=0,
       stationhead_health_version=0,
       updated_at=0
     WHERE id=1`,
  ).run();
  await db.prepare(
    "UPDATE jobs SET next_run_at=0, lease_until=NULL, last_success_at=NULL, last_error=NULL, consecutive_failures=0",
  ).run();
}
