import { constantTimeEqual } from "./crypto_cache";
import type { Env } from "./sources";

export const DEVICE_ID_PATTERN = /^[A-Za-z0-9._-]{1,100}$/;



const deviceTokenMapCache = new Map<string, Map<string, string>>();

function parseMappedDeviceTokens(raw: string): Map<string, string> {
  const cached = deviceTokenMapCache.get(raw);
  if (cached) return cached;
  let parsed: Map<string, string>;
  try {
    const value = JSON.parse(raw) as unknown;
    parsed = !value || typeof value !== "object" || Array.isArray(value)
      ? new Map()
      : new Map(
        Object.entries(value as Record<string, unknown>)
          .filter((entry): entry is [string, string] =>
            DEVICE_ID_PATTERN.test(entry[0]) && typeof entry[1] === "string" && entry[1].trim().length > 0)
          .map(([deviceId, token]) => [deviceId, token.trim()] as const),
      );
  } catch {
    parsed = new Map();
  }
  deviceTokenMapCache.set(raw, parsed);
  return parsed;
}

export function bearerToken(request: Request): string {
  const authorization = request.headers.get("Authorization") ?? "";
  return authorization.startsWith("Bearer ") ? authorization.slice(7) : "";
}

export function configuredDeviceTokens(env: Env): Map<string, string> | null {
  const raw = env.HOMEPANEL_DEVICE_TOKENS?.trim() ?? "";
  return raw ? parseMappedDeviceTokens(raw) : null;
}

export function deviceIdFromRequest(request: Request): string | null {
  const deviceId = new URL(request.url).searchParams.get("deviceId")?.trim() ?? "";
  return DEVICE_ID_PATTERN.test(deviceId) ? deviceId : null;
}

export function matchesAnyToken(supplied: string, expected: Array<string | undefined>): boolean {
  return Boolean(supplied) && expected.some(value => Boolean(value) && constantTimeEqual(supplied, value!.trim()));
}

export function deviceSecrets(env: Env): Array<string | undefined> {
  return [env.HOMEPANEL_INGEST_SECRET, env.DEVICE_TOKEN];
}




export function actionSecrets(env: Env): Array<string | undefined> {
  return [env.API_TOKEN, env.HOMEPANEL_INGEST_SECRET, env.DEVICE_TOKEN];
}

export function authorizedAnyDevice(request: Request, env: Env): boolean {
  const supplied = bearerToken(request);
  const configured = configuredDeviceTokens(env);
  return configured
    ? [...configured.values()].some(expected => constantTimeEqual(supplied, expected))
    : matchesAnyToken(supplied, deviceSecrets(env));
}

export function authorizedDevice(request: Request, env: Env, deviceId: string): boolean {
  const supplied = bearerToken(request);
  const configured = configuredDeviceTokens(env);
  return configured
    ? Boolean(configured.get(deviceId)) && constantTimeEqual(supplied, configured.get(deviceId)!)
    : matchesAnyToken(supplied, deviceSecrets(env));
}

export function authorizedAction(request: Request, env: Env): boolean {
  return matchesAnyToken(bearerToken(request), actionSecrets(env));
}
