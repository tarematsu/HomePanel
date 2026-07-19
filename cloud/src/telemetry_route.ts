import { authorizedDevice, bearerToken, DEVICE_ID_PATTERN } from "./auth";
import { json } from "./http";
import { unauthorized } from "./response";
import type { Env } from "./sources";
import { markStateChanged } from "./state_generation";
import {
  markTelemetrySamplesAppliedStatement,
  telemetryBucketAt,
  telemetrySampleReadbackStatement,
  telemetrySampleStatement,
  telemetrySequenceBucketStatement,
  type EnvironmentHistoryRow,
  type TelemetrySample,
  type TelemetrySampleReceipt,
  type TelemetryStoredSampleReceipt,
} from "./telemetry_bucket";
import {
  telemetryHeartbeatReturningStatement,
  type TelemetryHeartbeatReceipt,
} from "./telemetry_heartbeat";
import { mergeEnvironmentRows } from "./telemetry_history";

interface TelemetryInput {
  deviceId?: string;
  appVersion?: string;
  stationheadOk?: boolean;
  outboxCount?: number;
  samples?: TelemetrySample[];
}

interface BucketBatchLayout {
  aggregateIndex: number;
  markIndex: number;
}

const MAX_TELEMETRY_SAMPLES = 1440;
const TELEMETRY_SAMPLES_PER_BATCH = 99;
const TELEMETRY_SEQUENCES_PER_STATEMENT = 90;
const TELEMETRY_STATEMENTS_PER_BATCH = 90;

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

function storedNumber(value: number | null): number | null {
  return value === null ? null : Number(value);
}

