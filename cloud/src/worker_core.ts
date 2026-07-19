import { adminPage } from "./admin";
import { authorizedAction, authorizedAnyDevice, authorizedDevice, deviceIdFromRequest } from "./auth";
import { json } from "./http";
import { methodNotAllowed, etagResponse, unauthorized } from "./response";
import { requestRefresh, runScheduler } from "./scheduler";
import { buildMeta, ensureDashboard, readState, sha256Hex, updateState, WORKER_VERSION } from "./snapshot";
import { constantTimeEqual } from "./crypto_cache";
import { updateFileResponse, updateManifestResponse } from "./update_proxy";
import { handleSwitchBotWebhook, webhookToken } from "./switchbot";
import {
  acknowledgeDeviceCommand,
  createDeviceCommand,
  getDeviceCommands,
  getDeviceConfig,
  putDeviceConfig,
} from "./device_control";
import { getDeviceSync } from "./device_sync";
import { proxyRadarTile } from "./radar_tile";
import type { Env } from "./sources";
import { fetchStationhead } from "./spotify_source";
import { stationheadHealthPayload } from "./stationhead_health";
import { receiveTelemetryOptimized } from "./telemetry_route";

async function dashboardJsonResponse(request: Request, env: Env): Promise<Response> {
  const snapshot = await ensureDashboard(env);
  return etagResponse(request, snapshot.payload, "application/json; charset=utf-8", snapshot.content_hash!);
}

async function stateJson(request: Request, env: Env, source: string): Promise<Response> {
  const state = await readState(env, source);
  if (!state) return json({ error: `${source} unavailable` }, { status: 503 });
  return etagResponse(request, state.payload, "application/json; charset=utf-8", state.content_hash!);
}

async function stationheadHealthState(request: Request, env: Env): Promise<Response> {
  const state = await readState(env, "stationhead_health");
  if (!state) return json({ error: "stationhead_health unavailable" }, { status: 503 });
  const payload = JSON.stringify(stationheadHealthPayload(state));
  return etagResponse(
    request,
    payload,
    "application/json; charset=utf-8",
    await sha256Hex(payload),
  );
}

async function stationheadState(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
  const state = await readState(env, "stationhead");
  if (state) return etagResponse(request, state.payload, "application/json; charset=utf-8", state.content_hash!);
  ctx.waitUntil(fetchStationhead(env)
    .then(result => updateState(env, result))
    .catch(error => console.error("Stationhead warm-up failed", error instanceof Error ? error.message : String(error))));
  return json({ configured: false, connected: false, playing: false }, { status: 503 });
}

