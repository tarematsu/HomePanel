import { authorizedAnyDevice } from "./auth";
import {
  cachedDashboard,
  cachedDashboardEtag,
  cachedMeta,
  cachedMetaEtag,
} from "./dashboard_cache";
import worker from "./worker_core";
import { etagResponse, suppliedEtags, unauthorized } from "./response";
import { radarFrameResponse } from "./radar_source";
import { WORKER_VERSION } from "./snapshot";
import type { Env } from "./sources";
import { receiveTelemetryOptimized } from "./telemetry_route";
import { queueUpdateCheckPing } from "./update_check";
import { updateFileResponse } from "./update_proxy";
import {
  spotifyAccessToken,
  spotifyCallback,
  spotifyStatus,
  startSpotifyAuthorization,
} from "./spotify_oauth";

const UPDATE_FILE_PREFIX = "/v1/update/file/";

function notModified(etag: string): Response {
  return new Response(null, {
    status: 304,
    headers: {
      ETag: etag,
      "Cache-Control": "private, max-age=0, must-revalidate",
      Vary: "Accept-Encoding",
    },
  });
}

export default {
  async fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname;
    if (request.method === "GET" && path === "/v1/health") {
      return Response.json({
        ok: true,
        d1: "unchecked",
        workerVersion: WORKER_VERSION,
        now: new Date().toISOString(),
      });
    }

    if (request.method === "GET" && path.startsWith("/v1/radar/frame/")) {
      if (!authorizedAnyDevice(request, env)) return unauthorized();
      return radarFrameResponse(path, env);
    }

    if (request.method === "POST" && path === "/v1/update/ping") {
      return Response.json({ queued: queueUpdateCheckPing(env, ctx) }, { status: 202 });
    }

    if (request.method === "GET" && path === "/v1/spotify/callback") {
      return spotifyCallback(request, env);
    }
    if (path.startsWith("/v1/spotify/")) {
      if (!authorizedAnyDevice(request, env)) return unauthorized();
      if (request.method === "POST" && path === "/v1/spotify/authorize") {
        return startSpotifyAuthorization(request, env);
      }
      if (request.method === "GET" && path === "/v1/spotify/status") {
        return spotifyStatus(request, env);
      }
      if (request.method === "GET" && path === "/v1/spotify/access-token") {
        return spotifyAccessToken(request, env);
      }
      return Response.json({ error: "not_found" }, { status: 404 });
    }

    const signedUpdateAsset = request.method === "GET"
      && path.startsWith(UPDATE_FILE_PREFIX)
      && url.searchParams.has("expires")
      && url.searchParams.has("signature");
    if (signedUpdateAsset) {
      return updateFileResponse(request, env, path.slice(UPDATE_FILE_PREFIX.length));
    }

    if (request.method === "GET" && (path === "/v1/dashboard.json" || path === "/v1/meta")) {
      if (!authorizedAnyDevice(request, env)) return unauthorized();
      const supplied = suppliedEtags(request);
      const cachedEtag = path === "/v1/dashboard.json" ? cachedDashboardEtag(env) : cachedMetaEtag(env);
      if (cachedEtag && supplied.includes(cachedEtag)) return notModified(cachedEtag);

      if (path === "/v1/dashboard.json") {
        const snapshot = await cachedDashboard(env);
        return etagResponse(request, snapshot.payload, "application/json; charset=utf-8", snapshot.content_hash!);
      }
      const meta = await cachedMeta(env);
      return etagResponse(request, meta.payload, "application/json; charset=utf-8", meta.hash);
    }

    if (request.method === "POST" && path === "/v1/telemetry") {
      return receiveTelemetryOptimized(request, env);
    }

    return worker.fetch(request, env, ctx);
  },
  scheduled(event: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    return worker.scheduled(event, env, ctx);
  },
} satisfies ExportedHandler<Env>;
