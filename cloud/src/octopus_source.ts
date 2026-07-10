import { fetchJson } from "./http";
import { JST_MS, jstDayKey as jstDayKeyMs, type Env, type SourceResult } from "./sources";
import { alignedWeekComparison } from "./week_comparison";

const OCTOPUS_TOKEN_TTL_MS = 55 * 60_000;
const DAY_MS = 86_400_000;
const WEEKDAYS = ["月", "火", "水", "木", "金", "土", "日"] as const;

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

function collectErrorText(value: unknown): string[] {
  if (typeof value === "string") return value.trim() ? [value.trim()] : [];
  if (Array.isArray(value)) return value.flatMap(collectErrorText);
  if (value && typeof value === "object") {
    return Object.values(value as Record<string, unknown>).flatMap(collectErrorText);
  }
  return [];
}

function octopusApiError(context: string, issues: OctopusGraphqlIssue[] = []): OctopusApiError {
  const codes = Array.from(new Set(issues.map(issue => issue.extensions?.errorCode).filter((value): value is string => Boolean(value))));
  const types = Array.from(new Set(issues.map(issue => issue.extensions?.errorType).filter((value): value is string => Boolean(value))));
  const details = issues.map(issue => {
    const code = issue.extensions?.errorCode;
    const descriptions = [
      issue.extensions?.errorDescription,
      ...collectErrorText(issue.extensions?.validationErrors),
      issue.message,
    ].filter((value): value is string => Boolean(value));
    const description = Array.from(new Set(descriptions)).join(" / ") || "GraphQL error";
    return code ? `${code}: ${description}` : description;
  });
  return new OctopusApiError(`${context}: ${details.join("; ") || "response did not contain data"}`, codes, types);
}

function isAuthorizationError(error: unknown): boolean {
  if (!(error instanceof OctopusApiError)) return false;
  return error.types.includes("AUTHORIZATION") || error.codes.some(code => [
    "KT-CT-118", "KT-CT-1111", "KT-CT-1112", "KT-CT-4177",
  ].includes(code));
}