async function route(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
  const url = new URL(request.url);
  if (url.pathname === "/admin") return request.method === "GET" ? adminPage() : methodNotAllowed(["GET"]);

  if (request.method === "GET" && url.pathname === "/v1/health") {
    return json({ ok: true, d1: "unchecked", workerVersion: WORKER_VERSION, now: new Date().toISOString() });
  }

  if (request.method === "GET" && url.pathname.startsWith("/v1/radar/tile/")) return proxyRadarTile(request, env);

  if (request.method === "GET" && url.pathname.startsWith("/v1/wx-icon/")) {
    const match = url.pathname.match(/^\/v1\/wx-icon\/(\d+)_(day|night)\.png$/);
    if (match) {
      const upstream = `https://s.yimg.jp/images/weather/general/next/size90/${match[1]}_${match[2]}.png`;
      try {
        const response = await fetch(upstream, { cf: { cacheEverything: true, cacheTtl: 86400 } } as RequestInit);
        if (!response.ok) return new Response(null, { status: 502 });
        return new Response(response.body, {
          status: 200,
          headers: {
            "Content-Type": "image/png",
            "Cache-Control": "public, max-age=86400",
            "Access-Control-Allow-Origin": "*",
          },
        });
      } catch {
        return new Response(null, { status: 502 });
      }
    }
  }

  const webhookPrefix = "/v1/switchbot/webhook/";
  if (url.pathname.startsWith(webhookPrefix)) {
    if (request.method !== "POST") return methodNotAllowed(["POST"]);
    const supplied = url.pathname.slice(webhookPrefix.length);
    const expected = await webhookToken(env);
    if (!expected || !constantTimeEqual(supplied, expected)) return json({ error: "not found" }, { status: 404 });
    return handleSwitchBotWebhook(request, env);
  }

  if (url.pathname === "/v1/device/sync") {
    if (request.method !== "GET") return methodNotAllowed(["GET"]);
    const deviceId = deviceIdFromRequest(request);
    if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
    if (!authorizedDevice(request, env, deviceId)) return unauthorized();
    return getDeviceSync(request, env);
  }

  if (url.pathname === "/v1/device/config") {
    if (!["GET", "PUT"].includes(request.method)) return methodNotAllowed(["GET", "PUT"]);
    const deviceId = deviceIdFromRequest(request);
    if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
    if (request.method === "PUT" && !authorizedAction(request, env)) return unauthorized();
    if (request.method === "GET" && !authorizedAction(request, env) && !authorizedDevice(request, env, deviceId)) {
      return unauthorized();
    }
    return request.method === "GET" ? getDeviceConfig(request, env) : putDeviceConfig(request, env);
  }

  if (url.pathname === "/v1/device/commands") {
    if (!["GET", "POST"].includes(request.method)) return methodNotAllowed(["GET", "POST"]);
    if (request.method === "POST") {
      if (!authorizedAction(request, env)) return unauthorized();
      return createDeviceCommand(request, env);
    }
    const deviceId = deviceIdFromRequest(request);
    if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
    if (!authorizedDevice(request, env, deviceId)) return unauthorized();
    return getDeviceCommands(request, env);
  }

  if (url.pathname === "/v1/device/commands/ack") {
    if (request.method !== "POST") return methodNotAllowed(["POST"]);
    const deviceId = deviceIdFromRequest(request);
    if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
    if (!authorizedDevice(request, env, deviceId)) return unauthorized();
    return acknowledgeDeviceCommand(request, env);
  }

  if (url.pathname === "/v1/update/manifest") {
    if (request.method !== "GET") return methodNotAllowed(["GET"]);
    if (!authorizedAnyDevice(request, env)) return unauthorized();
    return updateManifestResponse(request, env);
  }

  const updateFilePrefix = "/v1/update/file/";
  if (url.pathname.startsWith(updateFilePrefix)) {
    if (request.method !== "GET") return methodNotAllowed(["GET"]);
    if (!authorizedAnyDevice(request, env)) return unauthorized();
    return updateFileResponse(request, env, url.pathname.slice(updateFilePrefix.length));
  }

  if ([
    "/v1/meta",
    "/v1/dashboard.json",
    "/v1/radar",
    "/v1/switchbot",
    "/v1/stationhead",
    "/v1/stationhead-health",
  ].includes(url.pathname)) {
    if (request.method !== "GET") return methodNotAllowed(["GET"]);
    if (!authorizedAnyDevice(request, env)) return unauthorized();
    if (url.pathname === "/v1/meta") {
      const payload = JSON.stringify(await buildMeta(env));
      return etagResponse(request, payload, "application/json; charset=utf-8", await sha256Hex(payload));
    }
    if (url.pathname === "/v1/dashboard.json") return dashboardJsonResponse(request, env);
    if (url.pathname === "/v1/switchbot") return stateJson(request, env, "switchbot");
    if (url.pathname === "/v1/stationhead") return stationheadState(request, env, ctx);
    if (url.pathname === "/v1/stationhead-health") return stationheadHealthState(request, env);
    return stateJson(request, env, "radar");
  }

  if (url.pathname === "/v1/telemetry") {
    if (request.method !== "POST") return methodNotAllowed(["POST"]);
    return receiveTelemetryOptimized(request, env);
  }

  if (url.pathname === "/v1/refresh") {
    if (request.method !== "POST") return methodNotAllowed(["POST"]);
    if (!authorizedAction(request, env)) return unauthorized();
    let body: { sources?: unknown };
    try {
      body = await request.json() as { sources?: unknown };
    } catch {
      return json({ error: "invalid json" }, { status: 400 });
    }
    if (body.sources !== undefined && !Array.isArray(body.sources)) {
      return json({ error: "sources must be an array" }, { status: 400 });
    }
    const names = Array.isArray(body.sources)
      ? body.sources.filter((value): value is string => typeof value === "string")
      : undefined;
    await requestRefresh(env, names);
    ctx.waitUntil(runScheduler(env));
    return json({ queued: true }, { status: 202 });
  }

  return json({ error: "not found" }, { status: 404 });
}

export default {
  fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
    return route(request, env, ctx).catch(error => {
      console.error("request failed", error instanceof Error ? error.message : String(error));
      return json({ error: "internal error" }, { status: 500 });
    });
  },
  async scheduled(_event: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    ctx.waitUntil(runScheduler(env));
  },
} satisfies ExportedHandler<Env>;
