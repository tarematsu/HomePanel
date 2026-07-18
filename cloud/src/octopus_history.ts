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

interface BackfillStateRow {
  cursor_before: number;
  consecutive_empty_days: number;
  completed: number;
}

interface StoredReadingRow {
  supply_point: string;
  observed_at: number;
  energy_kwh: number;
}

type NormalizedReading = {
  supplyPoint: string;
  observedAt: number;
  energyKwh: number;
};

const JST_MS = 9 * 60 * 60 * 1000;
const HALF_HOUR_MS = 30 * 60 * 1000;
const DAY_MS = 86_400_000;
const SAFE_RANGE_MS = 2 * DAY_MS;
export const OCTOPUS_HISTORY_FLOOR_MS = Date.UTC(2025, 10, 1) - JST_MS;
const RECENT_REPAIR_DAYS = 2;
const BACKFILL_DAYS_PER_RUN = 30;
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

function collectCoverage(
  timestamps: Set<number>,
  readings: readonly OctopusReading[],
  fromMs: number,
  toMs: number,
): void {
  for (const reading of readings) {
    const observedAt = Date.parse(reading.startAt);
    if (!Number.isFinite(observedAt) || observedAt < fromMs || observedAt >= toMs) continue;
    if ((observedAt - fromMs) % HALF_HOUR_MS !== 0) continue;
    const value = Number(reading.value);
    if (!Number.isFinite(value) || value < 0) continue;
    timestamps.add(observedAt);
  }
}

function hasCompleteHalfHourCoverage(
  range: OctopusRange,
  stored: readonly OctopusReading[],
  fetched: readonly OctopusReading[] = [],
): boolean {
  const fromMs = range.from.getTime();
  const toMs = range.to.getTime();
  const duration = toMs - fromMs;
  if (duration <= 0 || duration % HALF_HOUR_MS !== 0) return false;

  const timestamps = new Set<number>();
  collectCoverage(timestamps, stored, fromMs, toMs);
  collectCoverage(timestamps, fetched, fromMs, toMs);

  const expectedSlots = duration / HALF_HOUR_MS;
  if (timestamps.size !== expectedSlots) return false;
  for (let observedAt = fromMs; observedAt < toMs; observedAt += HALF_HOUR_MS) {
    if (!timestamps.has(observedAt)) return false;
  }
  return true;
}

async function enforceHistoryFloor(env: Env, accountNumber: string, nowMs: number): Promise<void> {
  await env.DB.batch([
    env.DB.prepare(
      "DELETE FROM octopus_readings WHERE account_number=?1 AND observed_at<?2",
    ).bind(accountNumber, OCTOPUS_HISTORY_FLOOR_MS),
    env.DB.prepare(
      "DELETE FROM octopus_sync_ranges WHERE account_number=?1 AND to_at<=?2",
    ).bind(accountNumber, OCTOPUS_HISTORY_FLOOR_MS),
    env.DB.prepare(
      `UPDATE octopus_backfill_state SET
         cursor_before=MAX(cursor_before,?2),
         completed=CASE WHEN MAX(cursor_before,?2)<=?2 THEN 1 ELSE 0 END,
         updated_at=?3
       WHERE account_number=?1`,
    ).bind(accountNumber, OCTOPUS_HISTORY_FLOOR_MS, nowMs),
  ]);
}

