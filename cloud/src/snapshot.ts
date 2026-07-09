import type { Env, SourceResult } from "./sources";

export const WORKER_VERSION = "2.11.0";

const EMPTY_OBJECT_HASH = "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";
const JST_MS = 9 * 60 * 60 * 1000;

export interface StateRow {
  source: string;
  version: number;
  payload: string;
  observed_at: number | null;
  fetched_at: number;
  last_success_at: number | null;
  status: "ok" | "stale" | "error";
  error: string | null;
  content_hash: string | null;
}

export interface DashboardStates {
  weather?: unknown;
  news?: unknown;
  octopus?: unknown;
  switchbot?: unknown;
  stationhead?: unknown;
  environment?: unknown;
}

export const DASHBOARD_SOURCE_NAMES = [
  "weather",
  "news",
  "octopus",
  "switchbot",
  "stationhead",
  "environment",
] as const;
const STATE_HEARTBEAT_MS = 15 * 60_000;

export async function sha256Hex(value: string | ArrayBuffer): Promise<string> {
  const bytes = typeof value === "string" ? new TextEncoder().encode(value) : new Uint8Array(value);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, "0")).join("");
}

export async function readState(env: Env, source: string): Promise<StateRow | null> {
  return env.DB.prepare(
    `SELECT source, version, payload, observed_at, fetched_at, last_success_at, status, error, content_hash
       FROM current_state WHERE source = ?1`,
  ).bind(source).first<StateRow>();
}

export async function readStates(env: Env, sources: readonly string[]): Promise<Record<string, StateRow>> {
  if (!sources.length) return {};
  const placeholders = sources.map(() => "?").join(",");
  const rows = await env.DB.prepare(
    `SELECT source, version, payload, observed_at, fetched_at, last_success_at, status, error, content_hash
       FROM current_state WHERE source IN (${placeholders})`,
  ).bind(...sources).all<StateRow>();
  return Object.fromEntries((rows.results ?? []).map(row => [row.source, row]));
}

function jstDayKey(timestamp: number): string {
  const date = new Date(timestamp + JST_MS);
  return `${date.getUTCFullYear()}-${String(date.getUTCMonth() + 1).padStart(2, "0")}-${String(date.getUTCDate()).padStart(2, "0")}`;
}

function dashboardSourcePayload(name: string, payload: Record<string, unknown>): Record<string, unknown> {
  if (name !== "octopus" || !Array.isArray(payload.history)) return payload;
  const yesterday = jstDayKey(Date.now() - 86_400_000);
  return {
    ...payload,
    history: payload.history.filter(item => {
      if (!item || typeof item !== "object" || Array.isArray(item)) return false;
      const date = String((item as Record<string, unknown>).date ?? "");
      return /^\d{4}-\d{2}-\d{2}$/.test(date) && date < yesterday;
    }),
  };
}

export function dashboardPayload(rows: Record<string, StateRow>): DashboardStates {
  const states: DashboardStates = {};
  for (const name of DASHBOARD_SOURCE_NAMES) {
    const row = rows[name];
    if (!row) continue;
    try {
      const parsed = JSON.parse(row.payload) as Record<string, unknown>;
      const payload = dashboardSourcePayload(name, parsed);
      states[name as keyof DashboardStates] = {
        ...payload,
        __status: row.status,
        __error: row.error,
        __lastSuccessAt: row.status === "ok" ? null : row.last_success_at,
        __version: row.version,
      } as never;
    } catch {
      states[name as keyof DashboardStates] = {
        __status: "error",
        __error: `${name} payload is invalid JSON`,
        __lastSuccessAt: row.last_success_at,
        __version: row.version,
      } as never;
    }
  }
  return states;
}

export function dashboardVersion(rows: Record<string, StateRow>): number {
  return DASHBOARD_SOURCE_NAMES.reduce((sum, source) => sum + Number(rows[source]?.version ?? 0), 0);
}

function dashboardStatus(rows: Record<string, StateRow>): StateRow["status"] {
  const statuses = DASHBOARD_SOURCE_NAMES.map(source => rows[source]?.status).filter(Boolean);
  if (statuses.includes("error")) return "error";
  if (statuses.includes("stale")) return "stale";
  return "ok";
}

export async function dashboardSnapshotFromRows(rows: Record<string, StateRow>): Promise<StateRow> {
  const payload = JSON.stringify(dashboardPayload(rows));
  const sourceRows = DASHBOARD_SOURCE_NAMES.map(source => rows[source]).filter((row): row is StateRow => Boolean(row));
  const fetchedAt = Math.max(0, ...sourceRows.map(row => Number(row.fetched_at ?? 0))) || Date.now();
  const observedAt = Math.max(0, ...sourceRows.map(row => Number(row.observed_at ?? 0))) || null;
  const lastSuccessAt = Math.max(0, ...sourceRows.map(row => Number(row.last_success_at ?? 0))) || null;
  const status = dashboardStatus(rows);
  const errors = sourceRows.filter(row => row.error).map(row => `${row.source}: ${row.error}`);
  return {
    source: "dashboard",
    version: dashboardVersion(rows),
    payload,
    observed_at: observedAt,
    fetched_at: fetchedAt,
    last_success_at: lastSuccessAt,
    status,
    error: errors.length ? errors.join("; ").slice(0, 1000) : null,
    content_hash: await sha256Hex(payload),
  };
}

