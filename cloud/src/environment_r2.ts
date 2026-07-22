import { markStateChanged } from "./state_generation";
import type { StateRow } from "./snapshot";
import type { Env } from "./sources";
import type { TelemetrySample } from "./telemetry_bucket";

const ENVIRONMENT_STATE_KEY = "environment/v2/latest.json";
const ENVIRONMENT_BUCKET_MS = 15 * 60_000;
const ENVIRONMENT_HISTORY_MS = 24 * 60 * 60_000;
const MAX_CAS_ATTEMPTS = 4;
const CACHE_TTL_MS = 30_000;
const HEX_DIGITS = "0123456789abcdef";
const UTF8_ENCODER = new TextEncoder();

interface StoredBucket {
  bucketAt: number;
  sampleCount: number;
  co2Sum: number;
  co2Count: number;
  temperatureSum: number;
  temperatureCount: number;
  humiditySum: number;
  humidityCount: number;
}

interface StoredDevice {
  deviceId: string;
  buckets: StoredBucket[];
}

interface EnvironmentDocument {
  schemaVersion: 2;
  selectedDeviceId: string;
  lastSequences: Record<string, number>;
  devices: Record<string, StoredDevice>;
  row: StateRow;
}

export interface EnvironmentMergeResult {
  accepted: number;
  acknowledgedSequences: number[];
  lastSequence: number;
}

interface CachedEnvironmentRow {
  expiresAt: number;
  row: StateRow | null;
}

const ENVIRONMENT_CACHE = new WeakMap<object, CachedEnvironmentRow>();

function cacheKey(env: Env): object | null {
  return env.DATA_BUCKET ? env.DATA_BUCKET as unknown as object : null;
}

export function invalidateR2EnvironmentCache(env: Env): void {
  const key = cacheKey(env);
  if (key) ENVIRONMENT_CACHE.delete(key);
}

function bucketKey(observedAt: number): number {
  return Math.floor(observedAt / ENVIRONMENT_BUCKET_MS) * ENVIRONMENT_BUCKET_MS;
}

function finiteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function nonNegativeInteger(value: unknown): value is number {
  return Number.isSafeInteger(value) && Number(value) >= 0;
}

function validBucket(value: unknown): value is StoredBucket {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const row = value as Record<string, unknown>;
  return Number.isSafeInteger(row.bucketAt)
    && nonNegativeInteger(row.sampleCount)
    && finiteNumber(row.co2Sum)
    && nonNegativeInteger(row.co2Count)
    && finiteNumber(row.temperatureSum)
    && nonNegativeInteger(row.temperatureCount)
    && finiteNumber(row.humiditySum)
    && nonNegativeInteger(row.humidityCount);
}

function validStateRow(value: unknown): value is StateRow {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const row = value as Record<string, unknown>;
  return row.source === "environment"
    && finiteNumber(row.version)
    && typeof row.payload === "string"
    && (row.observed_at === null || finiteNumber(row.observed_at))
    && finiteNumber(row.fetched_at)
    && (row.last_success_at === null || finiteNumber(row.last_success_at))
    && ["ok", "stale", "error"].includes(String(row.status))
    && (row.error === null || typeof row.error === "string")
    && (row.content_hash === null || typeof row.content_hash === "string");
}

function parseDocument(value: unknown): EnvironmentDocument | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const root = value as Record<string, unknown>;
  if (root.schemaVersion !== 2 || typeof root.selectedDeviceId !== "string" || !validStateRow(root.row)) {
    return null;
  }
  if (!root.lastSequences || typeof root.lastSequences !== "object" || Array.isArray(root.lastSequences)) {
    return null;
  }
  if (!root.devices || typeof root.devices !== "object" || Array.isArray(root.devices)) return null;

  const lastSequences: Record<string, number> = {};
  for (const [deviceId, raw] of Object.entries(root.lastSequences as Record<string, unknown>)) {
    const sequence = Number(raw);
    if (!Number.isSafeInteger(sequence) || sequence < 0) return null;
    lastSequences[deviceId] = sequence;
  }

  const devices: Record<string, StoredDevice> = {};
  for (const [deviceId, raw] of Object.entries(root.devices as Record<string, unknown>)) {
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) return null;
    const device = raw as Record<string, unknown>;
    if (device.deviceId !== deviceId || !Array.isArray(device.buckets) || !device.buckets.every(validBucket)) {
      return null;
    }
    devices[deviceId] = {
      deviceId,
      buckets: device.buckets.map(bucket => ({ ...bucket } as StoredBucket)),
    };
  }

  return {
    schemaVersion: 2,
    selectedDeviceId: root.selectedDeviceId,
    lastSequences,
    devices,
    row: { ...root.row },
  };
}

