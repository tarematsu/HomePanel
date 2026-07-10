import type { Env } from "./sources";
import { DEVICE_ID_PATTERN, deviceIdFromRequest as deviceIdFrom } from "./auth";
import { json } from "./http";
import {
  dashboardPayload,
  dashboardVersion,
  WORKER_VERSION,
  type StateRow,
} from "./snapshot";

const ALLOWED_COMMANDS = new Set([
  "restart_app",
  "reconnect_stationhead",
  "clear_display_cache",
  "reload_dashboard",
  "check_update",
]);
const COMMAND_REDELIVERY_MS = 90_000;

interface DeviceConfigRow {
  version: number;
  payload: string;
  updated_at: number;
}

interface DeviceCommandRow {
  id: number;
  command: string;
  payload: string | null;
  created_at: number;
  expires_at: number | null;
}

interface SyncRow {
  kind: "state" | "config" | "commands";
  source: string;
  version: number;
  payload: string | null;
  observed_at: number | null;
  fetched_at: number | null;
  last_success_at: number | null;
  status: StateRow["status"] | null;
  error: string | null;
  content_hash: string | null;
  updated_at: number | null;
  pending: number;
}

function configEtag(deviceId: string, version: number): string {
  return `"device-config-${deviceId}-${version}"`;
}

function objectOrNull(value: unknown): Record<string, unknown> | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function canonicalValue(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonicalValue);
  if (value && typeof value === "object") {
    const object = value as Record<string, unknown>;
    return Object.fromEntries(Object.keys(object).sort().map(key => [key, canonicalValue(object[key])]));
  }
  return value;
}

function canonicalJson(value: unknown): string {
  return JSON.stringify(canonicalValue(value));
}

function requestedVersion(url: URL, name: string): number {
  const value = Number(url.searchParams.get(name));
  return Number.isSafeInteger(value) && value >= 0 ? value : -1;
}

export async function enqueueCommandOnce(
  env: Env,
  deviceId: string,
  command: string,
  serialized: string | null,
  expiresInSeconds: number,
): Promise<{ id: number; deduplicated: boolean }> {
  const now = Date.now();
  const expiresAt = now + Math.max(60, Math.min(86_400, expiresInSeconds)) * 1000;
  for (let attempt = 0; attempt < 2; attempt += 1) {
    const inserted = await env.DB.prepare(
      `INSERT INTO device_commands(device_id, command, payload, created_at, expires_at)
       SELECT ?1, ?2, ?3, ?4, ?5
        WHERE NOT EXISTS (
          SELECT 1
            FROM device_commands
           WHERE device_id=?1
             AND command=?2
             AND payload IS ?3
             AND completed_at IS NULL
             AND (expires_at IS NULL OR expires_at>?4)
        )
       RETURNING id`,
    ).bind(deviceId, command, serialized, now, expiresAt).all<{ id: number }>();
    const insertedId = Number(inserted.results?.[0]?.id ?? 0);
    if (insertedId > 0) return { id: insertedId, deduplicated: false };

    const existing = await env.DB.prepare(
      `SELECT id
         FROM device_commands
        WHERE device_id=?1
          AND command=?2
          AND payload IS ?3
          AND completed_at IS NULL
          AND (expires_at IS NULL OR expires_at>?4)
        ORDER BY id DESC
        LIMIT 1`,
    ).bind(deviceId, command, serialized, now).first<{ id: number }>();
    const existingId = Number(existing?.id ?? 0);
    if (existingId > 0) return { id: existingId, deduplicated: true };
  }
  throw new Error("device command could not be queued");
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
  return (rows.results ?? [])
    .sort((left, right) => Number(left.id) - Number(right.id))
    .map(row => {
      let payload: unknown = null;
      try { payload = row.payload ? JSON.parse(row.payload) : null; } catch { payload = null; }
      return { id: row.id, command: row.command, payload, createdAt: row.created_at, expiresAt: row.expires_at };
    });
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
    config: requestedVersion(url, "configVersion"),
  };
  const now = Date.now();
  const rowsResult = await env.DB.prepare(
    `SELECT 'state' AS kind, source, version,
            CASE
              WHEN source IN ('weather','news','octopus','switchbot','stationhead','environment')
                   AND (SELECT COALESCE(SUM(version), 0)
                          FROM current_state
                         WHERE source IN ('weather','news','octopus','switchbot','stationhead','environment'))<>?4
                THEN payload
              WHEN source='radar' AND version<>?5 THEN payload
              WHEN source='switchbot' AND version<>?6 THEN payload
              WHEN source='stationhead' AND version<>?7 THEN payload
              ELSE NULL
            END AS payload,
            observed_at, fetched_at,
            last_success_at, status, error, content_hash,
            NULL AS updated_at, 0 AS pending
       FROM current_state
      WHERE source IN ('weather','news','octopus','switchbot','stationhead','environment','radar')
     UNION ALL
     SELECT 'config' AS kind, 'config' AS source, version,
            CASE WHEN version<>?8 THEN payload ELSE NULL END,
            NULL, NULL, NULL, NULL, NULL, NULL, updated_at, 0
       FROM device_configs
      WHERE device_id=?1
     UNION ALL
     SELECT 'commands' AS kind, 'commands' AS source, 0, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            EXISTS(
              SELECT 1 FROM device_commands
               WHERE device_id=?1
                 AND completed_at IS NULL
                 AND (expires_at IS NULL OR expires_at>?2)
                 AND (delivered_at IS NULL OR delivered_at<=?3)
            )`,
  ).bind(
    deviceId,
    now,
    now - COMMAND_REDELIVERY_MS,
    requested.dashboard,
    requested.radar,
    requested.switchbot,
    requested.stationhead,
    requested.config,
  ).all<SyncRow>();
  const rows = rowsResult.results ?? [];
  const states: Record<string, StateRow> = {};
  for (const row of rows) {
    if (row.kind !== "state" || !row.status) continue;
    states[row.source] = {
      source: row.source,
      version: Number(row.version),
      payload: row.payload ?? "",
      observed_at: row.observed_at,
      fetched_at: Number(row.fetched_at ?? 0),
      last_success_at: row.last_success_at,
      status: row.status,
      error: row.error,
      content_hash: row.content_hash,
    };
  }
  const configRow = rows.find(row => row.kind === "config");
  const configVersion = Number(configRow?.version ?? 0);
  const hasPendingCommands = Number(rows.find(row => row.kind === "commands")?.pending ?? 0) === 1;
  const commands = hasPendingCommands ? await pendingCommands(env, deviceId, now) : [];
  const currentDashboardVersion = dashboardVersion(states);
  const radarVersion = Number(states.radar?.version ?? 0);
  const switchbotVersion = Number(states.switchbot?.version ?? 0);
  const stationheadVersion = Number(states.stationhead?.version ?? 0);
  const response: Record<string, unknown> = {
    workerVersion: WORKER_VERSION,
    versions: {
      dashboard: currentDashboardVersion,
      radar: radarVersion,
      switchbot: switchbotVersion,
      stationhead: stationheadVersion,
      config: configVersion,
    },
    commands,
  };
  if (currentDashboardVersion !== requested.dashboard) {
    response.dashboard = JSON.stringify(dashboardPayload(states));
  }
  for (const source of ["radar", "switchbot", "stationhead"] as const) {
    const row = states[source];
    if (row && row.version !== requested[source]) response[source] = row.payload;
  }
  if (configVersion !== requested.config) {
    let value: unknown = {};
    try { value = configRow?.payload ? JSON.parse(configRow.payload) : {}; } catch { value = {}; }
    response.deviceConfig = JSON.stringify({
      deviceId,
      version: configVersion,
      updatedAt: Number(configRow?.updated_at ?? 0),
      config: value,
    });
  }
  return json(response);
}

