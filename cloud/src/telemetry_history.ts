import { DEVICE_ID_PATTERN } from "./auth";
import { readState, sha256Hex, updateState, type StateRow } from "./snapshot";
import type { Env } from "./sources";
import type { EnvironmentHistoryRow } from "./telemetry_bucket";

interface EnvironmentDeviceHistory {
  deviceId: string;
  bucketMinutes: number;
  history: EnvironmentHistoryRow[];
}

interface StoredEnvironmentRow extends EnvironmentHistoryRow {
  device_id: string;
}

interface ParsedEnvironmentState {
  selectedDeviceId: string;
  firstDeviceId: string;
  devices: Record<string, EnvironmentDeviceHistory>;
}

const ENVIRONMENT_HISTORY_MS = 24 * 60 * 60 * 1000;
const FAST_MERGE_ATTEMPTS = 2;
const ENVIRONMENT_STATE_CACHE = new WeakMap<D1Database, StateRow | null>();

type FastMergeResult = "updated" | "not_applicable" | "conflict";

export function invalidateEnvironmentStateCache(db: D1Database): void {
  ENVIRONMENT_STATE_CACHE.delete(db);
}

async function readEnvironmentState(env: Env, force = false): Promise<StateRow | null> {
  if (!force && ENVIRONMENT_STATE_CACHE.has(env.DB)) {
    return ENVIRONMENT_STATE_CACHE.get(env.DB) ?? null;
  }
  const row = await readState(env, "environment");
  ENVIRONMENT_STATE_CACHE.set(env.DB, row);
  return row;
}

function previousSelectedDevice(previous: StateRow | null): string {
  if (!previous?.payload) return "";
  try {
    const parsed = JSON.parse(previous.payload) as unknown;
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return "";
    const deviceId = String((parsed as Record<string, unknown>).deviceId ?? "");
    return DEVICE_ID_PATTERN.test(deviceId) ? deviceId : "";
  } catch {
    return "";
  }
}

function nullableNumber(value: unknown, digits?: number): number | null {
  if (value === null || value === undefined) return null;
  const number = Number(value);
  if (!Number.isFinite(number)) return null;
  return digits === undefined ? Math.round(number) : Number(number.toFixed(digits));
}

function normalizedPoint(row: EnvironmentHistoryRow, t: number): EnvironmentHistoryRow {
  return {
    t,
    co2: nullableNumber(row.co2),
    temperature: nullableNumber(row.temperature, 2),
    humidity: nullableNumber(row.humidity, 2),
  };
}

function objectValue(value: unknown): Record<string, unknown> | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function normalizedStoredHistory(value: unknown, cutoff: number): EnvironmentHistoryRow[] | null {
  const device = objectValue(value);
  if (!device || Number(device.bucketMinutes ?? 5) !== 5 || !Array.isArray(device.history)) return null;

  const history: EnvironmentHistoryRow[] = [];
  let previousTime = Number.NEGATIVE_INFINITY;
  for (const raw of device.history) {
    const row = objectValue(raw);
    if (!row) return null;
    const t = Number(row.t);
    if (!Number.isSafeInteger(t)) return null;
    if (t < cutoff) continue;
    if (t <= previousTime) return null;
    history.push(normalizedPoint(row as unknown as EnvironmentHistoryRow, t));
    previousTime = t;
  }
  return history;
}

function parseEnvironmentState(previous: StateRow, cutoff: number): ParsedEnvironmentState | null {
  try {
    const payload = objectValue(JSON.parse(previous.payload));
    const storedDevices = objectValue(payload?.devices);
    if (!storedDevices) return null;

    const devices: Record<string, EnvironmentDeviceHistory> = {};
    let firstDeviceId = "";
    for (const [deviceId, value] of Object.entries(storedDevices)) {
      if (!DEVICE_ID_PATTERN.test(deviceId)) return null;
      const history = normalizedStoredHistory(value, cutoff);
      if (!history) return null;
      devices[deviceId] = { deviceId, bucketMinutes: 5, history };
      if (!firstDeviceId || deviceId < firstDeviceId) firstDeviceId = deviceId;
    }
    if (!firstDeviceId) return null;

    const selected = String(payload?.deviceId ?? "");
    return {
      selectedDeviceId: DEVICE_ID_PATTERN.test(selected) && devices[selected] ? selected : "",
      firstDeviceId,
      devices,
    };
  } catch {
    return null;
  }
}

