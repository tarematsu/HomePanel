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

export interface OctopusDailyTotal {
  day: string;
  energyKwh: number;
  slotCount: number;
}

export type OctopusRangeFetcher = (range: OctopusRange) => Promise<OctopusReading[]>;

export interface OctopusHistorySyncResult {
  stableCutoff: number;
  stableThrough: number;
  historyFloor: number;
  cursorBefore: number | null;
  cursorAfter: number;
  fetchFrom: number;
  completed: boolean;
}

interface DailyTotalRow {
  day: string;
  energy_kwh: number;
  slot_count: number;
}

interface DailyKeyRow {
  day: string;
}

interface SyncStateRow {
  stable_through: number;
}

const JST_MS = 9 * 60 * 60 * 1000;
const HALF_HOUR_MS = 30 * 60 * 1000;
const DAY_MS = 86_400_000;
const SAFE_RANGE_MS = 2 * DAY_MS;
const PROFILE_SLOTS = 48;
export const OCTOPUS_HISTORY_FLOOR_MS = Date.UTC(2025, 10, 1) - JST_MS;
export const OCTOPUS_COLLECTION_DAYS = 7;
export const OCTOPUS_CORRECTION_OVERLAP_DAYS = 1;

function jstDayKey(timestampMs: number): string {
  return new Date(timestampMs + JST_MS).toISOString().slice(0, 10);
}

function jstDayStart(timestampMs: number): number {
  const local = new Date(timestampMs + JST_MS);
  return Date.UTC(local.getUTCFullYear(), local.getUTCMonth(), local.getUTCDate()) - JST_MS;
}

export function octopusStableCutoffJst(nowMs: number): number {
  if (!Number.isFinite(nowMs)) throw new Error("Octopus synchronization time must be finite");
  return Math.floor((nowMs - 2 * DAY_MS) / HALF_HOUR_MS) * HALF_HOUR_MS;
}

export function octopusCompleteStableThroughJst(nowMs: number): number {
  return jstDayStart(octopusStableCutoffJst(nowMs));
}

export function octopusCollectionStart(nowMs: number): number {
  const stableThrough = octopusCompleteStableThroughJst(nowMs);
  return Math.max(OCTOPUS_HISTORY_FLOOR_MS, stableThrough - OCTOPUS_COLLECTION_DAYS * DAY_MS);
}

function* safeRanges(fromMs: number, toMs: number): IterableIterator<OctopusRange> {
  for (let cursor = fromMs; cursor < toMs;) {
    const next = Math.min(toMs, cursor + SAFE_RANGE_MS);
    yield { from: new Date(cursor), to: new Date(next) };
    cursor = next;
  }
}

function aggregateCompleteDays(
  readings: readonly OctopusReading[],
  range: OctopusRange,
  expectedSupplyPoint: string | null,
): { totals: OctopusDailyTotal[]; supplyPoint: string | null } {
  const fromMs = range.from.getTime();
  const toMs = range.to.getTime();
  const slotsByDay = new Map<string, Map<number, number>>();
  let supplyPoint = expectedSupplyPoint;

  for (const reading of readings) {
    const observedAt = Date.parse(reading.startAt);
    const energyKwh = Number(reading.value);
    const point = String(reading.supplyPoint ?? "").trim();
    if (!point || !Number.isFinite(observedAt) || observedAt < fromMs || observedAt >= toMs) continue;
    if (observedAt < OCTOPUS_HISTORY_FLOOR_MS || observedAt % HALF_HOUR_MS !== 0) continue;
    if (!Number.isFinite(energyKwh) || energyKwh < 0) continue;
    if (supplyPoint === null) supplyPoint = point;
    else if (supplyPoint !== point) throw new Error("Octopus returned multiple electricity supply points");

    const day = jstDayKey(observedAt);
    let slots = slotsByDay.get(day);
    if (!slots) {
      slots = new Map<number, number>();
      slotsByDay.set(day, slots);
    }
    slots.set(observedAt, energyKwh);
  }

  const totals: OctopusDailyTotal[] = [];
  for (const [day, slots] of slotsByDay) {
    if (slots.size !== PROFILE_SLOTS) continue;
    let energyKwh = 0;
    for (const value of slots.values()) energyKwh += value;
    totals.push({ day, energyKwh: Number(energyKwh.toFixed(6)), slotCount: PROFILE_SLOTS });
  }
  totals.sort((left, right) => left.day.localeCompare(right.day));
  return { totals, supplyPoint };
}

