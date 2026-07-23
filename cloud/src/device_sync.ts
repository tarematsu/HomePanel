import { deviceIdFromRequest as deviceIdFrom } from "./auth";
import {
  claimPendingDeviceCommands,
  COMMAND_REDELIVERY_MS,
} from "./device_command_delivery";
import { readR2EnvironmentState } from "./environment_r2";
import { json } from "./http";
import {
  DASHBOARD_SOURCE_NAMES,
  dashboardPayload,
  WORKER_VERSION,
  type StateRow,
} from "./snapshot";
import {
  normalizeDeviceSyncVersions,
  SYNC_SOURCE_NAMES,
  SYNC_SOURCE_PLACEHOLDERS,
} from "./device_sync_versions";
import type { Env } from "./sources";
import { stationheadHealthPayload } from "./stationhead_health";

type SyncSourceName = typeof SYNC_SOURCE_NAMES[number];

interface DeviceSyncSnapshotRow {
  dashboard_version: number;
  environment_version: number;
  environment_fetched_at: number;
  radar_version: number;
  switchbot_version: number;
  stationhead_version: number;
  stationhead_health_version: number;
  config_version: number;
  config_updated_at: number;
  config_payload: string | null;
  pending: number;
}

function requestedVersion(value: unknown): number {
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed >= 0 ? parsed : -1;
}

async function deviceSyncSnapshot(env: Env, deviceId: string, now: number): Promise<DeviceSyncSnapshotRow> {
  const deviceParameter = SYNC_SOURCE_NAMES.length + 1;
  const nowParameter = deviceParameter + 1;
  const redeliveryParameter = nowParameter + 1;
  const row = await env.DB.prepare(
    `WITH versions AS (
       SELECT
         COALESCE(SUM(CASE
           WHEN source IN ('weather','news','octopus','switchbot','stationhead','environment')
           THEN version ELSE 0 END),0) AS dashboard_version,
         COALESCE(MAX(CASE WHEN source='environment' THEN version ELSE 0 END),0) AS environment_version,
         COALESCE(MAX(CASE WHEN source='environment' THEN fetched_at ELSE 0 END),0) AS environment_fetched_at,
         COALESCE(MAX(CASE WHEN source='radar' THEN version ELSE 0 END),0) AS radar_version,
         COALESCE(MAX(CASE WHEN source='switchbot' THEN version ELSE 0 END),0) AS switchbot_version,
         COALESCE(MAX(CASE WHEN source='stationhead' THEN version ELSE 0 END),0) AS stationhead_version,
         COALESCE(MAX(CASE WHEN source='stationhead_health' THEN version ELSE 0 END),0) AS stationhead_health_version
       FROM current_state
       WHERE source IN (${SYNC_SOURCE_PLACEHOLDERS})
     ), config AS (
       SELECT version,updated_at,payload FROM device_configs WHERE device_id=?${deviceParameter}
     )
     SELECT
       versions.*,
       COALESCE((SELECT version FROM config),0) AS config_version,
       COALESCE((SELECT updated_at FROM config),0) AS config_updated_at,
       (SELECT payload FROM config) AS config_payload,
       EXISTS(
         SELECT 1 FROM device_commands
          WHERE device_id=?${deviceParameter}
            AND command='check_update'
            AND completed_at IS NULL
            AND (expires_at IS NULL OR expires_at>?${nowParameter})
            AND (delivered_at IS NULL OR delivered_at<=?${redeliveryParameter})
       ) AS pending
     FROM versions`,
  ).bind(
    ...SYNC_SOURCE_NAMES,
    deviceId,
    now,
    now - COMMAND_REDELIVERY_MS,
  ).first<DeviceSyncSnapshotRow>();
  return row ?? {
    dashboard_version: 0,
    environment_version: 0,
    environment_fetched_at: 0,
    radar_version: 0,
    switchbot_version: 0,
    stationhead_version: 0,
    stationhead_health_version: 0,
    config_version: 0,
    config_updated_at: 0,
    config_payload: null,
    pending: 0,
  };
}

function preferredEnvironmentState(
  snapshot: DeviceSyncSnapshotRow,
  r2: StateRow | null,
): StateRow | null {
  if (!r2) return null;
  const d1FetchedAt = Number(snapshot.environment_fetched_at ?? 0);
  return r2.fetched_at >= d1FetchedAt ? r2 : null;
}

