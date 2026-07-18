import { jstDayKey, type Env, type SourceResult } from "./sources";

export const WORKER_VERSION = "2.11.0";

const EMPTY_OBJECT_HASH = "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";
const HEX_DIGITS = "0123456789abcdef";
const UTF8_ENCODER = new TextEncoder();

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
const META_SOURCE_NAMES = [...DASHBOARD_SOURCE_NAMES, "radar"] as const;
const META_SOURCE_PLACEHOLDERS = "?,?,?,?,?,?,?";
const STATE_HEARTBEAT_MS = 15 * 60_000;

type DashboardSourceName = typeof DASHBOARD_SOURCE_NAMES[number];
type CachedDashboardSource = { key: string; value: unknown };
const DASHBOARD_SOURCE_CACHE = new Map<DashboardSourceName, CachedDashboardSource>();

export async function sha256Hex(value: string | ArrayBuffer): Promise<string> {
  const bytes = typeof value === "string" ? UTF8_ENCODER.encode(value) : new Uint8Array(value);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  let output = "";
  for (const byte of digest) {
    output += HEX_DIGITS.charAt(byte >>> 4) + HEX_DIGITS.charAt(byte & 0x0f);
  }
  return output;
}

export async function readState(env: Env, source: string): Promise<StateRow | null> {
  return env.DB.prepare(
    `SELECT source, version, payload, observed_at, fetched_at, last_success_at, status, error, content_hash
       FROM current_state WHERE source = ?1`,
  ).bind(source).first<StateRow>();
}

export async function readStates(env: Env, sources: readonly string[]): Promise<Record<string, StateRow>> {
  if (!sources.length) return {};
  let placeholders = "?";
  for (let index = 1; index < sources.length; index += 1) placeholders += ",?";
  const result = await env.DB.prepare(
    `SELECT source, version, payload, observed_at, fetched_at, last_success_at, status, error, content_hash
       FROM current_state WHERE source IN (${placeholders})`,
  ).bind(...sources).all<StateRow>();
  const rows: Record<string, StateRow> = {};
  for (const row of result.results ?? []) rows[row.source] = row;
  return rows;
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

function dashboardSourceCacheKey(name: DashboardSourceName, row: StateRow): string {
  const octopusDay = name === "octopus" ? jstDayKey(Date.now() - 86_400_000) : "";
  return `${row.version}\u0000${row.status}\u0000${row.error ?? ""}\u0000${row.last_success_at ?? ""}\u0000${row.content_hash ?? ""}\u0000${octopusDay}`;
}

function transformedDashboardSource(name: DashboardSourceName, row: StateRow): unknown {
  const key = dashboardSourceCacheKey(name, row);
  const cached = DASHBOARD_SOURCE_CACHE.get(name);
  if (cached?.key === key) return cached.value;

  let value: unknown;
  try {
    const parsed = JSON.parse(row.payload) as Record<string, unknown>;
    const payload = dashboardSourcePayload(name, parsed);
    value = {
      ...payload,
      __status: row.status,
      __error: row.error,
      __lastSuccessAt: row.status === "ok" ? null : row.last_success_at,
      __version: row.version,
    };
  } catch {
    value = {
      __status: "error",
      __error: `${name} payload is invalid JSON`,
      __lastSuccessAt: row.last_success_at,
      __version: row.version,
    };
  }
  DASHBOARD_SOURCE_CACHE.set(name, { key, value });
  return value;
}

export function dashboardPayload(rows: Record<string, StateRow>): DashboardStates {
  const states: DashboardStates = {};
  for (const name of DASHBOARD_SOURCE_NAMES) {
    const row = rows[name];
    if (!row) continue;
    states[name as keyof DashboardStates] = transformedDashboardSource(name, row) as never;
  }
  return states;
}

export function dashboardVersion(rows: Record<string, Pick<StateRow, "version">>): number {
  let version = 0;
  for (const source of DASHBOARD_SOURCE_NAMES) version += Number(rows[source]?.version ?? 0);
  return version;
}

function dashboardStatus(rows: Record<string, Pick<StateRow, "status">>): StateRow["status"] {
  let stale = false;
  for (const source of DASHBOARD_SOURCE_NAMES) {
    const status = rows[source]?.status;
    if (status === "error") return "error";
    if (status === "stale") stale = true;
  }
  return stale ? "stale" : "ok";
}

export async function dashboardSnapshotFromRows(rows: Record<string, StateRow>): Promise<StateRow> {
  const payload = JSON.stringify(dashboardPayload(rows));
  let fetchedAt = 0;
  let observedAt = 0;
  let lastSuccessAt = 0;
  let version = 0;
  let status: StateRow["status"] = "ok";
  const errors: string[] = [];
  for (const source of DASHBOARD_SOURCE_NAMES) {
    const row = rows[source];
    if (!row) continue;
    version += Number(row.version ?? 0);
    fetchedAt = Math.max(fetchedAt, Number(row.fetched_at ?? 0));
    observedAt = Math.max(observedAt, Number(row.observed_at ?? 0));
    lastSuccessAt = Math.max(lastSuccessAt, Number(row.last_success_at ?? 0));
    if (row.status === "error") status = "error";
    else if (row.status === "stale" && status === "ok") status = "stale";
    if (row.error) errors.push(`${row.source}: ${row.error}`);
  }
  return {
    source: "dashboard",
    version,
    payload,
    observed_at: observedAt || null,
    fetched_at: fetchedAt || Date.now(),
    last_success_at: lastSuccessAt || null,
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
      `UPDATE current_state
          SET payload=?1, observed_at=?2, fetched_at=?3, last_success_at=?3,
              status='ok', error=NULL
        WHERE source=?4`,
    ).bind(payload, result.observedAt, now, result.source).run();
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

type StateMetadataRow = Pick<StateRow, "source" | "version" | "fetched_at" | "status">;

export async function buildMeta(env: Env): Promise<MetaPayload> {
  const result = await env.DB.prepare(
    `SELECT source, version, fetched_at, status
       FROM current_state WHERE source IN (${META_SOURCE_PLACEHOLDERS})`,
  ).bind(...META_SOURCE_NAMES).all<StateMetadataRow>();
  const rows: Record<string, StateMetadataRow> = {};
  for (const row of result.results ?? []) rows[row.source] = row;
  const radar = rows.radar;
  let version = 0;
  let dashboardFetchedAt = 0;
  let missingDashboardSource = false;
  let statusForDashboard: StateRow["status"] = "ok";
  for (const source of DASHBOARD_SOURCE_NAMES) {
    const row = rows[source];
    if (!row) {
      missingDashboardSource = true;
      continue;
    }
    version += Number(row.version ?? 0);
    dashboardFetchedAt = Math.max(dashboardFetchedAt, Number(row.fetched_at ?? 0));
    if (row.status === "error") statusForDashboard = "error";
    else if (row.status === "stale" && statusForDashboard === "ok") statusForDashboard = "stale";
  }
  const status: MetaPayload["status"] = statusForDashboard === "error" || radar?.status === "error"
    ? "error"
    : missingDashboardSource || statusForDashboard === "stale" || !radar || radar.status === "stale"
      ? "stale"
      : "ok";
  const generated = Math.max(dashboardFetchedAt || Date.now(), Number(radar?.fetched_at ?? 0));
  return {
    version: version + Number(radar?.version ?? 0),
    dashboardVersion: version,
    radarVersion: Number(radar?.version ?? 0),
    generatedAt: new Date(generated || Date.now()).toISOString(),
    status,
    workerVersion: WORKER_VERSION,
  };
}
