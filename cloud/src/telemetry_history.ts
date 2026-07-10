import { DEVICE_ID_PATTERN } from "./auth";
import { readState, updateState, type StateRow } from "./snapshot";
import type { Env } from "./sources";
import type { EnvironmentHistoryRow } from "./telemetry_bucket";

interface EnvironmentDeviceHistory {
  deviceId: string;
  bucketMinutes: number;
  history: EnvironmentHistoryRow[];
}

interface PreviousEnvironmentState {
  deviceId: string;
  devices: Record<string, EnvironmentDeviceHistory>;
}

const ENVIRONMENT_HISTORY_MS = 24 * 60 * 60 * 1000;

function environmentPoint(value: unknown): EnvironmentHistoryRow | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const input = value as Record<string, unknown>;
  const t = Number(input.t);
  if (!Number.isSafeInteger(t)) return null;
  const nullable = (field: string): number | null => {
    if (input[field] === null || input[field] === undefined) return null;
    const number = Number(input[field]);
    return Number.isFinite(number) ? number : null;
  };
  return {
    t,
    co2: nullable("co2"),
    temperature: nullable("temperature"),
    humidity: nullable("humidity"),
  };
}

function previousDevices(
  previous: StateRow | null,
  cutoff: number,
): PreviousEnvironmentState {
  if (!previous?.payload) return { deviceId: "", devices: {} };
  let payload: Record<string, unknown>;
  try {
    const parsed = JSON.parse(previous.payload) as unknown;
    payload = parsed && typeof parsed === "object" && !Array.isArray(parsed)
      ? parsed as Record<string, unknown>
      : {};
  } catch {
    return { deviceId: "", devices: {} };
  }

  const devices: Record<string, EnvironmentDeviceHistory> = {};
  const add = (deviceId: string, value: unknown): void => {
    if (!DEVICE_ID_PATTERN.test(deviceId) || !value || typeof value !== "object" || Array.isArray(value)) return;
    const history = Array.isArray((value as Record<string, unknown>).history)
      ? ((value as Record<string, unknown>).history as unknown[])
        .map(environmentPoint)
        .filter((point): point is EnvironmentHistoryRow => point !== null && point.t >= cutoff)
      : [];
    if (history.length) devices[deviceId] = { deviceId, bucketMinutes: 5, history };
  };

  const rawDevices = payload.devices;
  if (rawDevices && typeof rawDevices === "object" && !Array.isArray(rawDevices)) {
    for (const [deviceId, value] of Object.entries(rawDevices as Record<string, unknown>)) add(deviceId, value);
  }
  const rootDeviceId = String(payload.deviceId ?? "");
  if (!devices[rootDeviceId] && Array.isArray(payload.history)) add(rootDeviceId, payload);
  return { deviceId: rootDeviceId, devices };
}

export async function mergeEnvironmentRows(
  env: Env,
  fallbackDeviceId: string,
  returnedRows: EnvironmentHistoryRow[],
  now: number,
): Promise<void> {
  const cutoff = now - ENVIRONMENT_HISTORY_MS;




  const stored = await env.DB.prepare(
    `SELECT bucket_at AS t,
       CASE WHEN co2_count>0 THEN co2_sum/co2_count ELSE NULL END AS co2,
       CASE WHEN temperature_count>0 THEN temperature_sum/temperature_count ELSE NULL END AS temperature,
       CASE WHEN humidity_count>0 THEN humidity_sum/humidity_count ELSE NULL END AS humidity
       FROM environment_buckets
      WHERE device_id=?1 AND bucket_at>=?2
      ORDER BY bucket_at`,
  ).bind(fallbackDeviceId, cutoff).all<EnvironmentHistoryRow>();
  const rows = (stored.results?.length ? stored.results : returnedRows)
    .filter(row => Number(row.t) >= cutoff);
  if (!rows.length) return;

  const previous = await readState(env, "environment");
  const previousState = previousDevices(previous, cutoff);
  const devices = previousState.devices;
  const target = devices[fallbackDeviceId] ?? {
    deviceId: fallbackDeviceId,
    bucketMinutes: 5,
    history: [],
  };
  const points = new Map(target.history.map(point => [point.t, point]));
  for (const row of rows) {
    points.set(Number(row.t), {
      t: Number(row.t),
      co2: row.co2 === null ? null : Math.round(Number(row.co2)),
      temperature: row.temperature === null ? null : Number(Number(row.temperature).toFixed(2)),
      humidity: row.humidity === null ? null : Number(Number(row.humidity).toFixed(2)),
    });
  }
  target.history = [...points.values()]
    .filter(point => point.t >= cutoff)
    .sort((left, right) => left.t - right.t);
  devices[fallbackDeviceId] = target;

  for (const [deviceId, device] of Object.entries(devices)) {
    if (!device.history.length) delete devices[deviceId];
  }

  const preferred = env.HOMEPANEL_PRIMARY_DEVICE_ID?.trim() ?? "";
  const selectedId = devices[preferred]
    ? preferred
    : devices[previousState.deviceId]
      ? previousState.deviceId
      : devices[fallbackDeviceId]
        ? fallbackDeviceId
        : Object.keys(devices).sort()[0] ?? fallbackDeviceId;
  const selected = devices[selectedId] ?? { deviceId: selectedId, bucketMinutes: 5, history: [] };
  await updateState(env, {
    source: "environment",
    observedAt: now,
    payload: { ...selected, devices },
  }, undefined, previous);
}