export async function buildDeviceSyncPayloadForDevice(
  env: Env,
  deviceId: string,
  clientVersions: Record<string, unknown>,
): Promise<Record<string, unknown>> {
  if (!deviceId) throw new Error("valid deviceId is required");
  const requested = {
    dashboard: requestedVersion(clientVersions.dashboard),
    radar: requestedVersion(clientVersions.radar),
    switchbot: requestedVersion(clientVersions.switchbot),
    stationhead: requestedVersion(clientVersions.stationhead),
    stationheadHealth: requestedVersion(clientVersions.stationheadHealth),
    config: requestedVersion(clientVersions.config),
  };
  const now = Date.now();
  const [snapshot, r2Environment] = await Promise.all([
    deviceSyncSnapshot(env, deviceId, now),
    readR2EnvironmentState(env),
  ]);
  const versions = normalizeDeviceSyncVersions(snapshot);
  const environmentState = preferredEnvironmentState(snapshot, r2Environment);
  const d1EnvironmentVersion = Number(snapshot.environment_version ?? 0);
  const environmentVersion = environmentState?.version ?? d1EnvironmentVersion;
  const currentDashboardVersion = Math.max(
    0,
    versions.dashboard_version - d1EnvironmentVersion + environmentVersion,
  );
  const radarVersion = versions.radar_version;
  const switchbotVersion = versions.switchbot_version;
  const stationheadVersion = versions.stationhead_version;
  const stationheadHealthVersion = versions.stationhead_health_version;
  const configVersion = Number(snapshot.config_version ?? 0);
  const configUpdatedAt = Number(snapshot.config_updated_at ?? 0);
  const commands = Number(snapshot.pending) === 1
    ? await claimPendingDeviceCommands(env, deviceId, now)
    : [];
  const response: Record<string, unknown> = {
    workerVersion: WORKER_VERSION,
    versions: {
      dashboard: currentDashboardVersion,
      radar: radarVersion,
      switchbot: switchbotVersion,
      stationhead: stationheadVersion,
      stationheadHealth: stationheadHealthVersion,
      config: configVersion,
    },
    commands,
  };

  const dashboardChanged = currentDashboardVersion !== requested.dashboard;
  const payloadSources = new Set<SyncSourceName>();
  if (dashboardChanged) for (const source of DASHBOARD_SOURCE_NAMES) payloadSources.add(source);
  if (radarVersion !== requested.radar) payloadSources.add("radar");
  if (switchbotVersion !== requested.switchbot) payloadSources.add("switchbot");
  if (stationheadVersion !== requested.stationhead) payloadSources.add("stationhead");
  if (stationheadHealthVersion !== requested.stationheadHealth) payloadSources.add("stationhead_health");

  const states: Record<string, StateRow> = {};
  if (payloadSources.size) {
    const names = [...payloadSources];
    const placeholders = names.map(() => "?").join(",");
    const stateResult = await env.DB.prepare(
      `SELECT source,version,payload,observed_at,fetched_at,last_success_at,status,error,content_hash
         FROM current_state WHERE source IN (${placeholders})`,
    ).bind(...names).all<StateRow>();
    for (const state of stateResult.results ?? []) states[state.source] = state;
    if (dashboardChanged && environmentState) states.environment = environmentState;
  }

  if (dashboardChanged) response.dashboard = JSON.stringify(dashboardPayload(states));
  const radarState = states.radar;
  if (radarState && radarVersion !== requested.radar) response.radar = radarState.payload;
  const switchbotState = states.switchbot;
  if (switchbotState && switchbotVersion !== requested.switchbot) response.switchbot = switchbotState.payload;
  const stationheadState = states.stationhead;
  if (stationheadState && stationheadVersion !== requested.stationhead) response.stationhead = stationheadState.payload;
  const stationheadHealthState = states.stationhead_health;
  if (stationheadHealthState && stationheadHealthVersion !== requested.stationheadHealth) {
    response.stationheadHealth = JSON.stringify(stationheadHealthPayload(stationheadHealthState));
  }

  if (configVersion !== requested.config) {
    let value: unknown = {};
    try { value = snapshot.config_payload ? JSON.parse(snapshot.config_payload) : {}; } catch { value = {}; }
    response.deviceConfig = JSON.stringify({
      deviceId,
      version: configVersion,
      updatedAt: configUpdatedAt,
      config: value,
    });
  }
  return response;
}

export async function buildDeviceSyncPayload(request: Request, env: Env): Promise<Record<string, unknown>> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) throw new Error("valid deviceId is required");
  const params = new URL(request.url).searchParams;
  return buildDeviceSyncPayloadForDevice(env, deviceId, {
    dashboard: params.get("dashboardVersion"),
    radar: params.get("radarVersion"),
    switchbot: params.get("switchbotVersion"),
    stationhead: params.get("stationheadVersion"),
    stationheadHealth: params.get("stationheadHealthVersion"),
    config: params.get("configVersion"),
  });
}

export async function getDeviceSync(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  return json(await buildDeviceSyncPayload(request, env));
}