async function sha256Hex(value: string): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", UTF8_ENCODER.encode(value)));
  let output = "";
  for (const byte of digest) {
    output += HEX_DIGITS.charAt(byte >>> 4) + HEX_DIGITS.charAt(byte & 0x0f);
  }
  return output;
}

function addSample(bucket: StoredBucket, sample: TelemetrySample): void {
  bucket.sampleCount += 1;
  if (sample.co2 !== undefined) {
    bucket.co2Sum += sample.co2;
    bucket.co2Count += 1;
  }
  const temperature = sample.temperatureCorrected ?? sample.temperature;
  if (temperature !== undefined) {
    bucket.temperatureSum += temperature;
    bucket.temperatureCount += 1;
  }
  const humidity = sample.humidityCorrected ?? sample.humidity;
  if (humidity !== undefined) {
    bucket.humiditySum += humidity;
    bucket.humidityCount += 1;
  }
}

function historyPoint(bucket: StoredBucket): Record<string, number | null> {
  return {
    t: bucket.bucketAt,
    co2: bucket.co2Count > 0 ? Math.round(bucket.co2Sum / bucket.co2Count) : null,
    temperature: bucket.temperatureCount > 0
      ? Number((bucket.temperatureSum / bucket.temperatureCount).toFixed(2))
      : null,
    humidity: bucket.humidityCount > 0
      ? Number((bucket.humiditySum / bucket.humidityCount).toFixed(2))
      : null,
  };
}

function copyDevices(previous: EnvironmentDocument | null): Record<string, StoredDevice> {
  const devices: Record<string, StoredDevice> = {};
  for (const [id, device] of Object.entries(previous?.devices ?? {})) {
    devices[id] = { deviceId: id, buckets: device.buckets.map(bucket => ({ ...bucket })) };
  }
  return devices;
}

function stateContentChanged(previous: StateRow | null, next: StateRow): boolean {
  return !previous
    || previous.version !== next.version
    || previous.status !== next.status
    || previous.error !== next.error
    || previous.content_hash !== next.content_hash;
}

async function nextDocument(
  previous: EnvironmentDocument | null,
  env: Env,
  deviceId: string,
  samples: readonly TelemetrySample[],
  now: number,
): Promise<EnvironmentDocument> {
  const devices = copyDevices(previous);
  const device = devices[deviceId] ?? { deviceId, buckets: [] };
  const byTime = new Map(device.buckets.map(bucket => [bucket.bucketAt, bucket]));
  for (const sample of samples) {
    const at = bucketKey(sample.observedAt);
    let bucket = byTime.get(at);
    if (!bucket) {
      bucket = {
        bucketAt: at,
        sampleCount: 0,
        co2Sum: 0,
        co2Count: 0,
        temperatureSum: 0,
        temperatureCount: 0,
        humiditySum: 0,
        humidityCount: 0,
      };
      byTime.set(at, bucket);
    }
    addSample(bucket, sample);
  }

  const cutoff = now - ENVIRONMENT_HISTORY_MS;
  device.buckets = [...byTime.values()]
    .filter(bucket => bucket.bucketAt >= cutoff)
    .sort((left, right) => left.bucketAt - right.bucketAt);
  devices[deviceId] = device;
  for (const stored of Object.values(devices)) {
    stored.buckets = stored.buckets
      .filter(bucket => bucket.bucketAt >= cutoff)
      .sort((left, right) => left.bucketAt - right.bucketAt);
  }

  const configuredPrimary = env.HOMEPANEL_PRIMARY_DEVICE_ID?.trim() ?? "";
  const previousSelected = previous?.selectedDeviceId ?? "";
  const selectedDeviceId = devices[configuredPrimary]
    ? configuredPrimary
    : devices[previousSelected]
      ? previousSelected
      : deviceId;
  const publicDevices = Object.fromEntries(Object.entries(devices).map(([id, stored]) => [
    id,
    {
      deviceId: id,
      bucketMinutes: ENVIRONMENT_BUCKET_MS / 60_000,
      history: stored.buckets.map(historyPoint),
    },
  ]));
  const selected = publicDevices[selectedDeviceId] ?? {
    deviceId: selectedDeviceId,
    bucketMinutes: ENVIRONMENT_BUCKET_MS / 60_000,
    history: [],
  };
  const payload = JSON.stringify({
    deviceId: selected.deviceId,
    bucketMinutes: selected.bucketMinutes,
    history: selected.history,
    devices: publicDevices,
  });
  const contentHash = await sha256Hex(payload);
  const previousRow = previous?.row ?? null;
  const version = previousRow
    ? previousRow.version + (previousRow.content_hash === contentHash ? 0 : 1)
    : 1;
  const sampleObservedAt = samples.reduce(
    (maximum, sample) => Math.max(maximum, sample.observedAt),
    0,
  );
  const observedAt = Math.max(Number(previousRow?.observed_at ?? 0), sampleObservedAt) || now;
  const lastSequences = { ...(previous?.lastSequences ?? {}) };
  const highest = samples.reduce(
    (maximum, sample) => Math.max(maximum, sample.sequence),
    lastSequences[deviceId] ?? 0,
  );
  lastSequences[deviceId] = highest;

  return {
    schemaVersion: 2,
    selectedDeviceId,
    lastSequences,
    devices,
    row: {
      source: "environment",
      version,
      payload,
      observed_at: observedAt,
      fetched_at: now,
      last_success_at: now,
      status: "ok",
      error: null,
      content_hash: contentHash,
    },
  };
}

