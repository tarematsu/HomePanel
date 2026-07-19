import { changedOctopusReadings, type ComparableOctopusReading } from "./octopus_reading_filter";
import type { Env } from "./sources";

export interface OctopusReading {
  startAt: string;
  value: string | number;
  supplyPoint: string;
}

export interface OctopusRange {
  from: Date;
  to: Date;
}

export type OctopusRangeFetcher = (range: OctopusRange) => Promise<OctopusReading[]>;

export interface OctopusHistorySyncResult {
  liveReadings: OctopusReading[];
  stableCutoff: number;
  historyFloor: number;
  cursorBefore: number;
  completed: boolean;
}

interface StoredReadingRow {
  supply_point: string;
  observed_at: number;
  energy_kwh: number;
}

type NormalizedReading = ComparableOctopusReading;

const JST_MS = 9 * 60 * 60 * 1000;
const HALF_HOUR_MS = 30 * 60 * 1000;
const DAY_MS = 86_400_000;
const SAFE_RANGE_MS = 2 * DAY_MS;
export const OCTOPUS_HISTORY_FLOOR_MS = Date.UTC(2025, 10, 1) - JST_MS;
export const OCTOPUS_COLLECTION_DAYS = 7;
const D1_BATCH_SIZE = 90;
const UPSERT_READING_SQL = `INSERT INTO octopus_readings(account_number,supply_point,observed_at,energy_kwh,updated_at)
 VALUES(?1,?2,?3,?4,?5)
 ON CONFLICT(account_number,supply_point,observed_at) DO UPDATE SET
   energy_kwh=excluded.energy_kwh,
   updated_at=excluded.updated_at
 WHERE octopus_readings.energy_kwh IS NOT excluded.energy_kwh`;

export function octopusStableCutoffJst(nowMs: number): number {
  return Math.floor((nowMs - 2 * DAY_MS) / HALF_HOUR_MS) * HALF_HOUR_MS;
}

export function octopusCollectionStart(nowMs: number): number {
  if (!Number.isFinite(nowMs)) throw new Error("Octopus collection time must be finite");
  const recentStart = Math.floor((nowMs - OCTOPUS_COLLECTION_DAYS * DAY_MS) / HALF_HOUR_MS) * HALF_HOUR_MS;
  return Math.max(OCTOPUS_HISTORY_FLOOR_MS, recentStart);
}

function* safeRanges(fromMs: number, toMs: number): IterableIterator<OctopusRange> {
  for (let cursor = fromMs; cursor < toMs;) {
    const next = Math.min(toMs, cursor + SAFE_RANGE_MS);
    yield { from: new Date(cursor), to: new Date(next) };
    cursor = next;
  }
}

function normalizeReadings(readings: OctopusReading[], stableCutoff: number): NormalizedReading[] {
  const unique = new Map<string, NormalizedReading>();
  for (const reading of readings) {
    const observedAt = Date.parse(reading.startAt);
    const energyKwh = Number(reading.value);
    const supplyPoint = String(reading.supplyPoint ?? "").trim();
    if (!supplyPoint || !Number.isFinite(observedAt) ||
        observedAt < OCTOPUS_HISTORY_FLOOR_MS || observedAt >= stableCutoff) continue;
    if (!Number.isFinite(energyKwh) || energyKwh < 0) continue;
    unique.set(`${supplyPoint}:${observedAt}`, { supplyPoint, observedAt, energyKwh });
  }
  return Array.from(unique.values());
}

async function enforceHistoryFloor(env: Env, accountNumber: string): Promise<void> {
  await env.DB.prepare(
    "DELETE FROM octopus_readings WHERE account_number=?1 AND observed_at<?2",
  ).bind(accountNumber, OCTOPUS_HISTORY_FLOOR_MS).run();
}

async function readComparableStoredReadings(
  env: Env,
  accountNumber: string,
  normalized: readonly NormalizedReading[],
): Promise<ComparableOctopusReading[]> {
  let fromMs = Number.POSITIVE_INFINITY;
  let toMs = Number.NEGATIVE_INFINITY;
  for (const reading of normalized) {
    if (reading.observedAt < fromMs) fromMs = reading.observedAt;
    if (reading.observedAt > toMs) toMs = reading.observedAt;
  }
  const result = await env.DB.prepare(
    `SELECT supply_point,observed_at,energy_kwh
       FROM octopus_readings
      WHERE account_number=?1 AND observed_at>=?2 AND observed_at<=?3`,
  ).bind(accountNumber, fromMs, toMs).all<StoredReadingRow>();
  return (result.results ?? []).map(row => ({
    supplyPoint: row.supply_point,
    observedAt: Number(row.observed_at),
    energyKwh: Number(row.energy_kwh),
  }));
}