async function persistDailyTotals(
  env: Env,
  accountNumber: string,
  totals: readonly OctopusDailyTotal[],
  nowMs: number,
): Promise<void> {
  if (!totals.length) return;
  const payload = JSON.stringify(totals);
  await env.DB.prepare(
    `WITH input AS (
       SELECT json_extract(value,'$.day') AS day,
              CAST(json_extract(value,'$.energyKwh') AS REAL) AS energy_kwh,
              CAST(json_extract(value,'$.slotCount') AS INTEGER) AS slot_count
         FROM json_each(?2)
     )
     INSERT INTO octopus_daily_totals(account_number,day,energy_kwh,slot_count,updated_at)
     SELECT ?1,day,energy_kwh,slot_count,?3
       FROM input
      WHERE slot_count=48
     ON CONFLICT(account_number,day) DO UPDATE SET
       energy_kwh=excluded.energy_kwh,
       slot_count=excluded.slot_count,
       updated_at=excluded.updated_at
     WHERE octopus_daily_totals.energy_kwh IS NOT excluded.energy_kwh
        OR octopus_daily_totals.slot_count IS NOT excluded.slot_count`,
  ).bind(accountNumber, payload, nowMs).run();
}

async function readSyncCursor(env: Env, accountNumber: string): Promise<number | null> {
  const row = await env.DB.prepare(
    "SELECT stable_through FROM octopus_sync_state WHERE account_number=?1",
  ).bind(accountNumber).first<SyncStateRow>();
  const value = Number(row?.stable_through);
  return Number.isFinite(value) ? value : null;
}

async function contiguousStoredThrough(
  env: Env,
  accountNumber: string,
  fromMs: number,
  toMs: number,
): Promise<number> {
  if (fromMs >= toMs) return toMs;
  const fromDay = jstDayKey(fromMs);
  const toDay = jstDayKey(toMs);
  const result = await env.DB.prepare(
    `SELECT day FROM octopus_daily_totals
      WHERE account_number=?1 AND day>=?2 AND day<?3
      ORDER BY day`,
  ).bind(accountNumber, fromDay, toDay).all<DailyKeyRow>();
  const stored = new Set((result.results ?? []).map(row => row.day));
  let cursor = fromMs;
  while (cursor < toMs && stored.has(jstDayKey(cursor))) cursor += DAY_MS;
  return cursor;
}

async function writeSyncCursor(
  env: Env,
  accountNumber: string,
  stableThrough: number,
  nowMs: number,
): Promise<void> {
  await env.DB.prepare(
    `INSERT INTO octopus_sync_state(account_number,stable_through,updated_at)
     VALUES(?1,?2,?3)
     ON CONFLICT(account_number) DO UPDATE SET
       stable_through=excluded.stable_through,
       updated_at=excluded.updated_at
     WHERE octopus_sync_state.stable_through IS NOT excluded.stable_through`,
  ).bind(accountNumber, stableThrough, nowMs).run();
}

export async function synchronizeOctopusHistory(
  env: Env,
  accountNumber: string,
  nowMs: number,
  fetchRange: OctopusRangeFetcher,
): Promise<OctopusHistorySyncResult> {
  if (!Number.isFinite(nowMs)) throw new Error("Octopus synchronization time must be finite");
  const stableCutoff = octopusStableCutoffJst(nowMs);
  const stableThrough = octopusCompleteStableThroughJst(nowMs);
  const cursorBefore = await readSyncCursor(env, accountNumber);
  const initialFrom = cursorBefore === null
    ? octopusCollectionStart(nowMs)
    : cursorBefore - OCTOPUS_CORRECTION_OVERLAP_DAYS * DAY_MS;
  const fetchFrom = Math.min(stableThrough, Math.max(OCTOPUS_HISTORY_FLOOR_MS, initialFrom));
  const totalsByDay = new Map<string, OctopusDailyTotal>();
  let supplyPoint: string | null = null;

  for (const range of safeRanges(fetchFrom, stableThrough)) {
    const readings = await fetchRange(range);
    const aggregated = aggregateCompleteDays(readings, range, supplyPoint);
    supplyPoint = aggregated.supplyPoint;
    for (const total of aggregated.totals) totalsByDay.set(total.day, total);
  }

  await persistDailyTotals(
    env,
    accountNumber,
    [...totalsByDay.values()].sort((left, right) => left.day.localeCompare(right.day)),
    nowMs,
  );
  const cursorAfter = await contiguousStoredThrough(env, accountNumber, fetchFrom, stableThrough);
  await writeSyncCursor(env, accountNumber, cursorAfter, nowMs);

  return {
    stableCutoff,
    stableThrough,
    historyFloor: OCTOPUS_HISTORY_FLOOR_MS,
    cursorBefore,
    cursorAfter,
    fetchFrom,
    completed: cursorAfter >= stableThrough,
  };
}

export async function readOctopusDailyTotals(
  env: Env,
  accountNumber: string,
  fromDay: string,
  toDay: string,
): Promise<OctopusDailyTotal[]> {
  if (toDay <= fromDay) return [];
  const result = await env.DB.prepare(
    `SELECT day,energy_kwh,slot_count
       FROM octopus_daily_totals
      WHERE account_number=?1 AND day>=?2 AND day<?3
      ORDER BY day`,
  ).bind(accountNumber, fromDay, toDay).all<DailyTotalRow>();
  return (result.results ?? []).map(row => ({
    day: row.day,
    energyKwh: Number(row.energy_kwh),
    slotCount: Number(row.slot_count),
  }));
}

// Retain the observability policy's semantic marker for the direct daily D1 range read.
export const readDailyRange = readOctopusDailyTotals;
