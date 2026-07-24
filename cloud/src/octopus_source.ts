import { fetchJson } from "./http";
import {
  octopusCompleteStableThroughJst,
  readOctopusDailyTotals,
  synchronizeOctopusHistory,
  type OctopusDailyTotal,
  type OctopusRange,
  type OctopusReading,
} from "./octopus_history";
import { JST_MS, jstDayKey as jstDayKeyMs, type Env, type SourceResult } from "./sources";

const OCTOPUS_TOKEN_TTL_MS = 55 * 60_000;
const DAY_MS = 86_400_000;
const PROFILE_DAYS = 7;
const SLOTS_PER_DAY = 48;
const WEEKDAY_LABELS = ["日", "月", "火", "水", "木", "金", "土"];
const JSON_HEADERS = { "Content-Type": "application/json" } as const;
const AUTHORIZATION_ERROR_CODES = new Set([
  "KT-CT-118", "KT-CT-1111", "KT-CT-1112", "KT-CT-4177",
]);
const LOGIN_MUTATION = `mutation login($input: ObtainJSONWebTokenInput!) { obtainKrakenToken(input: $input) { token refreshToken refreshExpiresIn } }`;
const READINGS_QUERY = `query readings($accountNumber: String!, $fromDatetime: DateTime, $toDatetime: DateTime) { account(accountNumber: $accountNumber) { properties { electricitySupplyPoints { spin status halfHourlyReadings(fromDatetime: $fromDatetime, toDatetime: $toDatetime) { startAt value } } } } }`;

type OctopusToken = {
  value: string;
  refresh?: string | undefined;
  expiresAt: number;
  refreshExpiresAt?: number | undefined;
};

type OctopusGraphqlIssue = {
  message?: string;
  path?: Array<string | number>;
  extensions?: {
    errorCode?: string;
    errorType?: string;
    errorDescription?: string;
    validationErrors?: unknown;
  };
};

type OctopusGraphqlResponse<T> = {
  data?: T;
  errors?: OctopusGraphqlIssue[];
};

export interface OctopusProfilePoint {
  day: string;
  currentTotal: number | null;
  previousTotal: number | null;
  currentComplete: boolean;
  previousComplete: boolean;
}

export interface OctopusProfileRanges {
  currentStart: Date;
  currentEnd: Date;
  previousStart: Date;
  previousEnd: Date;
}

class OctopusApiError extends Error {
  constructor(
    message: string,
    readonly codes: string[] = [],
    readonly types: string[] = [],
  ) {
    super(message);
    this.name = "OctopusApiError";
  }
}

let octopusToken: OctopusToken | null = null;

function appendUnique(output: string[], seen: Set<string>, value: string | undefined): void {
  if (!value || seen.has(value)) return;
  seen.add(value);
  output.push(value);
}

function collectErrorText(value: unknown, output: string[], seen: Set<string>): void {
  if (typeof value === "string") {
    appendUnique(output, seen, value.trim());
    return;
  }
  if (Array.isArray(value)) {
    for (const item of value) collectErrorText(item, output, seen);
    return;
  }
  if (!value || typeof value !== "object") return;
  const record = value as Record<string, unknown>;
  for (const name in record) {
    if (Object.prototype.hasOwnProperty.call(record, name)) {
      collectErrorText(record[name], output, seen);
    }
  }
}

function octopusApiError(context: string, issues: OctopusGraphqlIssue[] = []): OctopusApiError {
  const codes: string[] = [];
  const types: string[] = [];
  const details: string[] = [];
  const codeSet = new Set<string>();
  const typeSet = new Set<string>();

  for (const issue of issues) {
    const extension = issue.extensions;
    appendUnique(codes, codeSet, extension?.errorCode);
    appendUnique(types, typeSet, extension?.errorType);

    const descriptions: string[] = [];
    const descriptionSet = new Set<string>();
    appendUnique(descriptions, descriptionSet, extension?.errorDescription);
    collectErrorText(extension?.validationErrors, descriptions, descriptionSet);
    appendUnique(descriptions, descriptionSet, issue.message);
    const description = descriptions.join(" / ") || "GraphQL error";
    details.push(extension?.errorCode ? `${extension.errorCode}: ${description}` : description);
  }

  return new OctopusApiError(
    `${context}: ${details.join("; ") || "response did not contain data"}`,
    codes,
    types,
  );
}