async function persistStableReadings(
  env: Env,
  accountNumber: string,
  readings: OctopusReading[],
  stableCutoff: number,
  nowMs: number,
): Promise<void> {
  const normalized = normalizeReadings(readings, stableCutoff);
  if (!normalized.length) return;
  const stored = await readComparableStoredReadings(env, accountNumber, normalized);
  const changed = changedOctopusReadings(normalized, stored);
  if (!changed.length) return;

  const insert = env.DB.prepare(UPSERT_READING_SQL);
  for (let offset = 0; offset < changed.length; offset += D1_BATCH_SIZE) {
    const end = Math.min(changed.length, offset + D1_BATCH_SIZE);
    const statements: D1PreparedStatement[] = [];
    for (let index = offset; index < end; index += 1) {
      const reading = changed[index]!;
      statements.push(insert.bind(
        accountNumber,
        reading.supplyPoint,
        reading.observedAt,
        reading.energyKwh,
        nowMs,
      ));
    }
    await env.DB.batch(statements);
  }
}

function addLiveReadings(
  output: Map<string, { reading: OctopusReading; observedAt: number }>,
  readings: readonly OctopusReading[],
  stableCutoff: number,
  nowMs: number,
): void {
  for (const reading of readings) {
    const observedAt = Date.parse(reading.startAt);
    const energyKwh = Number(reading.value);
    const supplyPoint = String(reading.supplyPoint ?? "").trim();
    if (!supplyPoint || !Number.isFinite(observedAt) ||
        observedAt < stableCutoff || observedAt >= nowMs) continue;
    if (!Number.isFinite(energyKwh) || energyKwh < 0) continue;
    output.set(`${supplyPoint}:${observedAt}`, {
      observedAt,
      reading: { startAt: reading.startAt, value: reading.value, supplyPoint },
    });
  }
}

export async function synchronizeOctopusHistory(
  env: Env,
  accountNumber: string,
  nowMs: number,
  _comparisonRangeKey: string,
  _comparisonRange: OctopusRange,
  fetchRange: OctopusRangeFetcher,
): Promise<OctopusHistorySyncResult> {
  if (!Number.isFinite(nowMs)) throw new Error("Octopus synchronization time must be finite");
  const stableCutoff = octopusStableCutoffJst(nowMs);
  const collectionStart = octopusCollectionStart(nowMs);
  const live = new Map<string, { reading: OctopusReading; observedAt: number }>();

  // Historical rows remain available for monthly totals and week comparisons.
  // Only network refreshes are bounded to the latest seven days.
  await enforceHistoryFloor(env, accountNumber);
  for (const range of safeRanges(collectionStart, nowMs)) {
    const readings = await fetchRange(range);
    await persistStableReadings(env, accountNumber, readings, stableCutoff, nowMs);
    addLiveReadings(live, readings, stableCutoff, nowMs);
  }

  const liveReadings = Array.from(live.values())
    .sort((left, right) => left.observedAt - right.observedAt ||
      left.reading.supplyPoint.localeCompare(right.reading.supplyPoint))
    .map(item => item.reading);
  return {
    liveReadings,
    stableCutoff,
    historyFloor: OCTOPUS_HISTORY_FLOOR_MS,
    cursorBefore: collectionStart,
    completed: true,
  };
}

export async function readStoredOctopusRanges(
  env: Env,
  accountNumber: string,
  ranges: OctopusRange[],
): Promise<OctopusReading[]> {
  const unique = new Map<string, OctopusReading>();
  const query = env.DB.prepare(
    `SELECT supply_point,observed_at,energy_kwh
       FROM octopus_readings
      WHERE account_number=?1 AND observed_at>=?2 AND observed_at<?3
      ORDER BY observed_at`,
  );
  for (const range of ranges) {
    const result = await query.bind(
      accountNumber,
      range.from.getTime(),
      range.to.getTime(),
    ).all<StoredReadingRow>();
    for (const row of result.results ?? []) {
      const observedAt = Number(row.observed_at);
      const key = `${row.supply_point}:${observedAt}`;
      unique.set(key, {
        supplyPoint: row.supply_point,
        startAt: new Date(observedAt).toISOString(),
        value: Number(row.energy_kwh),
      });
    }
  }
  return Array.from(unique.values());
}