function storedSampleMatches(sample: TelemetrySample, row: TelemetryStoredSampleReceipt): boolean {
  return Number(row.observed_at) === sample.observedAt
    && storedNumber(row.co2) === (sample.co2 ?? null)
    && storedNumber(row.temperature) === (sample.temperature ?? null)
    && storedNumber(row.humidity) === (sample.humidity ?? null)
    && storedNumber(row.temperature_corrected) === (sample.temperatureCorrected ?? null)
    && storedNumber(row.humidity_corrected) === (sample.humidityCorrected ?? null);
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
  const pendingSequences = new Set<number>();
  const appVersion = String(input.appVersion ?? "").slice(0, 100) || null;
  const rawOutbox = Number(input.outboxCount);
  const outboxCount = Number.isFinite(rawOutbox) ? Math.max(0, Math.trunc(rawOutbox)) : 0;
  const stationheadOk = input.stationheadOk ? 1 : 0;
  const returnedRows = new Map<number, EnvironmentHistoryRow>();
  let accepted = 0;

  const acknowledge = (sequence: number, bucketApplied: number): void => {
    acknowledged.add(sequence);
    if (bucketApplied === 0) pendingSequences.add(sequence);
  };

  for (let offset = 0; offset < uniqueSamples.length; offset += TELEMETRY_SAMPLES_PER_BATCH) {
    const chunk = uniqueSamples.slice(offset, offset + TELEMETRY_SAMPLES_PER_BATCH);
    const receiptResults = await env.DB.batch(chunk.map(sample => telemetrySampleStatement(env, deviceId, sample)));
    const returned = new Set<number>();
    for (const result of receiptResults) {
      const rows = (result.results ?? []) as TelemetrySampleReceipt[];
      for (const row of rows) {
        const sequence = Number(row.sequence);
        returned.add(sequence);
        acknowledge(sequence, Number(row.bucket_applied));
      }
    }

    const missing = chunk.filter(sample => !returned.has(sample.sequence));
    if (missing.length) {
      const requestedBySequence = new Map(missing.map(sample => [sample.sequence, sample]));
      const readback = await telemetrySampleReadbackStatement(
        env,
        deviceId,
        missing.map(sample => sample.sequence),
      ).all<TelemetryStoredSampleReceipt>();
      for (const row of readback.results ?? []) {
        const sequence = Number(row.sequence);
        const sample = requestedBySequence.get(sequence);
        if (!sample || !storedSampleMatches(sample, row)) continue;
        acknowledge(sequence, Number(row.bucket_applied));
      }
    }
  }

  const pendingSamples = uniqueSamples.filter(sample => pendingSequences.has(sample.sequence));
  const sequencesByBucket = new Map<number, number[]>();
  for (const sample of pendingSamples) {
    const bucketAt = telemetryBucketAt(sample.observedAt);
    const sequences = sequencesByBucket.get(bucketAt);
    if (sequences) sequences.push(sample.sequence);
    else sequencesByBucket.set(bucketAt, [sample.sequence]);
  }

  let statements: D1PreparedStatement[] = [];
  let layouts: BucketBatchLayout[] = [];
  const flushBuckets = async (): Promise<void> => {
    if (!statements.length) return;
    const results = await env.DB.batch(statements);
    for (const layout of layouts) {
      const rows = (results[layout.aggregateIndex]?.results ?? []) as EnvironmentHistoryRow[];
      for (const row of rows) returnedRows.set(Number(row.t), row);
      accepted += Number(results[layout.markIndex]?.meta.changes ?? 0);
    }
    statements = [];
    layouts = [];
  };

  const orderedBuckets = [...sequencesByBucket.entries()].sort((left, right) => left[0] - right[0]);
  for (const [bucketAt, sequences] of orderedBuckets) {
    for (let offset = 0; offset < sequences.length; offset += TELEMETRY_SEQUENCES_PER_STATEMENT) {
      const chunk = sequences.slice(offset, offset + TELEMETRY_SEQUENCES_PER_STATEMENT);
      if (statements.length && statements.length + 2 > TELEMETRY_STATEMENTS_PER_BATCH) {
        await flushBuckets();
      }
      const aggregateIndex = statements.length;
      statements.push(telemetrySequenceBucketStatement(env, deviceId, bucketAt, chunk));
      const markIndex = statements.length;
      statements.push(markTelemetrySamplesAppliedStatement(env, deviceId, chunk));
      layouts.push({ aggregateIndex, markIndex });
    }
  }
  await flushBuckets();

  const acknowledgedSequences = [...acknowledged].sort((left, right) => left - right);
  const proposedHighest = acknowledgedSequences[acknowledgedSequences.length - 1] ?? 0;
  const heartbeat = await telemetryHeartbeatReturningStatement(
    env,
    deviceId,
    now,
    appVersion,
    stationheadOk,
    outboxCount,
    proposedHighest,
  ).first<TelemetryHeartbeatReceipt>();
  let highestSequence = Math.max(proposedHighest, Number(heartbeat?.last_sequence ?? 0));
  if (!heartbeat) {
    const stored = await env.DB.prepare(
      "SELECT last_sequence FROM device_heartbeats WHERE device_id=?1",
    ).bind(deviceId).first<{ last_sequence: number }>();
    highestSequence = Math.max(highestSequence, Number(stored?.last_sequence ?? 0));
  }

  let rebuildEnvironment = accepted > 0;
  if (!rebuildEnvironment && acknowledgedSequences.length > 0) {
    const environmentState = await env.DB.prepare(
      "SELECT 1 AS present FROM current_state WHERE source='environment'",
    ).first<{ present: number }>();
    rebuildEnvironment = !environmentState;
  }
  if (rebuildEnvironment) {
    await mergeEnvironmentRows(env, deviceId, [...returnedRows.values()], now);
    markStateChanged(env);
  }

  const response: Record<string, unknown> = {
    accepted,
    acknowledgedSequences,
    nextSequence: Math.min(Number.MAX_SAFE_INTEGER, highestSequence + 1),
  };
  if (requestDuplicates) response.duplicates = requestDuplicates;
  return json(response);
}