async function octopusGraphql<T>(
  query: string,
  variables: Record<string, unknown>,
  token?: string,
): Promise<OctopusGraphqlResponse<T>> {
  const headers = new Headers({ "Content-Type": "application/json" });
  if (token) headers.set("Authorization", token);
  return fetchJson<OctopusGraphqlResponse<T>>("https://api.oejp-kraken.energy/v1/graphql/", {
    method: "POST",
    headers,
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
  const mutation = `mutation login($input: ObtainJSONWebTokenInput!) { obtainKrakenToken(input: $input) { token refreshToken refreshExpiresIn } }`;
  const response = await octopusGraphql<{
    obtainKrakenToken?: { token?: string; refreshToken?: string; refreshExpiresIn?: number };
  }>(mutation, { input });
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
  if (!forceCredentials && octopusToken && Date.now() < octopusToken.expiresAt) return octopusToken.value;
  const cached = octopusToken;
  const refreshToken = !forceCredentials && cached?.refresh &&
    (!cached.refreshExpiresAt || Date.now() < cached.refreshExpiresAt)
    ? cached.refresh
    : undefined;
  if (refreshToken) {
    try {
      const refreshed = await requestOctopusToken({ refreshToken });
      const activeToken: OctopusToken = refreshed.refresh ? refreshed : {
        ...refreshed,
        refresh: refreshToken,
        refreshExpiresAt: cached?.refreshExpiresAt,
      };
      octopusToken = activeToken;
      return activeToken.value;
    } catch {
    }
  }
  octopusToken = await requestOctopusToken(credentialInput(env));
  return octopusToken.value;
}

function jstBoundary(year: number, month: number, day: number): Date {
  return new Date(Date.UTC(year, month, day) - JST_MS);
}

type OctopusReading = { startAt: string; value: string | number; supplyPoint: string };
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
  range: { from: Date; to: Date },
  token: string,
): Promise<OctopusReading[]> {
  const queries = [
    `query readings($accountNumber: String!, $fromDatetime: DateTime, $toDatetime: DateTime) { account(accountNumber: $accountNumber) { properties(activeFrom: $fromDatetime) { electricitySupplyPoints { spin status halfHourlyReadings(fromDatetime: $fromDatetime, toDatetime: $toDatetime) { startAt value } } } } }`,
    `query readings($accountNumber: String!, $fromDatetime: DateTime, $toDatetime: DateTime) { account(accountNumber: $accountNumber) { properties { electricitySupplyPoints { spin status halfHourlyReadings(fromDatetime: $fromDatetime, toDatetime: $toDatetime) { startAt value } } } } }`,
  ];
  let lastErrors: OctopusGraphqlIssue[] = [];
  for (const query of queries) {
    const response = await octopusGraphql<OctopusReadingPayload>(query, {
      accountNumber,
      fromDatetime: range.from.toISOString(),
      toDatetime: range.to.toISOString(),
    }, token);
    const properties = response.data?.account?.properties ?? [];
    const rangeReadings = properties.flatMap((property, propertyIndex) =>
      (property.electricitySupplyPoints ?? []).flatMap((point, pointIndex) => {
        const supplyPoint = point.spin || `${propertyIndex}:${pointIndex}`;
        return (point.halfHourlyReadings ?? []).map(reading => ({ ...reading, supplyPoint }));
      }));
    if (rangeReadings.length > 0) return rangeReadings;
    lastErrors = response.errors ?? lastErrors;
    if (!response.errors?.length && response.data) break;
  }
  if (lastErrors.length) throw octopusApiError("Octopus readings failed", lastErrors);
  throw octopusApiError("Octopus readings failed");
}

async function fetchOctopusReadings(
  accountNumber: string,
  ranges: Array<{ from: Date; to: Date }>,
  token: string,
): Promise<OctopusReading[]> {
  const results = await Promise.all(ranges.map(range => fetchOctopusRangeReadings(accountNumber, range, token)));
  return results.flat();
}

export async function fetchOctopus(env: Env): Promise<SourceResult> {
  const legacyEnv = env as Env & { OCTOPUS_ACCOUNT?: string };
  const accountNumber = (env.OCTOPUS_ACCOUNT_NUMBER || legacyEnv.OCTOPUS_ACCOUNT || "").trim();
  if (!accountNumber) throw new Error("OCTOPUS_ACCOUNT or OCTOPUS_ACCOUNT_NUMBER is not configured");
  const now = new Date();
  const jst = new Date(now.getTime() + JST_MS);
  const billingMonth = jst.getUTCDate() >= 2 ? jst.getUTCMonth() : jst.getUTCMonth() - 1;
  const currentStart = jstBoundary(jst.getUTCFullYear(), billingMonth, 2);
  const previousStart = jstBoundary(jst.getUTCFullYear(), billingMonth - 1, 2);
  const nextStart = jstBoundary(jst.getUTCFullYear(), billingMonth + 1, 2);
  const comparison = alignedWeekComparison(now.getTime());
  const ranges = [
    { from: previousStart, to: currentStart },
    { from: currentStart, to: now },
    { from: comparison.previousYearWeekStart, to: comparison.previousYearWeekEnd },
  ];
  let token = await authenticateOctopus(env);
  let readings: OctopusReading[];
  try {
    readings = await fetchOctopusReadings(accountNumber, ranges, token);
  } catch (error) {
    if (!isAuthorizationError(error)) throw error;
    octopusToken = null;
    token = await authenticateOctopus(env, true);
    readings = await fetchOctopusReadings(accountNumber, ranges, token);
  }
  const seen = new Set<string>();
  const daily: Record<string, number> = {};
  const monthly = { previous: 0, current: 0 };
  let previousSlots = 0;
  for (const reading of readings) {
    const readingKey = `${reading.supplyPoint}:${reading.startAt}`;
    if (seen.has(readingKey)) continue;
    seen.add(readingKey);
    const date = new Date(reading.startAt);
    const value = Number(reading.value ?? 0);
    if (!Number.isFinite(date.getTime()) || !Number.isFinite(value)) continue;
    const key = jstDayKeyMs(date.getTime());
    daily[key] = (daily[key] ?? 0) + value;
    if (date >= previousStart && date < currentStart) { monthly.previous += value; previousSlots += 1; }
    if (date >= currentStart && date < now) monthly.current += value;
  }
  const history = Array.from({ length: 7 }, (_, index) => {
    const currentDate = new Date(comparison.currentWeekStart.getTime() + index * DAY_MS);
    const previousYearDate = new Date(comparison.previousYearWeekStart.getTime() + index * DAY_MS);
    const currentKey = jstDayKeyMs(currentDate.getTime());
    const previousYearKey = jstDayKeyMs(previousYearDate.getTime());
    const currentValue = currentDate.getTime() < now.getTime() && daily[currentKey] !== undefined
      ? Number(daily[currentKey].toFixed(3))
      : null;
    const previousYearValue = daily[previousYearKey] === undefined
      ? null
      : Number(daily[previousYearKey].toFixed(3));
    return {
      weekday: WEEKDAYS[index],
      date: currentKey,
      value: currentValue,
      previousYearDate: previousYearKey,
      previousYearValue,
    };
  });
  const elapsed = Math.max(1, now.getTime() - currentStart.getTime());
  const duration = Math.max(1, nextStart.getTime() - currentStart.getTime());
  const projected = monthly.current * duration / elapsed;
  const expectedSlots = Math.round((currentStart.getTime() - previousStart.getTime()) / DAY_MS) * 48;
  return {
    source: "octopus",
    payload: {
      history,
      comparison: {
        currentIsoYear: comparison.current.year,
        currentIsoWeek: comparison.current.week,
        previousIsoYear: comparison.previousYear.year,
        previousIsoWeek: comparison.previousYear.week,
      },
      lastMonth: { usage: Number(monthly.previous.toFixed(3)), complete: previousSlots / Math.max(1, expectedSlots) >= 0.95, coveredSlots: previousSlots, expectedSlots },
      thisMonth: { usageToDate: Number(monthly.current.toFixed(3)), projectedUsage: Number(projected.toFixed(3)) },
    },
    observedAt: now.getTime(),
  };
}
