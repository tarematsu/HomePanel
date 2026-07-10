import { authorizedDevice, bearerToken, DEVICE_ID_PATTERN } from "./auth";
import { json } from "./http";
import { unauthorized } from "./response";
import type { Env } from "./sources";
import {
  markTelemetryBucketAppliedStatement,
  pendingTelemetryBucketStatement,
  telemetryBucketAt,
  telemetryHeartbeatStatement,
  telemetrySampleStatement,
  type EnvironmentHistoryRow,
  type TelemetrySample,
} from "./telemetry_bucket";
import { mergeEnvironmentRows } from "./telemetry_history";

interface TelemetryInput {
  deviceId?: string;
  appVersion?: string;
  stationheadOk?: boolean;
  outboxCount?: number;
  samples?: TelemetrySample[];
}

interface HeartbeatRow {
  last_seen_at: number;
  app_version: string | null;
  stationhead_ok: number;
  outbox_count: number;
  last_sequence: number;
}

const MAX_TELEMETRY_SAMPLES = 1440;
const TELEMETRY_SAMPLES_PER_BATCH = 99;
const TELEMETRY_BUCKETS_PER_BATCH = 49;
const HEARTBEAT_INTERVAL_MS = 15 * 60 * 1000;

function finiteOptional(value: unknown): number | null | undefined {
  if (value === undefined || value === null) return null;
  const result = Number(value);
  return Number.isFinite(result) ? result : undefined;
}

function normalizeSample(value: unknown, now: number): TelemetrySample | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const input = value as Record<string, unknown>;
  const sequence = Math.trunc(Number(input.sequence));
  const observedAt = Math.trunc(Number(input.observedAt));
  if (!Number.isSafeInteger(sequence) || sequence <= 0 || sequence >= Number.MAX_SAFE_INTEGER) return null;
  if (!Number.isSafeInteger(observedAt) || observedAt < 946_684_800_000 || observedAt > now + 86_400_000) return null;
  const fields = ["co2", "temperature", "humidity", "temperatureCorrected", "humidityCorrected"] as const;
  const normalized: TelemetrySample = { sequence, observedAt };
  for (const field of fields) {
    const number = finiteOptional(input[field]);
    if (number === undefined) return null;
    if (number !== null) normalized[field] = number;
  }
  return normalized;
}

export async function receiveTelemetryOptimized(request: Request, env: Env): Promise<Response> {
  if (!bearerToken(request)) return unauthorized();

  let input: TelemetryInput;
  try {
    input = await request.json() as TelemetryInput;
  } catch {
    return json({ error: "invalid json" }, { status: 400 });
  }
  const deviceId = String(input.deviceId ?? "").trim();
  if (!DEVICE_ID_PATTERN.test(deviceId) || !Array.isArray(input.samples) || input.samples.length > MAX_TELEMETRY_SAMPLES) {
    return json({ error: "invalid telemetry" }, { status: 400 });
  }
  if (!authorizedDevice(request, env, deviceId)) return unauthorized();

  const now = Date.now();
  const samples: TelemetrySample[] = [];
  for (const sample of input.samples) {
    const normalized = normalizeSample(sample, now);
    if (!normalized) return json({ error: "invalid telemetry sample" }, { status: 400 });
    samples.push(normalized);
  }
  const previous = await env.DB.prepare(
    `SELECT last_seen_at,app_version,stationhead_ok,outbox_count,last_sequence
       FROM device_heartbeats WHERE device_id=?1`,
  ).bind(deviceId).first<HeartbeatRow>();
  const lastSequence = Number(previous?.last_sequence ?? 0);
  const unique = new Map<number, TelemetrySample>();
  let requestDuplicates = 0;
  for (const sample of samples) {
    if (unique.has(sample.sequence)) {
      requestDuplicates += 1;
      continue;
    }
    unique.set(sample.sequence, sample);
  }
  const uniqueSamples = [...unique.values()].sort((left, right) => left.sequence - right.sequence);
  const acknowledged = new Set<number>();
  const appVersion = String(input.appVersion ?? "").slice(0, 100) || null;
  const rawOutbox = Number(input.outboxCount);
  const outboxCount = Number.isFinite(rawOutbox) ? Math.max(0, Math.trunc(rawOutbox)) : 0;
  const stationheadOk = input.stationheadOk ? 1 : 0;
  const returnedRows = new Map<number, EnvironmentHistoryRow>();
  let accepted = 0;

  for (let offset = 0; offset < uniqueSamples.length; offset += TELEMETRY_SAMPLES_PER_BATCH) {
    const chunk = uniqueSamples.slice(offset, offset + TELEMETRY_SAMPLES_PER_BATCH);
    const receiptResults = await env.DB.batch(chunk.map(sample => telemetrySampleStatement(env, deviceId, sample)));
    for (const result of receiptResults) {
      const rows = (result.results ?? []) as Array<{ sequence: number }>;
      for (const row of rows) acknowledged.add(Number(row.sequence));
    }
  }

  const acknowledgedSamples = uniqueSamples.filter(sample => acknowledged.has(sample.sequence));
  const affectedBuckets = [...new Set(acknowledgedSamples.map(sample => telemetryBucketAt(sample.observedAt)))].sort((left, right) => left - right);
  for (let offset = 0; offset < affectedBuckets.length; offset += TELEMETRY_BUCKETS_PER_BATCH) {
    const bucketChunk = affectedBuckets.slice(offset, offset + TELEMETRY_BUCKETS_PER_BATCH);
    const statements: D1PreparedStatement[] = [];
    for (const bucketAt of bucketChunk) {
      statements.push(pendingTelemetryBucketStatement(env, deviceId, bucketAt));
      statements.push(markTelemetryBucketAppliedStatement(env, deviceId, bucketAt));
    }
    const bucketResults = await env.DB.batch(statements);
    for (let index = 0; index < bucketResults.length; index += 2) {
      const rows = (bucketResults[index]?.results ?? []) as EnvironmentHistoryRow[];
      for (const row of rows) {
        accepted += Number(row.applied_count ?? 0);
        returnedRows.set(Number(row.t), row);
      }
    }
  }

  const acknowledgedSequences = [...acknowledged].sort((left, right) => left - right);
  const highestSequence = acknowledgedSequences.reduce((highest, sequence) => Math.max(highest, sequence), lastSequence);
  const heartbeatDue = !previous || now - Number(previous.last_seen_at ?? 0) >= HEARTBEAT_INTERVAL_MS;
  const heartbeatChanged = !previous
    || previous.app_version !== appVersion
    || Number(previous.stationhead_ok) !== stationheadOk
    || Number(previous.outbox_count) !== outboxCount;
  if (acknowledgedSequences.length || heartbeatDue || heartbeatChanged) {
    await telemetryHeartbeatStatement(
      env,
      deviceId,
      now,
      appVersion,
      stationheadOk,
      outboxCount,
      highestSequence,
    ).run();
  }

  if (acknowledgedSamples.length) {
    await mergeEnvironmentRows(env, deviceId, [...returnedRows.values()], now);
  }

  const response: Record<string, unknown> = {
    accepted,
    acknowledgedSequences,
    nextSequence: Math.min(Number.MAX_SAFE_INTEGER, highestSequence + 1),
  };
  if (requestDuplicates) response.duplicates = requestDuplicates;
  return json(response);
}
