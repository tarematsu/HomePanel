import { deviceIdFromRequest as deviceIdFrom } from "./auth";
import { json } from "./http";
import {
  DASHBOARD_SOURCE_NAMES,
  dashboardPayload,
  WORKER_VERSION,
  type StateRow,
} from "./snapshot";
import type { Env } from "./sources";
import { stationheadHealthPayload } from "./stationhead_health";

const COMMAND_REDELIVERY_MS = 90_000;
const SYNC_SOURCE_NAMES = [...DASHBOARD_SOURCE_NAMES, "radar", "stationhead_health"] as const;
const SYNC_SOURCE_PLACEHOLDERS = SYNC_SOURCE_NAMES.map(() => "?").join(",");

type SyncSourceName = typeof SYNC_SOURCE_NAMES[number];
type SyncStateMetadata = Omit<StateRow, "payload">;

interface DeviceSyncAuxRow {
  config_version: number;
  config_updated_at: number;
  pending: number;
}

interface PayloadRow {
  source: SyncSourceName;
  payload: string;
}

interface DeviceCommandRow {
  id: number;
  command: string;
  payload: string | null;
  created_at: number;
  expires_at: number | null;
}

function requestedVersion(url: URL, name: string): number {
  const value = Number(url.searchParams.get(name));
  return Number.isSafeInteger(value) && value >= 0 ? value : -1;
}

async function pendingCommands(env: Env, deviceId: string, now: number): Promise<Array<Record<string, unknown>>> {
  const rows = await env.DB.prepare(
    `WITH pending AS (
       SELECT id
         FROM device_commands
        WHERE device_id=?1
          AND completed_at IS NULL
          AND (expires_at IS NULL OR expires_at>?2)
          AND (delivered_at IS NULL OR delivered_at<=?3)
        ORDER BY id
        LIMIT 10
     )
     UPDATE device_commands
        SET delivered_at=?2
      WHERE id IN (SELECT id FROM pending)
        AND device_id=?1
        AND completed_at IS NULL
        AND (expires_at IS NULL OR expires_at>?2)
        AND (delivered_at IS NULL OR delivered_at<=?3)
     RETURNING id, command, payload, created_at, expires_at`,
  ).bind(deviceId, now, now - COMMAND_REDELIVERY_MS).all<DeviceCommandRow>();
  const results = rows.results ?? [];
  results.sort((left, right) => Number(left.id) - Number(right.id));
  return results.map(row => {
    let payload: unknown = null;
    try { payload = row.payload ? JSON.parse(row.payload) : null; } catch { payload = null; }
    return {
      id: row.id,
      command: row.command,
      payload,
      createdAt: row.created_at,
      expiresAt: row.expires_at,
    };
  });
}

function stateRow(metadata: SyncStateMetadata, payload = ""): StateRow {
  return { ...metadata, payload };
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

  const [stateResult, auxResult] = await env.DB.batch([
    env.DB.prepare(
      `SELECT source,version,observed_at,fetched_at,last_success_at,status,error,content_hash
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

  const metadata = new Map<string, SyncStateMetadata>();
  let currentDashboardVersion = 0;
  let radarVersion = 0;
  let switchbotVersion = 0;
  let stationheadVersion = 0;
  let stationheadHealthVersion = 0;
  for (const raw of stateResult.results ?? []) {
    const row = raw as unknown as SyncStateMetadata;
    const version = Number(row.version);
    const normalized: SyncStateMetadata = {
      source: String(row.source),
      version,
      observed_at: row.observed_at,
      fetched_at: Number(row.fetched_at ?? 0),
      last_success_at: row.last_success_at,
      status: row.status,
      error: row.error,
      content_hash: row.content_hash,
    };
    metadata.set(normalized.source, normalized);
    if ((DASHBOARD_SOURCE_NAMES as readonly string[]).includes(normalized.source)) currentDashboardVersion += version;
    if (normalized.source === "radar") radarVersion = version;
    else if (normalized.source === "switchbot") switchbotVersion = version;
    else if (normalized.source === "stationhead") stationheadVersion = version;
    else if (normalized.source === "stationhead_health") stationheadHealthVersion = version;
  }

  const aux = (auxResult.results?.[0] ?? {}) as unknown as DeviceSyncAuxRow;
  const configVersion = Number(aux.config_version ?? 0);
  const configUpdatedAt = Number(aux.config_updated_at ?? 0);
  const commands = Number(aux.pending) === 1 ? await pendingCommands(env, deviceId, now) : [];
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
  const payloadSources = new Set<string>();
  if (dashboardChanged) for (const source of DASHBOARD_SOURCE_NAMES) payloadSources.add(source);
  if (radarVersion !== requested.radar) payloadSources.add("radar");
  if (switchbotVersion !== requested.switchbot) payloadSources.add("switchbot");
  if (stationheadVersion !== requested.stationhead) payloadSources.add("stationhead");
  if (stationheadHealthVersion !== requested.stationheadHealth) payloadSources.add("stationhead_health");

  const states: Record<string, StateRow> = {};
  if (payloadSources.size) {
    const names = [...payloadSources];
    const placeholders = names.map(() => "?").join(",");
    const payloadResult = await env.DB.prepare(
      `SELECT source,payload FROM current_state WHERE source IN (${placeholders})`,
    ).bind(...names).all<PayloadRow>();
    const payloads = new Map((payloadResult.results ?? []).map(row => [row.source, row.payload]));
    for (const source of names) {
      const row = metadata.get(source);
      if (row) states[source] = stateRow(row, payloads.get(source as SyncSourceName) ?? "");
    }
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
