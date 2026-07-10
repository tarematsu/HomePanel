import { readState, updateState, type StateRow } from "./snapshot";
import type { Env, SourceResult } from "./sources";

const SOURCE = "stationhead_health";
const DEFAULT_STALE_MS = 15 * 60_000;
const MIN_STALE_MS = 5 * 60_000;
const MAX_STALE_MS = 24 * 60 * 60_000;
const REQUEST_TIMEOUT_MS = 8_000;
const RESEND_ENDPOINT = "https://api.resend.com/emails";
const DEFAULT_FROM = "HomePanel Monitor <onboarding@resend.dev>";

type RemoteHealth = {
  ok?: boolean;
  collector_health_ok?: boolean;
  collector_health_stale?: boolean;
  collector_health_age_ms?: number | null;
  collector_health_stale_after_ms?: number | null;
  collector_last_run_at?: number | null;
  collector_last_success_at?: number | null;
  collector_last_error_present?: boolean;
};

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
};

function finite(value: unknown): number | null {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function positive(value: unknown, fallback: number): number {
  const number = Number(value);
  if (!Number.isFinite(number) || number <= 0) return fallback;
  return Math.max(MIN_STALE_MS, Math.min(MAX_STALE_MS, Math.trunc(number)));
}

export function stationheadHealthUrl(env: Pick<Env, "STATIONHEAD_HEALTH_URL" | "STATIONHEAD_MONITOR_URL">): string {
  const explicit = env.STATIONHEAD_HEALTH_URL?.trim();
  if (explicit) return explicit;

  const monitor = env.STATIONHEAD_MONITOR_URL?.trim();
  if (!monitor) throw new Error("STATIONHEAD_HEALTH_URL or STATIONHEAD_MONITOR_URL is not configured");
  const url = new URL(monitor);
  if (/\/api\/(?:playback|dashboard)$/.test(url.pathname)) {
    url.pathname = url.pathname.replace(/\/api\/(?:playback|dashboard)$/, "/api/health");
  } else if (/\/api\/[^/]+$/.test(url.pathname)) {
    url.pathname = url.pathname.replace(/\/api\/[^/]+$/, "/api/health");
  } else {
    url.pathname = `${url.pathname.replace(/\/$/, "")}/api/health`;
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
  const staleAfterMs = positive(payload?.collector_health_stale_after_ms, configuredStaleMs);
  const lastRunAt = finite(payload?.collector_last_run_at);
  const lastSuccessAt = finite(payload?.collector_last_success_at);
  const reportedAge = finite(payload?.collector_health_age_ms);
  const ageMs = reportedAge ?? (lastSuccessAt == null ? null : Math.max(0, now - lastSuccessAt));
  const stale = payload?.collector_health_stale === true
    || (ageMs != null && ageMs >= staleAfterMs);
  const remoteHealthy = payload?.ok !== false && payload?.collector_health_ok !== false;
  const reachable = payload !== null;
  const healthy = reachable && responseOk && remoteHealthy && !stale;
  let reason: string | null = null;
  if (!reachable) reason = "Stationhead health endpoint is unreachable";
  else if (!responseOk) reason = `Stationhead health endpoint returned HTTP ${statusCode ?? "error"}`;
  else if (payload?.collector_health_stale === true || stale) reason = "Stationhead collection is stale";
  else if (payload?.collector_health_ok === false) reason = "Stationhead collector reported unhealthy";
  else if (payload?.collector_last_error_present) reason = "Stationhead collector reported a recent error";

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
  };
}

function parsePrevious(row: StateRow | null): StationheadHealthSnapshot | null {
  if (!row) return null;
  try {
    const value = JSON.parse(row.payload) as StationheadHealthSnapshot;
    return typeof value?.healthy === "boolean" ? value : null;
  } catch {
    return null;
  }
}

function alertConfig(env: Env): { enabled: boolean; apiKey: string; to: string; from: string } {
  const apiKey = env.RESEND_API_KEY?.trim() || "";
  const to = env.STATIONHEAD_ALERT_TO?.trim() || "";
  const from = env.STATIONHEAD_ALERT_FROM?.trim() || DEFAULT_FROM;
  return { enabled: Boolean(apiKey && to), apiKey, to, from };
}

function formatJst(timestamp: number | null): string {
  if (timestamp == null) return "不明";
  return new Intl.DateTimeFormat("ja-JP", {
    timeZone: "Asia/Tokyo",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  }).format(new Date(timestamp));
}

function transitionKey(snapshot: StationheadHealthSnapshot, recovery: boolean): string {
  const anchor = snapshot.lastSuccessAt == null ? "never" : String(snapshot.lastSuccessAt);
  return `homepanel-stationhead-${recovery ? "recovered" : "down"}-${anchor}`;
}

async function sendTransitionAlert(env: Env, snapshot: StationheadHealthSnapshot, recovery: boolean): Promise<void> {
  const config = alertConfig(env);
  if (!config.enabled) return;
  const response = await fetch(RESEND_ENDPOINT, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${config.apiKey}`,
      "Content-Type": "application/json",
      "Idempotency-Key": transitionKey(snapshot, recovery),
    },
    body: JSON.stringify({
      from: config.from,
      to: [config.to],
      subject: recovery
        ? "【HomePanel】Stationhead収集が復旧しました"
        : "【HomePanel】Stationhead収集停止を検知",
      text: [
        recovery ? "Stationhead収集が正常状態へ復帰しました。" : "Stationhead収集が正常に更新されていません。",
        "",
        `確認時刻: ${formatJst(snapshot.sampledAt)}`,
        `最終実行: ${formatJst(snapshot.lastRunAt)}`,
        `最終成功: ${formatJst(snapshot.lastSuccessAt)}`,
        `状態: ${snapshot.reason || (snapshot.healthy ? "正常" : "異常")}`,
      ].join("\n"),
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
    return {
      configured: false,
      reachable: false,
      healthy: false,
      statusCode: null,
      sampledAt: now,
      lastRunAt: null,
      lastSuccessAt: null,
      ageMs: null,
      staleAfterMs: staleMs,
      reason: error instanceof Error ? error.message : String(error),
      alertConfigured: alertConfig(env).enabled,
      alertPending: false,
      recoveryPending: false,
    };
  }

  try {
    const response = await fetch(url, {
      headers: { Accept: "application/json", "Cache-Control": "no-cache, no-store" },
      cache: "no-store",
      signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
    });
    const payload = await response.json().catch(() => null) as RemoteHealth | null;
    return evaluateStationheadHealth(payload, response.ok, response.status, now, staleMs);
  } catch (error) {
    return {
      configured: true,
      reachable: false,
      healthy: false,
      statusCode: null,
      sampledAt: now,
      lastRunAt: null,
      lastSuccessAt: null,
      ageMs: null,
      staleAfterMs: staleMs,
      reason: error instanceof Error ? error.message : String(error),
      alertConfigured: alertConfig(env).enabled,
      alertPending: false,
      recoveryPending: false,
    };
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
    await env.DB.prepare(
      "UPDATE current_state SET status='stale', error=?1 WHERE source=?2",
    ).bind(snapshot.reason || "Stationhead collector is unhealthy", SOURCE).run();
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
    try {
      await sendTransitionAlert(env, current, recoveryTransition);
    } catch (error) {
      console.error("Stationhead transition alert failed", error instanceof Error ? error.message : String(error));
      current.alertPending = downTransition;
      current.recoveryPending = recoveryTransition;
    }
  }

  await persistHealth(env, current, previousRow);
  return current;
}
