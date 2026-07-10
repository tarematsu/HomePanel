import { authorizedDevice, bearerToken, DEVICE_ID_PATTERN } from "./auth";
import { json } from "./http";
import { unauthorized } from "./response";
import type { Env } from "./sources";
import {
  aggregateTelemetrySamples,
  telemetryBucketStatement,
  telemetryHeartbeatStatement,
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
  if (!Number.isSafeInteger(sequence) || sequence <= 0) return null;
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
  let duplicates = 0;
  for (const sample of samples) {
    if (sample.sequence <= lastSequence) continue;
    if (unique.has(sample.sequence)) {
      duplicates += 1;
      continue;
    }
    unique.set(sample.sequence, sample);
  }
  const accepted = [...unique.values()].sort((left, right) => left.sequence - right.sequence);
  const appVersion = String(input.appVersion ?? "").slice(0, 100) || null;
  const rawOutbox = Number(input.outboxCount);
  const outboxCount = Number.isFinite(rawOutbox) ? Math.max(0, Math.trunc(rawOutbox)) : 0;
  const stationheadOk = input.stationheadOk ? 1 : 0;
  const returnedRows = new Map<number, EnvironmentHistoryRow>();

  if (accepted.length) {
    for (let offset = 0; offset < accepted.length; offset += TELEMETRY_SAMPLES_PER_BATCH) {
      const chunk = accepted.slice(offset, offset + TELEMETRY_SAMPLES_PER_BATCH);
      const buckets = aggregateTelemetrySamples(chunk);
      const statements = buckets.map(bucket => telemetryBucketStatement(env, deviceId, bucket));
      if (offset + chunk.length === accepted.length) {
        statements.push(telemetryHeartbeatStatement(
          env,
          deviceId,
          now,
          appVersion,
          stationheadOk,
          outboxCount,
          chunk.at(-1)!.sequence,
        ));
      }
      const results = await env.DB.batch(statements);
      for (let index = 0; index < buckets.length; index += 1) {
        const rows = (results[index]?.results ?? []) as EnvironmentHistoryRow[];
        for (const row of rows) returnedRows.set(Number(row.t), row);
      }
    }
    await mergeEnvironmentRows(env, deviceId, [...returnedRows.values()], now);
  } else {
    const heartbeatDue = !previous || now - Number(previous.last_seen_at ?? 0) >= HEARTBEAT_INTERVAL_MS;
    const heartbeatChanged = !previous
      || previous.app_version !== appVersion
      || Number(previous.stationhead_ok) !== stationheadOk
      || Number(previous.outbox_count) !== outboxCount;
    if (heartbeatDue || heartbeatChanged) {
      await telemetryHeartbeatStatement(
        env,
        deviceId,
        now,
        appVersion,
        stationheadOk,
        outboxCount,
        lastSequence,
      ).run();
    }
  }

  return json(duplicates
    ? { accepted: accepted.length, duplicates }
    : { accepted: accepted.length });
}
