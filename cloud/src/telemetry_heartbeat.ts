import type { Env } from "./sources";

export interface TelemetryHeartbeatReceipt {
  last_sequence: number;
}

const HEARTBEAT_REFRESH_MS = 6 * 60 * 60_000;

export function telemetryHeartbeatReturningStatement(
  env: Env,
  deviceId: string,
  now: number,
  appVersion: string | null,
  stationheadOk: number,
  outboxCount: number,
  lastSequence: number,
): D1PreparedStatement {
  return env.DB.prepare(
    `INSERT INTO device_heartbeats(
       device_id,last_seen_at,app_version,stationhead_ok,outbox_count,payload,last_sequence
     ) VALUES(?1,?2,?3,?4,?5,NULL,?6)
     ON CONFLICT(device_id) DO UPDATE SET
       last_seen_at=excluded.last_seen_at,
       app_version=excluded.app_version,
       stationhead_ok=excluded.stationhead_ok,
       outbox_count=excluded.outbox_count,
       payload=NULL,
       last_sequence=MAX(device_heartbeats.last_sequence,excluded.last_sequence)
     WHERE device_heartbeats.app_version IS NOT excluded.app_version
        OR device_heartbeats.stationhead_ok IS NOT excluded.stationhead_ok
        OR device_heartbeats.outbox_count IS NOT excluded.outbox_count
        OR device_heartbeats.payload IS NOT NULL
        OR device_heartbeats.last_seen_at<=?7
     RETURNING last_sequence`,
  ).bind(
    deviceId,
    now,
    appVersion,
    stationheadOk,
    outboxCount,
    lastSequence,
    now - HEARTBEAT_REFRESH_MS,
  );
}