function normalizeReturnedRows(
  rows: readonly EnvironmentHistoryRow[],
  cutoff: number,
): EnvironmentHistoryRow[] {
  const normalized: EnvironmentHistoryRow[] = [];
  let ordered = true;
  let previousTime = Number.NEGATIVE_INFINITY;
  for (const row of rows) {
    const t = Number(row.t);
    if (!Number.isSafeInteger(t) || t < cutoff) continue;
    if (t < previousTime) ordered = false;
    normalized.push(normalizedPoint(row, t));
    previousTime = t;
  }
  if (!ordered) normalized.sort((left, right) => left.t - right.t);

  let write = 0;
  for (const point of normalized) {
    if (write > 0 && normalized[write - 1]!.t === point.t) {
      normalized[write - 1] = point;
    } else {
      normalized[write] = point;
      write += 1;
    }
  }
  normalized.length = write;
  return normalized;
}

function mergeSortedHistory(
  existing: readonly EnvironmentHistoryRow[],
  incoming: readonly EnvironmentHistoryRow[],
): EnvironmentHistoryRow[] {
  const merged: EnvironmentHistoryRow[] = [];
  let existingIndex = 0;
  let incomingIndex = 0;
  while (existingIndex < existing.length || incomingIndex < incoming.length) {
    const current = existing[existingIndex];
    const replacement = incoming[incomingIndex];
    if (!replacement || (current && current.t < replacement.t)) {
      merged.push(current!);
      existingIndex += 1;
    } else if (!current || replacement.t < current.t) {
      merged.push(replacement);
      incomingIndex += 1;
    } else {
      merged.push(replacement);
      existingIndex += 1;
      incomingIndex += 1;
    }
  }
  return merged;
}

function updateCachedEnvironmentRow(
  previous: StateRow,
  serialized: string,
  hash: string,
  now: number,
): void {
  previous.version += 1;
  previous.payload = serialized;
  previous.observed_at = now;
  previous.fetched_at = now;
  previous.last_success_at = now;
  previous.status = "ok";
  previous.error = null;
  previous.content_hash = hash;
}

async function mergeDeviceState(
  env: Env,
  previous: StateRow,
  deviceId: string,
  returnedRows: readonly EnvironmentHistoryRow[],
  now: number,
  cutoff: number,
): Promise<FastMergeResult> {
  const parsed = parseEnvironmentState(previous, cutoff);
  if (!parsed) return "not_applicable";

  const incoming = normalizeReturnedRows(returnedRows, cutoff);
  const existing = parsed.devices[deviceId]?.history ?? [];
  const history = mergeSortedHistory(existing, incoming);
  parsed.devices[deviceId] = { deviceId, bucketMinutes: 5, history };
  if (!parsed.firstDeviceId || deviceId < parsed.firstDeviceId) parsed.firstDeviceId = deviceId;

  const preferred = env.HOMEPANEL_PRIMARY_DEVICE_ID?.trim() ?? "";
  const selectedId = parsed.devices[preferred]
    ? preferred
    : parsed.devices[parsed.selectedDeviceId]
      ? parsed.selectedDeviceId
      : parsed.devices[deviceId]
        ? deviceId
        : parsed.firstDeviceId;
  const selected = parsed.devices[selectedId] ?? { deviceId: selectedId, bucketMinutes: 5, history: [] };
  const payload = {
    deviceId: selected.deviceId,
    bucketMinutes: selected.bucketMinutes,
    history: selected.history,
    devices: parsed.devices,
  };
  const serialized = JSON.stringify(payload);
  const hash = await sha256Hex(serialized);
  if (previous.content_hash === hash) return "updated";

  const updated = await env.DB.prepare(
    `UPDATE current_state
        SET version=version+1, payload=?1, observed_at=?2, fetched_at=?2,
            last_success_at=?2, status='ok', error=NULL, content_hash=?3
      WHERE source='environment' AND version=?4`,
  ).bind(serialized, now, hash, previous.version).run();
  if (Number(updated.meta.changes ?? 0) !== 1) return "conflict";
  updateCachedEnvironmentRow(previous, serialized, hash, now);
  return "updated";
}