async function readDocument(
  bucket: R2Bucket,
): Promise<{ document: EnvironmentDocument | null; etag: string | null }> {
  const object = await bucket.get(ENVIRONMENT_STATE_KEY);
  if (!object) return { document: null, etag: null };
  const parsed = parseDocument(await object.json<unknown>());
  if (!parsed) throw new Error("R2 environment state is invalid");
  return { document: parsed, etag: object.etag };
}

export async function readR2EnvironmentState(env: Env): Promise<StateRow | null> {
  const bucket = env.DATA_BUCKET;
  if (!bucket) return null;
  const key = bucket as unknown as object;
  const cached = ENVIRONMENT_CACHE.get(key);
  const now = Date.now();
  if (cached && cached.expiresAt > now) return cached.row;
  try {
    const { document } = await readDocument(bucket);
    const row = document?.row ?? null;
    ENVIRONMENT_CACHE.set(key, { expiresAt: now + CACHE_TTL_MS, row });
    return row;
  } catch (error) {
    console.error("R2 environment read failed", error instanceof Error ? error.message : String(error));
    return null;
  }
}

export async function mergeR2EnvironmentTelemetry(
  env: Env,
  deviceId: string,
  samples: readonly TelemetrySample[],
  now: number,
): Promise<EnvironmentMergeResult> {
  const bucket = env.DATA_BUCKET;
  if (!bucket) throw new Error("DATA_BUCKET is not configured");

  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const { document, etag } = await readDocument(bucket);
    const previousSequence = document?.lastSequences[deviceId] ?? 0;
    const ordered = [...samples]
      .filter(sample => sample.sequence > previousSequence)
      .sort((left, right) => left.sequence - right.sequence);
    const unique: TelemetrySample[] = [];
    for (const sample of ordered) {
      if (unique.at(-1)?.sequence !== sample.sequence) unique.push(sample);
    }
    if (!unique.length) {
      return {
        accepted: 0,
        acknowledgedSequences: samples
          .filter(sample => sample.sequence <= previousSequence)
          .map(sample => sample.sequence),
        lastSequence: previousSequence,
      };
    }

    const next = await nextDocument(document, env, deviceId, unique, now);
    const stored = await bucket.put(ENVIRONMENT_STATE_KEY, JSON.stringify(next), {
      onlyIf: etag ? { etagMatches: etag } : { etagDoesNotMatch: "*" },
      httpMetadata: { contentType: "application/json" },
      customMetadata: {
        schemaVersion: "2",
        updatedAt: String(now),
      },
    });
    if (!stored) continue;

    ENVIRONMENT_CACHE.set(bucket as unknown as object, {
      expiresAt: now + CACHE_TTL_MS,
      row: next.row,
    });
    if (stateContentChanged(document?.row ?? null, next.row)) markStateChanged(env);
    const lastSequence = next.lastSequences[deviceId] ?? previousSequence;
    return {
      accepted: unique.length,
      acknowledgedSequences: samples
        .filter(sample => sample.sequence <= lastSequence)
        .map(sample => sample.sequence),
      lastSequence,
    };
  }
  throw new Error("R2 environment update conflicted repeatedly");
}