export async function getDeviceConfig(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  const row = await env.DB.prepare(
    "SELECT version, payload, updated_at FROM device_configs WHERE device_id=?1",
  ).bind(deviceId).first<DeviceConfigRow>();
  const version = row?.version ?? 0;
  let config: unknown = {};
  if (row?.payload) {
    try { config = JSON.parse(row.payload); } catch { config = {}; }
  }
  const body = JSON.stringify({ deviceId, version, updatedAt: row?.updated_at ?? 0, config });
  const headers = new Headers({
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "private, max-age=0, must-revalidate",
    ETag: configEtag(deviceId, version),
  });
  if (request.headers.get("If-None-Match")?.split(",").map(value => value.trim()).includes(headers.get("ETag")!)) {
    return new Response(null, { status: 304, headers });
  }
  return new Response(body, { status: 200, headers });
}

export async function putDeviceConfig(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  let input: unknown;
  try { input = await request.json(); } catch { return json({ error: "invalid json" }, { status: 400 }); }
  const config = objectOrNull(input);
  if (!config) return json({ error: "config must be an object" }, { status: 400 });
  const payload = canonicalJson(config);
  if (payload.length > 32_000) {
    return json({ error: "config is too large" }, { status: 413 });
  }

  const existing = await env.DB.prepare(
    "SELECT version, payload, updated_at FROM device_configs WHERE device_id=?1",
  ).bind(deviceId).first<DeviceConfigRow>();
  const currentVersion = Number(existing?.version ?? 0);
  const currentEtag = configEtag(deviceId, currentVersion);
  const suppliedEtags = request.headers.get("If-Match")?.split(",").map(value => value.trim()) ?? [];
  if (!suppliedEtags.length) {
    return json(
      { error: "device config precondition required", deviceId, currentVersion },
      { status: 428, headers: { ETag: currentEtag } },
    );
  }
  if (!suppliedEtags.includes(currentEtag)) {
    return json(
      { error: "device config changed; reload before saving", deviceId, currentVersion },
      { status: 412, headers: { ETag: currentEtag } },
    );
  }
  if (existing?.payload === payload) {
    return json(
      { saved: true, changed: false, deviceId, version: currentVersion, updatedAt: existing.updated_at },
      { headers: { ETag: currentEtag } },
    );
  }

  const now = Date.now();
  const nextVersion = currentVersion + 1;
  const restartPayload = canonicalJson({ reason: "device_config_updated" });
  const expiresAt = now + 3_600_000;
  const configStatement = existing
    ? env.DB.prepare(
      `UPDATE device_configs
          SET version=?2, payload=?3, updated_at=?4
        WHERE device_id=?1 AND version=?5`,
    ).bind(deviceId, nextVersion, payload, now, currentVersion)
    : env.DB.prepare(
      `INSERT OR IGNORE INTO device_configs(device_id, version, payload, updated_at)
       VALUES(?1, 1, ?2, ?3)`,
    ).bind(deviceId, payload, now);
  const supersedeStatement = env.DB.prepare(
    `UPDATE device_commands
        SET completed_at=?1, success=1, result='superseded by newer config'
      WHERE device_id=?2 AND command='restart_app' AND completed_at IS NULL
        AND EXISTS (
          SELECT 1 FROM device_configs
           WHERE device_id=?2 AND version=?3 AND payload=?4 AND updated_at=?1
        )`,
  ).bind(now, deviceId, nextVersion, payload);
  const restartStatement = env.DB.prepare(
    `INSERT INTO device_commands(device_id, command, payload, created_at, expires_at)
     SELECT ?1, 'restart_app', ?2, ?3, ?4
      WHERE EXISTS (
        SELECT 1 FROM device_configs
         WHERE device_id=?1 AND version=?5 AND payload=?6 AND updated_at=?3
      )`,
  ).bind(deviceId, restartPayload, now, expiresAt, nextVersion, payload);

  const results = await env.DB.batch([configStatement, supersedeStatement, restartStatement]);
  if (Number(results[0]?.meta.changes ?? 0) !== 1) {
    const latest = await env.DB.prepare(
      "SELECT version FROM device_configs WHERE device_id=?1",
    ).bind(deviceId).first<{ version: number }>();
    const latestVersion = Number(latest?.version ?? 0);
    return json(
      { error: "device config changed; reload before saving", deviceId, currentVersion: latestVersion },
      { status: 412, headers: { ETag: configEtag(deviceId, latestVersion) } },
    );
  }
  const commandId = Number(results[2]?.meta.last_row_id ?? 0);
  if (!commandId) throw new Error("restart command was not created");
  return json(
    { saved: true, changed: true, deviceId, version: nextVersion, restartCommandId: commandId },
    { headers: { ETag: configEtag(deviceId, nextVersion) } },
  );
}

