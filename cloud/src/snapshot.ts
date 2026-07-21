import { jstDayKey, type Env, type SourceResult } from "./sources";
import { markStateChanged } from "./state_generation";
import { memoizedStateHash } from "./state_hash_cache";
import { readCachedState, writeCachedState } from "./state_cache";

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
const D1_STATE_CHECKPOINT_MS = 6 * 60 * 60_000;

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

export async function readStateFromD1(env: Env, source: string): Promise<StateRow | null> {
  return env.DB.prepare(
    `SELECT source, version, payload, observed_at, fetched_at, last_success_at, status, error, content_hash
       FROM current_state WHERE source = ?1`,
  ).bind(source).first<StateRow>();
}

export async function readState(env: Env, source: string): Promise<StateRow | null> {
  const cached = await readCachedState(env, source);
  if (cached) return cached;
  const row = await readStateFromD1(env, source);
  if (row) await writeCachedState(env, row);
  return row;
}

export async function readStates(env: Env, sources: readonly string[]): Promise<Record<string, StateRow>> {
  if (!sources.length) return {};
  const cached = await Promise.all(sources.map(source => readCachedState(env, source)));
  const rows: Record<string, StateRow> = {};
  const misses: string[] = [];
  sources.forEach((source, index) => {
    const row = cached[index];
    if (row) rows[source] = row;
    else misses.push(source);
  });
  if (!misses.length) return rows;

  let placeholders = "?";
  for (let index = 1; index < misses.length; index += 1) placeholders += ",?";
  const result = await env.DB.prepare(
    `SELECT source, version, payload, observed_at, fetched_at, last_success_at, status, error, content_hash
       FROM current_state WHERE source IN (${placeholders})`,
  ).bind(...misses).all<StateRow>();
  const fetched = result.results ?? [];
  for (const row of fetched) rows[row.source] = row;
  await Promise.all(fetched.map(row => writeCachedState(env, row)));
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

function shouldInvalidateState(previous: StateRow | null, next: StateRow): boolean {
  return !previous
    || previous.version !== next.version
    || previous.status !== next.status
    || previous.error !== next.error
    || previous.content_hash !== next.content_hash;
}

async function hydrateColdCache(env: Env, source: string): Promise<StateRow | null> {
  if (!env.STATE_CACHE) return null;
  const row = await readStateFromD1(env, source);
  if (row) await writeCachedState(env, row);
  return row;
}

export async function updateState(
  env: Env,
  result: SourceResult,
  error?: string,
  knownPrevious?: StateRow | null,
): Promise<void> {
  const now = Date.now();
  const checkpointBefore = now - D1_STATE_CHECKPOINT_MS;
  const previous = knownPrevious === undefined ? await readCachedState(env, result.source) : knownPrevious;
  const forceCheckpoint = previous === null ? 1 : 0;

  if (error) {
    const status: StateRow["status"] = previous?.last_success_at == null ? "error" : "stale";
    const next: StateRow = previous
      ? { ...previous, fetched_at: now, status, error }
      : {
        source: result.source,
        version: 1,
        payload: "{}",
        observed_at: null,
        fetched_at: now,
        last_success_at: null,
        status,
        error,
        content_hash: EMPTY_OBJECT_HASH,
      };
    const written = await env.DB.prepare(
      `INSERT INTO current_state(
         source,version,payload,observed_at,fetched_at,last_success_at,status,error,content_hash
       ) VALUES(?1,1,'{}',NULL,?2,NULL,'error',?3,?4)
       ON CONFLICT(source) DO UPDATE SET
         fetched_at=excluded.fetched_at,
         status=CASE WHEN current_state.last_success_at IS NULL THEN 'error' ELSE 'stale' END,
         error=excluded.error
       WHERE current_state.status<>CASE
               WHEN current_state.last_success_at IS NULL THEN 'error' ELSE 'stale' END
          OR current_state.error IS NOT excluded.error
          OR current_state.fetched_at<=?5
          OR ?6=1`,
    ).bind(result.source, now, error, EMPTY_OBJECT_HASH, checkpointBefore, forceCheckpoint).run();
    const current = previous ? next : await hydrateColdCache(env, result.source);
    if (current && (Number(written.meta.changes ?? 0) > 0 || shouldInvalidateState(previous, current))) {
      markStateChanged(env);
    }
    if (previous) await writeCachedState(env, next);
    return;
  }

  const payload = JSON.stringify(result.payload);
  const stable = stablePayload(result);
  const stableJson = stable === result.payload ? payload : JSON.stringify(stable);
  const hash = await memoizedStateHash(
    env,
    result.source,
    stableJson,
    () => sha256Hex(stableJson),
  );
  const contentChanged = !previous || previous.content_hash !== hash;
  const next: StateRow = {
    source: result.source,
    version: previous ? previous.version + (contentChanged ? 1 : 0) : 1,
    payload: contentChanged ? payload : previous?.payload ?? payload,
    observed_at: contentChanged ? result.observedAt : previous?.observed_at ?? result.observedAt,
    fetched_at: now,
    last_success_at: now,
    status: "ok",
    error: null,
    content_hash: hash,
  };
  const written = await env.DB.prepare(
    `INSERT INTO current_state(
       source,version,payload,observed_at,fetched_at,last_success_at,status,error,content_hash
     ) VALUES(?1,1,?2,?3,?4,?4,'ok',NULL,?5)
     ON CONFLICT(source) DO UPDATE SET
       version=CASE
         WHEN current_state.content_hash IS NOT excluded.content_hash THEN current_state.version+1
         ELSE current_state.version
       END,
       payload=CASE
         WHEN current_state.content_hash IS NOT excluded.content_hash THEN excluded.payload
         ELSE current_state.payload
       END,
       observed_at=CASE
         WHEN current_state.content_hash IS NOT excluded.content_hash THEN excluded.observed_at
         ELSE current_state.observed_at
       END,
       fetched_at=excluded.fetched_at,
       last_success_at=excluded.last_success_at,
       status='ok',
       error=NULL,
       content_hash=excluded.content_hash
     WHERE current_state.content_hash IS NOT excluded.content_hash
        OR current_state.status<>'ok'
        OR current_state.error IS NOT NULL
        OR current_state.fetched_at<=?6
        OR ?7=1`,
  ).bind(result.source, payload, result.observedAt, now, hash, checkpointBefore, forceCheckpoint).run();
  const current = previous ? next : await hydrateColdCache(env, result.source);
  if (current && (Number(written.meta.changes ?? 0) > 0 || shouldInvalidateState(previous, current))) {
    markStateChanged(env);
  }
  if (previous) await writeCachedState(env, next);
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
  const rows = await readStates(env, META_SOURCE_NAMES);
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
