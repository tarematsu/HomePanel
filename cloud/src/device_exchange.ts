import { authorizedDevice, deviceIdFromRequest } from "./auth";
import { buildDeviceSyncPayload } from "./device_sync";
import type { Env } from "./sources";
import { applyCompactTelemetryInput } from "./telemetry_compact";

const EXCHANGE_MAGIC = new TextEncoder().encode("HPEX0001");
const ENCODER = new TextEncoder();
const VERSION_FIELDS = [
  "dashboard",
  "radar",
  "switchbot",
  "stationhead",
  "stationheadHealth",
  "config",
] as const;

interface DeviceExchangeInput {
  versions?: Record<string, unknown>;
  telemetry?: unknown;
}

function writeUint32(target: Uint8Array, offset: number, value: number): void {
  target[offset] = value & 0xff;
  target[offset + 1] = value >>> 8 & 0xff;
  target[offset + 2] = value >>> 16 & 0xff;
  target[offset + 3] = value >>> 24 & 0xff;
}

function exchangeBody(jsonBytes: Uint8Array): Uint8Array {
  const output = new Uint8Array(EXCHANGE_MAGIC.length + 4 + jsonBytes.length);
  output.set(EXCHANGE_MAGIC);
  writeUint32(output, EXCHANGE_MAGIC.length, jsonBytes.length);
  output.set(jsonBytes, EXCHANGE_MAGIC.length + 4);
  return output;
}

function syncRequest(request: Request, deviceId: string, versions: Record<string, unknown>): Request {
  const url = new URL("/v1/device/sync", request.url);
  url.searchParams.set("deviceId", deviceId);
  for (const field of VERSION_FIELDS) {
    const raw = Number(versions[field]);
    const value = Number.isSafeInteger(raw) && raw >= 0 ? raw : -1;
    url.searchParams.set(`${field}Version`, String(value));
  }
  return new Request(url, { method: "GET", headers: request.headers });
}

async function applyTelemetry(
  env: Env,
  deviceId: string,
  telemetry: unknown,
  payload: Record<string, unknown>,
): Promise<void> {
  const telemetryResponse = await applyCompactTelemetryInput(telemetry, env, deviceId);
  if (telemetryResponse.status === 200) {
    payload.telemetry = await telemetryResponse.json<unknown>();
    return;
  }
  let detail: unknown = null;
  try { detail = await telemetryResponse.json<unknown>(); } catch { detail = null; }
  payload.telemetryError = { status: telemetryResponse.status, detail };
}

export async function deviceExchangeResponse(
  request: Request,
  env: Env,
  _ctx: ExecutionContext,
): Promise<Response> {
  const deviceId = deviceIdFromRequest(request);
  if (!deviceId) return Response.json({ error: "valid deviceId is required" }, { status: 400 });
  if (!authorizedDevice(request, env, deviceId)) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }

  let input: DeviceExchangeInput;
  try {
    input = await request.json<DeviceExchangeInput>();
  } catch {
    return Response.json({ error: "invalid json" }, { status: 400 });
  }
  if (!input || typeof input !== "object" || Array.isArray(input)) {
    return Response.json({ error: "body must be an object" }, { status: 400 });
  }
  const versions = input.versions && typeof input.versions === "object" && !Array.isArray(input.versions)
    ? input.versions
    : {};

  const payload = await buildDeviceSyncPayload(syncRequest(request, deviceId, versions), env);
  if (input.telemetry !== undefined) await applyTelemetry(env, deviceId, input.telemetry, payload);

  // Keep the exchange hot path focused on the small state/telemetry response.
  // The native client already falls back to the authenticated radar bundle URL
  // when no bundle bytes are appended, avoiding multi-megabyte assembly inside
  // the Free-plan 10 ms stateless invocation budget.
  const jsonBytes = ENCODER.encode(JSON.stringify(payload));
  const body = exchangeBody(jsonBytes);
  return new Response(body.buffer as ArrayBuffer, {
    status: 200,
    headers: {
      "Content-Type": "application/vnd.homepanel.device-exchange",
      "Content-Length": String(body.length),
      "Cache-Control": "private, no-store",
      "X-HomePanel-Exchange-Json-Bytes": String(jsonBytes.length),
      "X-HomePanel-Exchange-Radar-Bytes": "0",
    },
  });
}