function isAuthorizationError(error: unknown): boolean {
  if (!(error instanceof OctopusApiError)) return false;
  for (const type of error.types) if (type === "AUTHORIZATION") return true;
  for (const code of error.codes) if (AUTHORIZATION_ERROR_CODES.has(code)) return true;
  return false;
}

async function octopusGraphql<T>(
  query: string,
  variables: Record<string, unknown>,
  token?: string,
): Promise<OctopusGraphqlResponse<T>> {
  return fetchJson<OctopusGraphqlResponse<T>>("https://api.oejp-kraken.energy/v1/graphql/", {
    method: "POST",
    headers: token ? { "Content-Type": "application/json", Authorization: token } : JSON_HEADERS,
    body: JSON.stringify({ query, variables }),
  });
}

function credentialInput(env: Env): Record<string, string> {
  const email = env.OCTOPUS_EMAIL?.trim();
  const password = env.OCTOPUS_PASSWORD;
  if (!email || !password) throw new Error("OCTOPUS_EMAIL or OCTOPUS_PASSWORD is not configured");
  return { email, password };
}

function normalizeRefreshExpiry(value: unknown): number | undefined {
  const numeric = Number(value);
  if (!Number.isFinite(numeric) || numeric <= 0) return undefined;
  return numeric > 10_000_000_000 ? numeric : numeric * 1000;
}

async function requestOctopusToken(input: Record<string, string>): Promise<OctopusToken> {
  const response = await octopusGraphql<{
    obtainKrakenToken?: { token?: string; refreshToken?: string; refreshExpiresIn?: number };
  }>(LOGIN_MUTATION, { input });
  const result = response.data?.obtainKrakenToken;
  if (response.errors?.length || !result?.token) {
    throw octopusApiError("Octopus authentication failed", response.errors);
  }
  return {
    value: result.token,
    refresh: result.refreshToken,
    expiresAt: Date.now() + OCTOPUS_TOKEN_TTL_MS,
    refreshExpiresAt: normalizeRefreshExpiry(result.refreshExpiresIn),
  };
}

async function authenticateOctopus(env: Env, forceCredentials = false): Promise<string> {
  const now = Date.now();
  if (!forceCredentials && octopusToken && now < octopusToken.expiresAt) return octopusToken.value;
  const cached = octopusToken;
  const refreshToken = !forceCredentials && cached?.refresh &&
    (!cached.refreshExpiresAt || now < cached.refreshExpiresAt)
    ? cached.refresh
    : undefined;
  if (refreshToken) {
    try {
      const refreshed = await requestOctopusToken({ refreshToken });
      if (!refreshed.refresh) {
        refreshed.refresh = refreshToken;
        refreshed.refreshExpiresAt = cached?.refreshExpiresAt;
      }
      octopusToken = refreshed;
      return refreshed.value;
    } catch {
    }
  }
  octopusToken = await requestOctopusToken(credentialInput(env));
  return octopusToken.value;
}

function jstBoundary(year: number, month: number, day: number): Date {
  return new Date(Date.UTC(year, month, day) - JST_MS);
}

export function completeDayProfileRanges(nowMs: number): OctopusProfileRanges {
  const currentEnd = new Date(octopusCompleteStableThroughJst(nowMs));
  const currentStart = new Date(currentEnd.getTime() - PROFILE_DAYS * DAY_MS);
  const previousEnd = currentStart;
  const previousStart = new Date(previousEnd.getTime() - PROFILE_DAYS * DAY_MS);
  return { currentStart, currentEnd, previousStart, previousEnd };
}