export async function mergeEnvironmentRows(
  env: Env,
  fallbackDeviceId: string,
  returnedRows: EnvironmentHistoryRow[],
  now: number,
): Promise<void> {
  const cutoff = now - ENVIRONMENT_HISTORY_MS;
  let previous = await readEnvironmentState(env);
  let fastResult: FastMergeResult = "not_applicable";
  for (let attempt = 0; previous && attempt < FAST_MERGE_ATTEMPTS; attempt += 1) {
    fastResult = await mergeDeviceState(
      env,
      previous,
      fallbackDeviceId,
      returnedRows,
      now,
      cutoff,
    );
    if (fastResult === "updated") return;
    if (fastResult === "not_applicable") break;
    previous = await readEnvironmentState(env, true);
  }

  const stored = await env.DB.prepare(
    `SELECT device_id,bucket_at AS t,
       CASE WHEN co2_count>0 THEN co2_sum/co2_count ELSE NULL END AS co2,
       CASE WHEN temperature_count>0 THEN temperature_sum/temperature_count ELSE NULL END AS temperature,
       CASE WHEN humidity_count>0 THEN humidity_sum/humidity_count ELSE NULL END AS humidity
       FROM environment_buckets
      WHERE bucket_at>=?1
      ORDER BY bucket_at,device_id`,
  ).bind(cutoff).all<StoredEnvironmentRow>();

  const durableRows = stored.results ?? [];
  const useDurableRows = durableRows.length > 0;
  const rows: readonly EnvironmentHistoryRow[] = useDurableRows ? durableRows : returnedRows;
  const devices: Record<string, EnvironmentDeviceHistory> = {};
  let unorderedDevices: Set<string> | null = null;
  let firstDeviceId = "";
  for (const row of rows) {
    const deviceId = useDurableRows
      ? String((row as StoredEnvironmentRow).device_id ?? "")
      : fallbackDeviceId;
    const t = Number(row.t);
    if (!DEVICE_ID_PATTERN.test(deviceId) || !Number.isSafeInteger(t) || t < cutoff) continue;
    const point = normalizedPoint(row, t);
    let device = devices[deviceId];
    if (!device) {
      device = { deviceId, bucketMinutes: 5, history: [] };
      devices[deviceId] = device;
      if (!firstDeviceId || deviceId < firstDeviceId) firstDeviceId = deviceId;
    } else if (device.history.length && device.history[device.history.length - 1]!.t > t) {
      (unorderedDevices ??= new Set<string>()).add(deviceId);
    }
    device.history.push(point);
  }
  if (unorderedDevices) {
    for (const deviceId of unorderedDevices) {
      devices[deviceId]!.history.sort((left, right) => left.t - right.t);
    }
  }

  const latestPrevious = !previous || fastResult === "conflict"
    ? await readEnvironmentState(env, true)
    : previous;
  const previousDeviceId = previousSelectedDevice(latestPrevious);
  const preferred = env.HOMEPANEL_PRIMARY_DEVICE_ID?.trim() ?? "";
  const selectedId = devices[preferred]
    ? preferred
    : devices[previousDeviceId]
      ? previousDeviceId
      : devices[fallbackDeviceId]
        ? fallbackDeviceId
        : firstDeviceId || fallbackDeviceId;
  const selected = devices[selectedId] ?? { deviceId: selectedId, bucketMinutes: 5, history: [] };
  await updateState(env, {
    source: "environment",
    observedAt: now,
    payload: {
      deviceId: selected.deviceId,
      bucketMinutes: selected.bucketMinutes,
      history: selected.history,
      devices,
    },
  }, undefined, latestPrevious);
  invalidateEnvironmentStateCache(env.DB);
}