export async function getDeviceCommands(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  return json({ deviceId, commands: await pendingCommands(env, deviceId, Date.now()) });
}

export async function createDeviceCommand(request: Request, env: Env): Promise<Response> {
  let input: Record<string, unknown>;
  try { input = objectOrNull(await request.json()) ?? {}; } catch { return json({ error: "invalid json" }, { status: 400 }); }
  const deviceId = String(input.deviceId ?? "").trim();
  const command = String(input.command ?? "").trim();
  if (!DEVICE_ID_PATTERN.test(deviceId) || !ALLOWED_COMMANDS.has(command)) {
    return json({ error: "invalid deviceId or command" }, { status: 400 });
  }
  const payload = input.payload ?? null;
  const serialized = payload === null ? null : canonicalJson(payload);
  if (serialized && serialized.length > 8_000) {
    return json({ error: "command payload is too large" }, { status: 413 });
  }
  const expiresInSeconds = Number(input.expiresInSeconds) || 3600;
  const queued = await enqueueCommandOnce(env, deviceId, command, serialized, expiresInSeconds);
  return json({ queued: true, ...queued, deviceId, command }, { status: 202 });
}

export async function acknowledgeDeviceCommand(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  let input: Record<string, unknown>;
  try { input = objectOrNull(await request.json()) ?? {}; } catch { return json({ error: "invalid json" }, { status: 400 }); }
  const id = Math.trunc(Number(input.id));
  const success = input.success !== false;
  const result = String(input.result ?? "").slice(0, 1000);
  if (!Number.isFinite(id) || id <= 0) return json({ error: "invalid command id" }, { status: 400 });
  const update = await env.DB.prepare(
    `UPDATE device_commands
        SET completed_at=?1, success=?2, result=?3
      WHERE id=?4 AND device_id=?5 AND completed_at IS NULL`,
  ).bind(Date.now(), success ? 1 : 0, result || null, id, deviceId).run();
  return json({ acknowledged: (update.meta.changes ?? 0) === 1 });
}
