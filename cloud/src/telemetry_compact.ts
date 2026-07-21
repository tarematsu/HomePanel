import { authorizedDevice, bearerToken, DEVICE_ID_PATTERN } from "./auth";
import { archiveLatestTelemetry, mergeR2EnvironmentTelemetry } from "./environment_r2";
import { json } from "./http";
import { unauthorized } from "./response";
import type { Env } from "./sources";
import type { TelemetrySample } from "./telemetry_bucket";
import {
  telemetryHeartbeatReturningStatement,
  type TelemetryHeartbeatReceipt,
} from "./telemetry_heartbeat";

interface TelemetryInput {
  deviceId?: string;
  appVersion?: string;
  stationheadOk?: boolean;
  outboxCount?: number;
  samples?: unknown[];
}

const MAX_COMPACT_TELEMETRY_SAMPLES = 60;

function finiteOptional(value: unknown): number | null | undefined {
  if (value === undefined || value === null) return null;
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

function boundedOptional(
  value: unknown,
  minimum: number,
  maximum: number,
): number | null | undefined {
  const result = finiteOptional(value);
  if (result === null || result === undefined) return result;
  return result >= minimum && result <= maximum ? result : undefined;
}

function normalizeSample(value: unknown, now: number): TelemetrySample | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const input = value as Record<string, unknown>;
  if (typeof input.sequence !== "number" || !Number.isSafeInteger(input.sequence) ||
      input.sequence <= 0 || input.sequence >= Number.MAX_SAFE_INTEGER) {
    return null;
  }
  if (typeof input.observedAt !== "number" || !Number.isSafeInteger(input.observedAt) ||
      input.observedAt < 946_684_800_000 || input.observedAt > now + 86_400_000) {
    return null;
  }

  const co2 = boundedOptional(input.co2, 250, 10_000);
  const temperature = boundedOptional(input.temperature, -40, 85);
  const humidity = boundedOptional(input.humidity, 0, 100);
  const temperatureCorrected = boundedOptional(input.temperatureCorrected, -80, 120);
  const humidityCorrected = boundedOptional(input.humidityCorrected, 0, 100);
  if (co2 === undefined || temperature === undefined || humidity === undefined ||
      temperatureCorrected === undefined || humidityCorrected === undefined) {
    return null;
  }

  const normalized: TelemetrySample = {
    sequence: input.sequence,
    observedAt: input.observedAt,
  };
  if (co2 !== null) normalized.co2 = co2;
  if (temperature !== null) normalized.temperature = temperature;
  if (humidity !== null) normalized.humidity = humidity;
  if (temperatureCorrected !== null) normalized.temperatureCorrected = temperatureCorrected;
  if (humidityCorrected !== null) normalized.humidityCorrected = humidityCorrected;
  return normalized;
}

export async function receiveCompactTelemetry(
  request: Request,
  env: Env,
  ctx: ExecutionContext,
): Promise<Response> {
  if (!bearerToken(request)) return unauthorized();
  if (!env.DATA_BUCKET) return json({ error: "telemetry storage unavailable" }, { status: 503 });

  let input: TelemetryInput;
  try {
    input = await request.json() as TelemetryInput;
  } catch {
    return json({ error: "invalid json" }, { status: 400 });
  }

  const deviceId = String(input.deviceId ?? "").trim();
  if (!DEVICE_ID_PATTERN.test(deviceId) || !Array.isArray(input.samples) ||
      input.samples.length > MAX_COMPACT_TELEMETRY_SAMPLES) {
    return json({ error: "invalid telemetry" }, { status: 400 });
  }
  if (!authorizedDevice(request, env, deviceId)) return unauthorized();

  const now = Date.now();
  const unique = new Map<number, TelemetrySample>();
  let requestDuplicates = 0;
  for (const raw of input.samples) {
    const sample = normalizeSample(raw, now);
    if (!sample) return json({ error: "invalid telemetry sample" }, { status: 400 });
    if (unique.has(sample.sequence)) {
      requestDuplicates += 1;
      continue;
    }
    unique.set(sample.sequence, sample);
  }
  const samples = [...unique.values()].sort((left, right) => left.sequence - right.sequence);
  const merged = await mergeR2EnvironmentTelemetry(env, deviceId, samples, now);

  const appVersion = String(input.appVersion ?? "").slice(0, 100) || null;
  const rawOutbox = Number(input.outboxCount);
  const outboxCount = Number.isFinite(rawOutbox) ? Math.max(0, Math.trunc(rawOutbox)) : 0;
  const stationheadOk = input.stationheadOk ? 1 : 0;
  const heartbeat = await telemetryHeartbeatReturningStatement(
    env,
    deviceId,
    now,
    appVersion,
    stationheadOk,
    outboxCount,
    merged.lastSequence,
  ).first<TelemetryHeartbeatReceipt>();
  const lastSequence = Math.max(merged.lastSequence, Number(heartbeat?.last_sequence ?? 0));

  ctx.waitUntil(archiveLatestTelemetry(env, deviceId, {
    deviceId,
    appVersion,
    stationheadOk: Boolean(input.stationheadOk),
    outboxCount,
    samples,
  }, now).catch(error => {
    console.error("telemetry R2 archive failed", error instanceof Error ? error.message : String(error));
  }));

  const response: Record<string, unknown> = {
    accepted: merged.accepted,
    acknowledgedSequences: merged.acknowledgedSequences,
    nextSequence: Math.min(Number.MAX_SAFE_INTEGER, lastSequence + 1),
  };
  if (requestDuplicates) response.duplicates = requestDuplicates;
  return json(response);
}
