import { authorizedDevice, deviceIdFromRequest } from "./auth";
import { buildDeviceSyncPayloadForDevice } from "./device_sync";
import { queueSchedulerWatchdog } from "./scheduler_coordinator";
import type { Env } from "./sources";
import { applyCompactTelemetryInput } from "./telemetry_compact";

const EXCHANGE_MAGIC = new TextEncoder().encode("HPEX0001");
const ENCODER = new TextEncoder();

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

async function applyTelemetry(
  env: Env,
  deviceId: string,
  telemetry: unknown,
  payload: Record<string, unknown>,
): Promise<void> {
  const result = await applyCompactTelemetryInput(telemetry, env, deviceId);
  if (result.status === 200) {
    payload.telemetry = result.body;
    return;
  }
  payload.telemetryError = { status: result.status, detail: result.body };
}

export async function deviceExchangeResponse(
  request: Request,
  env: Env,
  ctx: ExecutionContext,
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

  // A valid native poll is the deployment bootstrap/watchdog signal. The
  // module-level throttle keeps this to at most once per day per isolate.
  queueSchedulerWatchdog(env, ctx);

  // Merge telemetry first. The R2 merge primes the environment cache, so the
  // following sync read reuses the same row and can return the new sample in
  // this response instead of one native polling cycle later.
  const telemetryPayload: Record<string, unknown> = {};
  if (input.telemetry !== undefined) {
    await applyTelemetry(env, deviceId, input.telemetry, telemetryPayload);
  }
  const payload = await buildDeviceSyncPayloadForDevice(env, deviceId, versions);
  Object.assign(payload, telemetryPayload);

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