type OctopusReadingPayload = {
  account?: {
    properties?: Array<{
      electricitySupplyPoints?: Array<{
        spin?: string;
        status?: string;
        halfHourlyReadings?: Array<{ startAt: string; value: string | number }>;
      }>;
    }>;
  };
};

async function fetchOctopusRangeReadings(
  accountNumber: string,
  range: OctopusRange,
  token: string,
): Promise<OctopusReading[]> {
  const response = await octopusGraphql<OctopusReadingPayload>(READINGS_QUERY, {
    accountNumber,
    fromDatetime: range.from.toISOString(),
    toDatetime: range.to.toISOString(),
  }, token);
  if (response.errors?.length) throw octopusApiError("Octopus readings failed", response.errors);

  const fromMs = range.from.getTime();
  const toMs = range.to.getTime();
  const output: OctopusReading[] = [];
  const properties = response.data?.account?.properties ?? [];
  for (let propertyIndex = 0; propertyIndex < properties.length; propertyIndex += 1) {
    const points = properties[propertyIndex]?.electricitySupplyPoints ?? [];
    for (let pointIndex = 0; pointIndex < points.length; pointIndex += 1) {
      const point = points[pointIndex]!;
      const supplyPoint = point.spin || `${propertyIndex}:${pointIndex}`;
      for (const reading of point.halfHourlyReadings ?? []) {
        const observedAt = Date.parse(reading.startAt);
        if (!Number.isFinite(observedAt) || observedAt < fromMs || observedAt >= toMs) continue;
        output.push({ startAt: reading.startAt, value: reading.value, supplyPoint });
      }
    }
  }
  return output;
}

export function buildOctopusDailyProfile(
  totals: readonly OctopusDailyTotal[],
  ranges: OctopusProfileRanges,
): OctopusProfilePoint[] {
  const byDay = new Map<string, OctopusDailyTotal>();
  for (const total of totals) {
    if (total.slotCount !== SLOTS_PER_DAY || !Number.isFinite(total.energyKwh) || total.energyKwh < 0) continue;
    byDay.set(total.day, total);
  }

  const profile: OctopusProfilePoint[] = [];
  for (let offset = 0; offset < PROFILE_DAYS; offset += 1) {
    const currentDayMs = ranges.currentStart.getTime() + offset * DAY_MS;
    const previousDayMs = ranges.previousStart.getTime() + offset * DAY_MS;
    const current = byDay.get(jstDayKeyMs(currentDayMs));
    const previous = byDay.get(jstDayKeyMs(previousDayMs));
    const weekday = new Date(currentDayMs + JST_MS).getUTCDay();
    profile.push({
      day: WEEKDAY_LABELS[weekday]!,
      currentTotal: current ? Number(current.energyKwh.toFixed(4)) : null,
      previousTotal: previous ? Number(previous.energyKwh.toFixed(4)) : null,
      currentComplete: current !== undefined,
      previousComplete: previous !== undefined,
    });
  }
  return profile;
}

export function projectOctopusMonthlyUsage(
  usageKwh: number,
  coveredDays: number,
  billingDays: number,
): number {
  if (!Number.isFinite(usageKwh) || usageKwh < 0) return 0;
  if (!Number.isFinite(coveredDays) || coveredDays <= 0) return 0;
  if (!Number.isFinite(billingDays) || billingDays <= 0) return 0;
  return usageKwh * billingDays / coveredDays;
}