async function persistStableReadings(
  env: Env,
  accountNumber: string,
  readings: OctopusReading[],
  stableCutoff: number,
  nowMs: number,
): Promise<number> {
  const normalized = normalizeReadings(readings, stableCutoff);
  if (!normalized.length) return 0;
  const insert = env.DB.prepare(UPSERT_READING_SQL);
  for (let offset = 0; offset < normalized.length; offset += D1_BATCH_SIZE) {
    const end = Math.min(normalized.length, offset + D1_BATCH_SIZE);
    const statements: D1PreparedStatement[] = [];
    for (let index = offset; index < end; index += 1) {
      const reading = normalized[index]!;
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
  return normalized.length;
}

async function fetchAndPersistRanges(
  env: Env,
  accountNumber: string,
  ranges: Iterable<OctopusRange>,
  fetchRange: OctopusRangeFetcher,
  stableCutoff: number,
  nowMs: number,
): Promise<number> {
  let count = 0;
  for (const range of ranges) {
    const readings = await fetchRange(range);
    count += await persistStableReadings(env, accountNumber, readings, stableCutoff, nowMs);
  }
  return count;
}

async function ensurePriorityRange(
  env: Env,
  accountNumber: string,
  rangeKey: string,
  range: OctopusRange,
  fetchRange: OctopusRangeFetcher,
  stableCutoff: number,
  nowMs: number,
): Promise<void> {
  const existing = await env.DB.prepare(
    "SELECT completed_at FROM octopus_sync_ranges WHERE account_number=?1 AND range_key=?2",
  ).bind(accountNumber, rangeKey).first<{ completed_at: number }>();
  if (existing) return;

  const requestedFromMs = range.from.getTime();
  const requestedToMs = range.to.getTime();
  const fromMs = Math.max(OCTOPUS_HISTORY_FLOOR_MS, requestedFromMs);
  const toMs = Math.min(nowMs, requestedToMs);
  const fetched: OctopusReading[] = [];
  if (fromMs < toMs) {
    for (const requestRange of safeRanges(fromMs, toMs)) {
      const readings = await fetchRange(requestRange);
      for (const reading of readings) fetched.push(reading);
      await persistStableReadings(env, accountNumber, readings, stableCutoff, nowMs);
    }
  }

  if (fromMs !== requestedFromMs || toMs !== requestedToMs) return;
  const stored = await readStoredOctopusRanges(env, accountNumber, [range]);
  if (!hasCompleteHalfHourCoverage(range, stored, fetched)) return;

  await env.DB.prepare(
    `INSERT INTO octopus_sync_ranges(account_number,range_key,from_at,to_at,completed_at)
     VALUES(?1,?2,?3,?4,?5)
     ON CONFLICT(account_number,range_key) DO UPDATE SET
       from_at=excluded.from_at,
       to_at=excluded.to_at,
       completed_at=excluded.completed_at`,
  ).bind(accountNumber, rangeKey, requestedFromMs, requestedToMs, nowMs).run();
}

async function loadBackfillState(
  env: Env,
  accountNumber: string,
  initialCursor: number,
  nowMs: number,
): Promise<BackfillStateRow> {
  const state = await env.DB.prepare(
    `INSERT INTO octopus_backfill_state(
       account_number,cursor_before,consecutive_empty_days,completed,updated_at
     ) VALUES(?1,?2,0,0,?3)
     ON CONFLICT(account_number) DO UPDATE SET
       cursor_before=MIN(octopus_backfill_state.cursor_before,excluded.cursor_before),
       completed=CASE
         WHEN MIN(octopus_backfill_state.cursor_before,excluded.cursor_before)<=?4 THEN 1
         ELSE 0
       END,
       updated_at=MAX(octopus_backfill_state.updated_at,excluded.updated_at)
     RETURNING cursor_before,consecutive_empty_days,completed`,
  ).bind(accountNumber, initialCursor, nowMs, OCTOPUS_HISTORY_FLOOR_MS).first<BackfillStateRow>();
  if (!state) throw new Error("Octopus backfill state could not be initialized");
  return state;
}

async function runBackfill(
  env: Env,
  accountNumber: string,
  fetchRange: OctopusRangeFetcher,
  stableCutoff: number,
  nowMs: number,
): Promise<BackfillStateRow> {
  const recentStart = Math.max(OCTOPUS_HISTORY_FLOOR_MS, stableCutoff - RECENT_REPAIR_DAYS * DAY_MS);
  let state = await loadBackfillState(env, accountNumber, recentStart, nowMs);
  if (state.completed && state.cursor_before <= OCTOPUS_HISTORY_FLOOR_MS) return state;

  let cursor = Math.min(state.cursor_before, recentStart);
  let emptyDays = state.consecutive_empty_days;
  let completed = false;
  const updateBackfill = env.DB.prepare(
    `UPDATE octopus_backfill_state SET
       cursor_before=?2,
       consecutive_empty_days=?3,
       completed=?4,
       updated_at=?5
     WHERE account_number=?1`,
  );
  for (let coveredDays = 0; coveredDays < BACKFILL_DAYS_PER_RUN && !completed;) {
    const remainingDays = BACKFILL_DAYS_PER_RUN - coveredDays;
    const requestedSpan = Math.min(SAFE_RANGE_MS, remainingDays * DAY_MS);
    const fromMs = Math.max(OCTOPUS_HISTORY_FLOOR_MS, cursor - requestedSpan);
    if (fromMs >= cursor) {
      completed = true;
      break;
    }
    const readings = await fetchRange({ from: new Date(fromMs), to: new Date(cursor) });
    const stored = await persistStableReadings(env, accountNumber, readings, stableCutoff, nowMs);
    const spanDays = Math.max(1, Math.ceil((cursor - fromMs) / DAY_MS));
    emptyDays = stored > 0 ? 0 : emptyDays + spanDays;
    coveredDays += spanDays;
    cursor = fromMs;
    completed = cursor <= OCTOPUS_HISTORY_FLOOR_MS;
    await updateBackfill.bind(
      accountNumber,
      cursor,
      emptyDays,
      completed ? 1 : 0,
      nowMs,
    ).run();
  }
  state = { cursor_before: cursor, consecutive_empty_days: emptyDays, completed: completed ? 1 : 0 };
  return state;
}

export async function synchronizeOctopusHistory(
  env: Env,
  accountNumber: string,
  nowMs: number,
  comparisonRangeKey: string,
  comparisonRange: OctopusRange,
  fetchRange: OctopusRangeFetcher,
): Promise<OctopusHistorySyncResult> {
  const stableCutoff = octopusStableCutoffJst(nowMs);
  const recentStart = Math.max(OCTOPUS_HISTORY_FLOOR_MS, stableCutoff - RECENT_REPAIR_DAYS * DAY_MS);

  await enforceHistoryFloor(env, accountNumber, nowMs);
  await fetchAndPersistRanges(
    env,
    accountNumber,
    safeRanges(recentStart, stableCutoff),
    fetchRange,
    stableCutoff,
    nowMs,
  );
  await ensurePriorityRange(
    env,
    accountNumber,
    comparisonRangeKey,
    comparisonRange,
    fetchRange,
    stableCutoff,
    nowMs,
  );
  const state = await runBackfill(env, accountNumber, fetchRange, stableCutoff, nowMs);

  const liveReadings: OctopusReading[] = [];
  for (const range of safeRanges(stableCutoff, nowMs)) {
    const readings = await fetchRange(range);
    for (const reading of readings) liveReadings.push(reading);
  }
  return {
    liveReadings,
    stableCutoff,
    historyFloor: OCTOPUS_HISTORY_FLOOR_MS,
    cursorBefore: state.cursor_before,
    completed: state.completed === 1,
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
