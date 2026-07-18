import { readState, updateState, type StateRow } from "./snapshot";
import type { Env, SourceResult } from "./sources";

const SOURCE = "stationhead_health";
const DEFAULT_STALE_MS = 15 * 60_000;
const MIN_STALE_MS = 5 * 60_000;
const MAX_STALE_MS = 24 * 60 * 60_000;
const REQUEST_TIMEOUT_MS = 8_000;
const RESEND_ENDPOINT = "https://api.resend.com/emails";
const DEFAULT_FROM = "HomePanel Monitor <onboarding@resend.dev>";
const HEALTH_REQUEST_HEADERS = { Accept: "application/json", "Cache-Control": "no-cache, no-store" };
const JST_FORMATTER = new Intl.DateTimeFormat("ja-JP", {
  timeZone: "Asia/Tokyo",
  year: "numeric",
  month: "2-digit",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
  second: "2-digit",
  hour12: false,
});

type RemoteHealth = {
  ok?: boolean;
  collector_health_ok?: boolean;
  collector_stale?: boolean;
  collector_health_stale?: boolean;
  collector_age_ms?: number | null;
  collector_health_age_ms?: number | null;
  collector_stale_after_ms?: number | null;
  collector_health_stale_after_ms?: number | null;
  collector_last_run_at?: number | null;
  collector_last_success_at?: number | null;
  collector_last_error_present?: boolean;
};

type AlertConfig = { enabled: boolean; apiKey: string; to: string; from: string };

export type StationheadHealthSnapshot = {
  configured: boolean;
  reachable: boolean;
  healthy: boolean;
  statusCode: number | null;
  sampledAt: number;
  lastRunAt: number | null;
  lastSuccessAt: number | null;
  ageMs: number | null;
  staleAfterMs: number;
  reason: string | null;
  alertConfigured: boolean;
  alertPending: boolean;
  recoveryPending: boolean;
  alertEventKey: string | null;
};

