import type { Env } from "./sources";

export interface TelemetrySample {
  sequence: number;
  observedAt: number;
  co2?: number;
  temperature?: number;
  humidity?: number;
  temperatureCorrected?: number;
  humidityCorrected?: number;
}

export interface TelemetrySampleReceipt {
  sequence: number;
  bucket_applied: number;
}

export interface EnvironmentHistoryRow {
  t: number;
  co2: number | null;
  temperature: number | null;
  humidity: number | null;
  applied_count?: number;
}

export interface EnvironmentBucket {
  bucketAt: number;
  sampleCount: number;
  co2Sum: number;
  co2Count: number;
  temperatureSum: number;
  temperatureCount: number;
  humiditySum: number;
  humidityCount: number;
}

export const ENVIRONMENT_BUCKET_MS = 5 * 60 * 1000;

export function telemetryBucketAt(observedAt: number): number {
  return Math.floor(observedAt / ENVIRONMENT_BUCKET_MS) * ENVIRONMENT_BUCKET_MS;
}

export function aggregateTelemetrySamples(samples: readonly TelemetrySample[]): EnvironmentBucket[] {
  const buckets = new Map<number, EnvironmentBucket>();
  for (const sample of samples) {
    const bucketAt = telemetryBucketAt(sample.observedAt);
    let bucket = buckets.get(bucketAt);
    if (!bucket) {
      bucket = {
        bucketAt,
        sampleCount: 0,
        co2Sum: 0,
        co2Count: 0,
        temperatureSum: 0,
        temperatureCount: 0,
        humiditySum: 0,
        humidityCount: 0,
      };
      buckets.set(bucketAt, bucket);
    }
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
  return [...buckets.values()].sort((left, right) => left.bucketAt - right.bucketAt);
}

export function telemetrySampleStatement(
  env: Env,
  deviceId: string,
  sample: TelemetrySample,
): D1PreparedStatement {
  return env.DB.prepare(
    `INSERT INTO environment_samples(
       device_id,sequence,observed_at,co2,temperature,humidity,
       temperature_corrected,humidity_corrected,bucket_applied
     ) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,0)
     ON CONFLICT(device_id,sequence) DO UPDATE SET
       observed_at=excluded.observed_at,
       co2=excluded.co2,
       temperature=excluded.temperature,
       humidity=excluded.humidity,
       temperature_corrected=excluded.temperature_corrected,
       humidity_corrected=excluded.humidity_corrected,
       bucket_applied=CASE
         WHEN excluded.observed_at>environment_samples.observed_at THEN 0
         ELSE environment_samples.bucket_applied
       END
     WHERE excluded.observed_at>environment_samples.observed_at
        OR (
          excluded.observed_at=environment_samples.observed_at
          AND excluded.co2 IS environment_samples.co2
          AND excluded.temperature IS environment_samples.temperature
          AND excluded.humidity IS environment_samples.humidity
          AND excluded.temperature_corrected IS environment_samples.temperature_corrected
          AND excluded.humidity_corrected IS environment_samples.humidity_corrected
        )
     RETURNING sequence,bucket_applied`,
  ).bind(
    deviceId,
    sample.sequence,
    sample.observedAt,
    sample.co2 ?? null,
    sample.temperature ?? null,
    sample.humidity ?? null,
    sample.temperatureCorrected ?? null,
    sample.humidityCorrected ?? null,
  );
}

export function telemetryBucketAggregateStatement(
  env: Env,
  deviceId: string,
  bucket: EnvironmentBucket,
): D1PreparedStatement {
  return env.DB.prepare(
    `INSERT INTO environment_buckets(
       device_id,bucket_at,sample_count,co2_sum,co2_count,
       temperature_sum,temperature_count,humidity_sum,humidity_count
     ) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)
     ON CONFLICT(device_id,bucket_at) DO UPDATE SET
       sample_count=environment_buckets.sample_count+excluded.sample_count,
       co2_sum=environment_buckets.co2_sum+excluded.co2_sum,
       co2_count=environment_buckets.co2_count+excluded.co2_count,
       temperature_sum=environment_buckets.temperature_sum+excluded.temperature_sum,
       temperature_count=environment_buckets.temperature_count+excluded.temperature_count,
       humidity_sum=environment_buckets.humidity_sum+excluded.humidity_sum,
       humidity_count=environment_buckets.humidity_count+excluded.humidity_count
     RETURNING bucket_at AS t,
       CASE WHEN co2_count>0 THEN co2_sum/co2_count ELSE NULL END AS co2,
       CASE WHEN temperature_count>0 THEN temperature_sum/temperature_count ELSE NULL END AS temperature,
       CASE WHEN humidity_count>0 THEN humidity_sum/humidity_count ELSE NULL END AS humidity`,
  ).bind(
    deviceId,
    bucket.bucketAt,
    bucket.sampleCount,
    bucket.co2Sum,
    bucket.co2Count,
    bucket.temperatureSum,
    bucket.temperatureCount,
    bucket.humiditySum,
    bucket.humidityCount,
  );
}

export function telemetrySequenceBucketStatement(
  env: Env,
  deviceId: string,
  bucketAt: number,
  sequences: readonly number[],
): D1PreparedStatement {
  if (!sequences.length) throw new Error("telemetry sequence list is empty");
  const placeholders = sequences.map(() => "?").join(",");
  return env.DB.prepare(
    `INSERT INTO environment_buckets(
       device_id,bucket_at,sample_count,co2_sum,co2_count,
       temperature_sum,temperature_count,humidity_sum,humidity_count
     )
     SELECT ?,?,COUNT(*),
       COALESCE(SUM(co2),0),COUNT(co2),
       COALESCE(SUM(COALESCE(temperature_corrected,temperature)),0),
       COUNT(COALESCE(temperature_corrected,temperature)),
       COALESCE(SUM(COALESCE(humidity_corrected,humidity)),0),
       COUNT(COALESCE(humidity_corrected,humidity))
       FROM environment_samples
      WHERE device_id=? AND observed_at>=? AND observed_at<?
        AND sequence IN (${placeholders}) AND bucket_applied=0
     HAVING COUNT(*)>0
     ON CONFLICT(device_id,bucket_at) DO UPDATE SET
       sample_count=environment_buckets.sample_count+excluded.sample_count,
       co2_sum=environment_buckets.co2_sum+excluded.co2_sum,
       co2_count=environment_buckets.co2_count+excluded.co2_count,
       temperature_sum=environment_buckets.temperature_sum+excluded.temperature_sum,
       temperature_count=environment_buckets.temperature_count+excluded.temperature_count,
       humidity_sum=environment_buckets.humidity_sum+excluded.humidity_sum,
       humidity_count=environment_buckets.humidity_count+excluded.humidity_count
     RETURNING bucket_at AS t,
       CASE WHEN co2_count>0 THEN co2_sum/co2_count ELSE NULL END AS co2,
       CASE WHEN temperature_count>0 THEN temperature_sum/temperature_count ELSE NULL END AS temperature,
       CASE WHEN humidity_count>0 THEN humidity_sum/humidity_count ELSE NULL END AS humidity`,
  ).bind(
    deviceId,
    bucketAt,
    deviceId,
    bucketAt,
    bucketAt + ENVIRONMENT_BUCKET_MS,
    ...sequences,
  );
}

export function pendingTelemetryBucketStatement(
  env: Env,
  deviceId: string,
  bucketAt: number,
): D1PreparedStatement {
  return env.DB.prepare(
    `INSERT INTO environment_buckets(
       device_id,bucket_at,sample_count,co2_sum,co2_count,
       temperature_sum,temperature_count,humidity_sum,humidity_count
     )
     SELECT ?1,?2,COUNT(*),
       COALESCE(SUM(co2),0),COUNT(co2),
       COALESCE(SUM(COALESCE(temperature_corrected,temperature)),0),
       COUNT(COALESCE(temperature_corrected,temperature)),
       COALESCE(SUM(COALESCE(humidity_corrected,humidity)),0),
       COUNT(COALESCE(humidity_corrected,humidity))
       FROM environment_samples
      WHERE device_id=?1 AND observed_at>=?2 AND observed_at<?3 AND bucket_applied=0
     HAVING COUNT(*)>0
     ON CONFLICT(device_id,bucket_at) DO UPDATE SET
       sample_count=environment_buckets.sample_count+excluded.sample_count,
       co2_sum=environment_buckets.co2_sum+excluded.co2_sum,
       co2_count=environment_buckets.co2_count+excluded.co2_count,
       temperature_sum=environment_buckets.temperature_sum+excluded.temperature_sum,
       temperature_count=environment_buckets.temperature_count+excluded.temperature_count,
       humidity_sum=environment_buckets.humidity_sum+excluded.humidity_sum,
       humidity_count=environment_buckets.humidity_count+excluded.humidity_count
     RETURNING bucket_at AS t,
       CASE WHEN co2_count>0 THEN co2_sum/co2_count ELSE NULL END AS co2,
       CASE WHEN temperature_count>0 THEN temperature_sum/temperature_count ELSE NULL END AS temperature,
       CASE WHEN humidity_count>0 THEN humidity_sum/humidity_count ELSE NULL END AS humidity`,
  ).bind(deviceId, bucketAt, bucketAt + ENVIRONMENT_BUCKET_MS);
}

export function markTelemetrySamplesAppliedStatement(
  env: Env,
  deviceId: string,
  sequences: readonly number[],
): D1PreparedStatement {
  if (!sequences.length) throw new Error("telemetry sequence list is empty");
  const placeholders = sequences.map((_, index) => `?${index + 2}`).join(",");
  return env.DB.prepare(
    `UPDATE environment_samples
        SET bucket_applied=1
      WHERE device_id=?1 AND sequence IN (${placeholders}) AND bucket_applied=0`,
  ).bind(deviceId, ...sequences);
}

export function markTelemetryBucketAppliedStatement(
  env: Env,
  deviceId: string,
  bucketAt: number,
): D1PreparedStatement {
  return env.DB.prepare(
    `UPDATE environment_samples
        SET bucket_applied=1
      WHERE device_id=?1 AND observed_at>=?2 AND observed_at<?3 AND bucket_applied=0`,
  ).bind(deviceId, bucketAt, bucketAt + ENVIRONMENT_BUCKET_MS);
}

export function telemetryHeartbeatStatement(
  env: Env,
  deviceId: string,
  now: number,
  appVersion: string | null,
  stationheadOk: number,
  outboxCount: number,
  lastSequence: number,
): D1PreparedStatement {
  return env.DB.prepare(
    `INSERT INTO device_heartbeats(
       device_id,last_seen_at,app_version,stationhead_ok,outbox_count,payload,last_sequence
     ) VALUES(?1,?2,?3,?4,?5,NULL,?6)
     ON CONFLICT(device_id) DO UPDATE SET
       last_seen_at=excluded.last_seen_at,
       app_version=excluded.app_version,
       stationhead_ok=excluded.stationhead_ok,
       outbox_count=excluded.outbox_count,
       payload=NULL,
       last_sequence=MAX(device_heartbeats.last_sequence,excluded.last_sequence)`,
  ).bind(deviceId, now, appVersion, stationheadOk, outboxCount, lastSequence);
}