export async function buildDashboardPayload(env: Env): Promise<DashboardStates> {
  return dashboardPayload(await readStates(env, DASHBOARD_SOURCE_NAMES));
}

function stablePayload(result: SourceResult): unknown {
  if (!result.payload || typeof result.payload !== "object" || Array.isArray(result.payload)) return result.payload;
  const copy = { ...(result.payload as Record<string, unknown>) };
  delete copy.observedAt;
  delete copy.generatedAt;
  delete copy.sampledAt;
  delete copy.monitorSampledAt;
  if (result.source !== "switchbot") delete copy.lastPowerPollAt;
  if (result.source === "stationhead") delete copy.progressMs;
  if (result.source === "switchbot" && Array.isArray(copy.devices)) {
    copy.devices = copy.devices.map(device => {
      if (!device || typeof device !== "object" || Array.isArray(device)) return device;
      const normalized = { ...(device as Record<string, unknown>) };
      delete normalized.observedAt;
      return normalized;
    });
  }
  return copy;
}

function attachedPrevious(result: SourceResult): StateRow | null | undefined {
  if (!Object.prototype.hasOwnProperty.call(result, "previousRow")) return undefined;
  return (result as SourceResult & { previousRow?: StateRow | null }).previousRow ?? null;
}

export async function updateState(
  env: Env,
  result: SourceResult,
  error?: string,
  knownPrevious?: StateRow | null,
): Promise<void> {
  const now = Date.now();
  const attached = attachedPrevious(result);
  const previous = knownPrevious !== undefined
    ? knownPrevious
    : attached !== undefined ? attached : await readState(env, result.source);
  if (error) {
    if (previous) {
      const nextStatus = previous.last_success_at === null ? "error" : "stale";
      if (previous.status === nextStatus && previous.error === error && now - previous.fetched_at < STATE_HEARTBEAT_MS) return;
      await env.DB.prepare(
        `UPDATE current_state
            SET fetched_at = ?1,
                status = CASE WHEN last_success_at IS NULL THEN 'error' ELSE 'stale' END,
                error = ?2
          WHERE source = ?3`,
      ).bind(now, error, result.source).run();
    } else {
      await env.DB.prepare(
        `INSERT INTO current_state(source, version, payload, observed_at, fetched_at, last_success_at, status, error, content_hash)
         VALUES(?1, 1, '{}', NULL, ?2, NULL, 'error', ?3, ?4)`,
      ).bind(result.source, now, error, EMPTY_OBJECT_HASH).run();
    }
    return;
  }

  const payload = JSON.stringify(result.payload);
  const stable = stablePayload(result);
  const hash = await sha256Hex(stable === result.payload ? payload : JSON.stringify(stable));
  if (previous?.content_hash === hash) {
    const heartbeatDue = now - previous.fetched_at >= STATE_HEARTBEAT_MS;
    const recovered = previous.status !== "ok" || previous.error !== null;
    if (!heartbeatDue && !recovered) return;
    await env.DB.prepare(
      `UPDATE current_state SET observed_at=?1, fetched_at=?2, last_success_at=?2, status='ok', error=NULL
       WHERE source=?3`,
    ).bind(result.observedAt, now, result.source).run();
    return;
  }

  await env.DB.prepare(
    `INSERT INTO current_state(source, version, payload, observed_at, fetched_at, last_success_at, status, error, content_hash)
     VALUES(?1, 1, ?2, ?3, ?4, ?4, 'ok', NULL, ?5)
     ON CONFLICT(source) DO UPDATE SET
       version = current_state.version + 1,
       payload = excluded.payload,
       observed_at = excluded.observed_at,
       fetched_at = excluded.fetched_at,
       last_success_at = excluded.last_success_at,
       status = 'ok', error = NULL, content_hash = excluded.content_hash`,
  ).bind(result.source, payload, result.observedAt, now, hash).run();
}

export async function ensureDashboard(env: Env): Promise<StateRow> {
  return dashboardSnapshotFromRows(await readStates(env, DASHBOARD_SOURCE_NAMES));
}

export interface MetaPayload {
  version: number;
  dashboardVersion: number;
  radarVersion: number;
  generatedAt: string;
  status: "ok" | "stale" | "error";
  workerVersion: string;
}

export async function buildMeta(env: Env): Promise<MetaPayload> {
  const rows = await readStates(env, [...DASHBOARD_SOURCE_NAMES, "radar"]);
  const radar = rows.radar;
  const dashboard = await dashboardSnapshotFromRows(rows);
  const status: MetaPayload["status"] = dashboard.status === "error" || radar?.status === "error"
    ? "error"
    : dashboard.status === "stale" || radar?.status === "stale" ? "stale" : "ok";
  const generated = Math.max(Number(dashboard.fetched_at ?? 0), Number(radar?.fetched_at ?? 0));
  return {
    version: dashboard.version + Number(radar?.version ?? 0),
    dashboardVersion: dashboard.version,
    radarVersion: Number(radar?.version ?? 0),
    generatedAt: new Date(generated || Date.now()).toISOString(),
    status,
    workerVersion: WORKER_VERSION,
  };
}
