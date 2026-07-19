import { deviceIdFromRequest as deviceIdFrom } from "./auth";
import {
  claimPendingDeviceCommands,
  COMMAND_REDELIVERY_MS,
} from "./device_command_delivery";
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

interface DeviceSyncAuxRow {
  config_version: number;
  config_updated_at: number;
  pending: number;
}

function requestedVersion(url: URL, name: string): number {
  const value = Number(url.searchParams.get(name));
  return Number.isSafeInteger(value) && value >= 0 ? value : -1;
}

export async function getDeviceSync(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  const url = new URL(request.url);
  const requested = {
    dashboard: requestedVersion(url, "dashboardVersion"),
    radar: requestedVersion(url, "radarVersion"),
    switchbot: requestedVersion(url, "switchbotVersion"),
    stationhead: requestedVersion(url, "stationheadVersion"),
    stationheadHealth: requestedVersion(url, "stationheadHealthVersion"),
    config: requestedVersion(url, "configVersion"),
  };
  const now = Date.now();

  const [versionResult, auxResult] = await env.DB.batch([
    env.DB.prepare(
      `SELECT
         COALESCE(SUM(CASE
           WHEN source IN ('weather','news','octopus','switchbot','stationhead','environment')
           THEN version ELSE 0 END),0) AS dashboard_version,
         COALESCE(MAX(CASE WHEN source='radar' THEN version ELSE 0 END),0) AS radar_version,
         COALESCE(MAX(CASE WHEN source='switchbot' THEN version ELSE 0 END),0) AS switchbot_version,
         COALESCE(MAX(CASE WHEN source='stationhead' THEN version ELSE 0 END),0) AS stationhead_version,
         COALESCE(MAX(CASE WHEN source='stationhead_health' THEN version ELSE 0 END),0) AS stationhead_health_version
       FROM current_state
       WHERE source IN (${SYNC_SOURCE_PLACEHOLDERS})`,
    ).bind(...SYNC_SOURCE_NAMES),
    env.DB.prepare(
      `WITH config AS (
         SELECT version,updated_at FROM device_configs WHERE device_id=?1
       )
       SELECT
         COALESCE((SELECT version FROM config),0) AS config_version,
         COALESCE((SELECT updated_at FROM config),0) AS config_updated_at,
         EXISTS(
           SELECT 1 FROM device_commands
            WHERE device_id=?1
              AND completed_at IS NULL
              AND (expires_at IS NULL OR expires_at>?2)
              AND (delivered_at IS NULL OR delivered_at<=?3)
         ) AS pending`,
    ).bind(deviceId, now, now - COMMAND_REDELIVERY_MS),
  ]);

  const versions = normalizeDeviceSyncVersions(
    versionResult?.results?.[0] as Partial<ReturnType<typeof normalizeDeviceSyncVersions>> | undefined,
  );
  const currentDashboardVersion = versions.dashboard_version;
  const radarVersion = versions.radar_version;
  const switchbotVersion = versions.switchbot_version;
  const stationheadVersion = versions.stationhead_version;
  const stationheadHealthVersion = versions.stationhead_health_version;

  const aux = (auxResult?.results?.[0] ?? {}) as unknown as DeviceSyncAuxRow;
  const configVersion = Number(aux.config_version ?? 0);
  const configUpdatedAt = Number(aux.config_updated_at ?? 0);
  const commands = Number(aux.pending) === 1
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
    for (const row of stateResult.results ?? []) states[row.source] = row;
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
    const configRow = await env.DB.prepare(
      "SELECT payload FROM device_configs WHERE device_id=?1 AND version=?2",
    ).bind(deviceId, configVersion).first<{ payload: string }>();
    let value: unknown = {};
    try { value = configRow?.payload ? JSON.parse(configRow.payload) : {}; } catch { value = {}; }
    response.deviceConfig = JSON.stringify({
      deviceId,
      version: configVersion,
      updatedAt: configUpdatedAt,
      config: value,
    });
  }
  return json(response);
}