function finite(value: unknown): number | null {
  if (value === undefined || value === null || value === "") return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function firstFinite(first: unknown, second: unknown): number | null {
  const firstNumber = finite(first);
  return firstNumber === null ? finite(second) : firstNumber;
}

function firstBoolean(first: unknown, second: unknown): boolean | null {
  if (typeof first === "boolean") return first;
  return typeof second === "boolean" ? second : null;
}

function positive(value: unknown, fallback: number): number {
  const number = Number(value);
  if (!Number.isFinite(number) || number <= 0) return fallback;
  return Math.max(MIN_STALE_MS, Math.min(MAX_STALE_MS, Math.trunc(number)));
}

function objectOrEmpty(value: unknown): RemoteHealth {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as RemoteHealth
    : {};
}

function unavailableSnapshot(
  configured: boolean,
  now: number,
  staleAfterMs: number,
  reason: string,
): StationheadHealthSnapshot {
  return {
    configured,
    reachable: false,
    healthy: false,
    statusCode: null,
    sampledAt: now,
    lastRunAt: null,
    lastSuccessAt: null,
    ageMs: null,
    staleAfterMs,
    reason,
    alertConfigured: false,
    alertPending: false,
    recoveryPending: false,
    alertEventKey: null,
  };
}

export function stationheadHealthUrl(env: Pick<Env, "STATIONHEAD_HEALTH_URL" | "STATIONHEAD_MONITOR_URL">): string {
  const explicit = env.STATIONHEAD_HEALTH_URL?.trim();
  if (explicit) return explicit;

  const monitor = env.STATIONHEAD_MONITOR_URL?.trim();
  if (!monitor) throw new Error("STATIONHEAD_HEALTH_URL or STATIONHEAD_MONITOR_URL is not configured");
  const url = new URL(monitor);
  let pathEnd = url.pathname.length;
  while (pathEnd > 0 && url.pathname.charCodeAt(pathEnd - 1) === 47) pathEnd -= 1;
  const path = url.pathname.slice(0, pathEnd);
  const apiAt = path.lastIndexOf("/api/");
  if (apiAt >= 0 && path.indexOf("/", apiAt + 5) === -1) {
    url.pathname = `${path.slice(0, apiAt + 5)}health`;
  } else {
    url.pathname = `${path}/api/health` || "/api/health";
  }
  url.search = "";
  url.hash = "";
  return url.toString();
}

export function evaluateStationheadHealth(
  payload: RemoteHealth | null,
  responseOk: boolean,
  statusCode: number | null,
  now = Date.now(),
  configuredStaleMs = DEFAULT_STALE_MS,
): StationheadHealthSnapshot {
  const remoteStaleAfterMs = firstFinite(
    payload?.collector_stale_after_ms,
    payload?.collector_health_stale_after_ms,
  );
  const staleAfterMs = positive(configuredStaleMs, positive(remoteStaleAfterMs, DEFAULT_STALE_MS));
  const lastRunAt = finite(payload?.collector_last_run_at);
  const lastSuccessAt = finite(payload?.collector_last_success_at);
  const reportedAge = firstFinite(payload?.collector_age_ms, payload?.collector_health_age_ms);
  const ageMs = reportedAge ?? (lastSuccessAt == null ? null : Math.max(0, now - lastSuccessAt));
  const remoteStale = firstBoolean(payload?.collector_stale, payload?.collector_health_stale) === true;
  const remoteHealthy = firstBoolean(payload?.ok, payload?.collector_health_ok);
  const reachable = payload !== null;
  const payloadValid = reachable && remoteHealthy !== null;
  const hasSuccessfulCollection = lastSuccessAt !== null && lastSuccessAt > 0;
  const stale = remoteStale || (ageMs != null && ageMs >= staleAfterMs);
  const healthy = reachable
    && responseOk
    && payloadValid
    && remoteHealthy === true
    && hasSuccessfulCollection
    && !stale;

  let reason: string | null = null;
  if (!reachable) reason = "Stationhead health endpoint is unreachable";
  else if (!responseOk) reason = `Stationhead health endpoint returned HTTP ${statusCode ?? "error"}`;
  else if (!payloadValid) reason = "Stationhead health response is missing an explicit health status";
  else if (!hasSuccessfulCollection) reason = "Stationhead collector has no successful collection";
  else if (stale) reason = "Stationhead collection is stale";
  else if (remoteHealthy === false) reason = "Stationhead collector reported unhealthy";

  return {
    configured: true,
    reachable,
    healthy,
    statusCode,
    sampledAt: now,
    lastRunAt,
    lastSuccessAt,
    ageMs,
    staleAfterMs,
    reason,
    alertConfigured: false,
    alertPending: false,
    recoveryPending: false,
    alertEventKey: null,
  };
}

function parsePrevious(row: StateRow | null): StationheadHealthSnapshot | null {
  if (!row) return null;
  try {
    const value = JSON.parse(row.payload) as StationheadHealthSnapshot;
    if (typeof value?.healthy !== "boolean") return null;
    value.alertConfigured = value.alertConfigured === true;
    value.alertPending = value.alertPending === true;
    value.recoveryPending = value.recoveryPending === true;
    value.alertEventKey = typeof value.alertEventKey === "string" && value.alertEventKey
      ? value.alertEventKey
      : null;
    return value;
  } catch {
    return null;
  }
}

function alertConfig(env: Env): AlertConfig {
  const apiKey = env.RESEND_API_KEY?.trim() || "";
  const to = env.STATIONHEAD_ALERT_TO?.trim() || "";
  const from = env.STATIONHEAD_ALERT_FROM?.trim() || DEFAULT_FROM;
  return { enabled: Boolean(apiKey && to), apiKey, to, from };
}

function formatJst(timestamp: number | null): string {
  return timestamp == null ? "不明" : JST_FORMATTER.format(new Date(timestamp));
}

export function alertTransitionKey(
  snapshot: StationheadHealthSnapshot,
  previous: StationheadHealthSnapshot | null,
  recovery: boolean,
): string {
  const matchingRetryPending = recovery
    ? previous?.recoveryPending === true
    : previous?.alertPending === true;
  if (matchingRetryPending && previous?.alertEventKey) {
    return previous.alertEventKey;
  }
  const anchor = recovery
    ? previous?.lastSuccessAt ?? snapshot.lastSuccessAt ?? "never"
    : snapshot.lastSuccessAt ?? previous?.lastSuccessAt ?? "never";
  return `homepanel-stationhead-${recovery ? "recovered" : "down"}-${anchor}`;
}

async function sendTransitionAlert(
  config: AlertConfig,
  snapshot: StationheadHealthSnapshot,
  recovery: boolean,
  eventKey: string,
): Promise<void> {
  if (!config.enabled) return;
  const status = snapshot.reason || (snapshot.healthy ? "正常" : "異常");
  const text = `${recovery
    ? "Stationhead収集が正常状態へ復帰しました。"
    : "Stationhead収集が正常に更新されていません。"}\n\n確認時刻: ${formatJst(snapshot.sampledAt)}\n最終実行: ${formatJst(snapshot.lastRunAt)}\n最終成功: ${formatJst(snapshot.lastSuccessAt)}\n状態: ${status}`;
  const response = await fetch(RESEND_ENDPOINT, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${config.apiKey}`,
      "Content-Type": "application/json",
      "Idempotency-Key": eventKey,
    },
    body: JSON.stringify({
      from: config.from,
      to: [config.to],
      subject: recovery
        ? "【HomePanel】Stationhead収集が復旧しました"
        : "【HomePanel】Stationhead収集停止を検知",
      text,
    }),
    signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
  });
  if (!response.ok) {
    const detail = await response.text().catch(() => "");
    throw new Error(`Resend HTTP ${response.status}${detail ? `: ${detail.slice(0, 300)}` : ""}`);
  }
  await response.body?.cancel();
}

async function fetchRemoteHealth(env: Env, now: number): Promise<StationheadHealthSnapshot> {
  const staleMs = positive(env.STATIONHEAD_HEALTH_STALE_MS, DEFAULT_STALE_MS);
  let url: string;
  try {
    url = stationheadHealthUrl(env);
  } catch (error) {
    return unavailableSnapshot(
      false,
      now,
      staleMs,
      error instanceof Error ? error.message : String(error),
    );
  }

  try {
    const response = await fetch(url, {
      headers: HEALTH_REQUEST_HEADERS,
      cache: "no-store",
      signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
    });
    let payload: RemoteHealth;
    try {
      payload = objectOrEmpty(await response.json());
    } catch {
      payload = {};
    }
    return evaluateStationheadHealth(payload, response.ok, response.status, now, staleMs);
  } catch (error) {
    return unavailableSnapshot(
      true,
      now,
      staleMs,
      error instanceof Error ? error.message : String(error),
    );
  }
}

async function persistHealth(env: Env, snapshot: StationheadHealthSnapshot, previous: StateRow | null): Promise<void> {
  const result: SourceResult = {
    source: SOURCE,
    payload: snapshot,
    observedAt: snapshot.lastSuccessAt ?? snapshot.sampledAt,
  };
  await updateState(env, result, undefined, previous);
  if (!snapshot.healthy) {
    const status = !snapshot.configured || !snapshot.reachable || !snapshot.lastSuccessAt
      ? "error"
      : "stale";
    await env.DB.prepare(
      "UPDATE current_state SET status=?1, error=?2 WHERE source=?3",
    ).bind(status, snapshot.reason || "Stationhead collector is unhealthy", SOURCE).run();
  }
}

export async function runStationheadHealthMonitor(env: Env, now = Date.now()): Promise<StationheadHealthSnapshot> {
  const previousRow = await readState(env, SOURCE);
  const previous = parsePrevious(previousRow);
  const current = await fetchRemoteHealth(env, now);
  const alerts = alertConfig(env);
  current.alertConfigured = alerts.enabled;

  const alertsNewlyEnabled = alerts.enabled && previous?.alertConfigured !== true;
  const downTransition = !current.healthy
    && (previous?.healthy !== false || previous.alertPending === true || alertsNewlyEnabled);
  const recoveryTransition = current.healthy
    && (previous?.healthy === false || previous?.recoveryPending === true);

  if (alerts.enabled && (downTransition || recoveryTransition)) {
    current.alertEventKey = alertTransitionKey(current, previous, recoveryTransition);
    try {
      await sendTransitionAlert(alerts, current, recoveryTransition, current.alertEventKey);
      current.alertEventKey = null;
    } catch (error) {
      console.error("Stationhead transition alert failed", error instanceof Error ? error.message : String(error));
      current.alertPending = downTransition;
      current.recoveryPending = recoveryTransition;
    }
  }

  await persistHealth(env, current, previousRow);
  return current;
}

export function stationheadHealthPayload(state: StateRow): Record<string, unknown> {
  let value: Record<string, unknown>;
  try {
    const parsed = JSON.parse(state.payload) as unknown;
    value = parsed && typeof parsed === "object" && !Array.isArray(parsed)
      ? parsed as Record<string, unknown>
      : {};
  } catch {
    value = {};
  }
  if (state.status !== "ok") {
    value.healthy = false;
    value.monitorStatus = state.status;
    value.reason = state.error || value.reason || "Stationhead health monitor is unavailable";
    return value;
  }
  if (typeof value.healthy !== "boolean") {
    value.healthy = false;
    value.monitorStatus = "error";
    value.reason = "Stored Stationhead health payload is invalid";
  }
  return value;
}