export async function fetchOctopus(env: Env): Promise<SourceResult> {
  const legacyEnv = env as Env & { OCTOPUS_ACCOUNT?: string };
  const accountNumber = (env.OCTOPUS_ACCOUNT_NUMBER || legacyEnv.OCTOPUS_ACCOUNT || "").trim();
  if (!accountNumber) throw new Error("OCTOPUS_ACCOUNT or OCTOPUS_ACCOUNT_NUMBER is not configured");

  const nowMs = Date.now();
  const jst = new Date(nowMs + JST_MS);
  const billingMonth = jst.getUTCDate() >= 2 ? jst.getUTCMonth() : jst.getUTCMonth() - 1;
  const currentStart = jstBoundary(jst.getUTCFullYear(), billingMonth, 2);
  const previousStart = jstBoundary(jst.getUTCFullYear(), billingMonth - 1, 2);
  const nextStart = jstBoundary(jst.getUTCFullYear(), billingMonth + 1, 2);
  const profileRanges = completeDayProfileRanges(nowMs);

  let token = await authenticateOctopus(env);
  let synchronized;
  try {
    synchronized = await synchronizeOctopusHistory(
      env,
      accountNumber,
      nowMs,
      range => fetchOctopusRangeReadings(accountNumber, range, token),
    );
  } catch (error) {
    if (!isAuthorizationError(error)) throw error;
    octopusToken = null;
    token = await authenticateOctopus(env, true);
    synchronized = await synchronizeOctopusHistory(
      env,
      accountNumber,
      nowMs,
      range => fetchOctopusRangeReadings(accountNumber, range, token),
    );
  }

  const previousStartMs = previousStart.getTime();
  const currentStartMs = currentStart.getTime();
  const dataThroughMs = synchronized.stableThrough;
  const previousStartDay = jstDayKeyMs(previousStartMs);
  const currentStartDay = jstDayKeyMs(currentStartMs);
  const dataThroughDay = jstDayKeyMs(dataThroughMs);
  const profileStartDay = jstDayKeyMs(profileRanges.previousStart.getTime());
  const queryFromDay = previousStartDay < profileStartDay ? previousStartDay : profileStartDay;
  const totals = await readOctopusDailyTotals(env, accountNumber, queryFromDay, dataThroughDay);

  let previousUsage = 0;
  let currentUsage = 0;
  let previousDays = 0;
  let currentDays = 0;
  for (const total of totals) {
    if (total.slotCount !== SLOTS_PER_DAY) continue;
    if (total.day >= previousStartDay && total.day < currentStartDay) {
      previousUsage += total.energyKwh;
      previousDays += 1;
    }
    if (total.day >= currentStartDay && total.day < dataThroughDay) {
      currentUsage += total.energyKwh;
      currentDays += 1;
    }
  }

  const profile = buildOctopusDailyProfile(totals, profileRanges);
  const billingDays = Math.max(1, Math.round((nextStart.getTime() - currentStartMs) / DAY_MS));
  const projected = projectOctopusMonthlyUsage(currentUsage, currentDays, billingDays);
  const expectedDays = Math.round((currentStartMs - previousStartMs) / DAY_MS);
  return {
    source: "octopus",
    payload: {
      profile,
      comparison: {
        currentLabel: "今週",
        previousLabel: "先週",
        currentStartDate: jstDayKeyMs(profileRanges.currentStart.getTime()),
        currentEndDate: jstDayKeyMs(profileRanges.currentEnd.getTime() - DAY_MS),
        previousStartDate: jstDayKeyMs(profileRanges.previousStart.getTime()),
        previousEndDate: jstDayKeyMs(profileRanges.previousEnd.getTime() - DAY_MS),
        excludedRecentDays: 2,
      },
      archive: {
        stableThrough: synchronized.stableThrough,
        rawStableCutoff: synchronized.stableCutoff,
        historyFloor: synchronized.historyFloor,
        cursorBefore: synchronized.cursorBefore,
        cursorAfter: synchronized.cursorAfter,
        fetchFrom: synchronized.fetchFrom,
        completed: synchronized.completed,
        excludedRecentDays: 2,
      },
      lastMonth: {
        usage: Number(previousUsage.toFixed(3)),
        complete: previousDays / Math.max(1, expectedDays) >= 0.95,
        coveredDays: previousDays,
        expectedDays,
        coveredSlots: previousDays * SLOTS_PER_DAY,
        expectedSlots: expectedDays * SLOTS_PER_DAY,
      },
      thisMonth: {
        usageToDate: Number(currentUsage.toFixed(3)),
        projectedUsage: Number(projected.toFixed(3)),
        coveredDays: currentDays,
        expectedDays: billingDays,
      },
    },
    observedAt: nowMs,
  };
}
