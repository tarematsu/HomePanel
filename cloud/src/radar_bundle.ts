import { readState } from "./snapshot";
import { radarTileTargetForPath } from "./radar_tile";
import type { Env } from "./sources";

const BUNDLE_PATH = /^\/v1\/radar\/bundle\/(\d{14})\.hpb$/;
const BUNDLE_MAGIC = new TextEncoder().encode("HPRB0001");
const MAX_PATHS_PER_SHARD = 20;
const MAX_TOTAL_PATHS = 256;
const MAX_UPSTREAM_CONCURRENCY = 4;
const MAX_SHARD_CONCURRENCY = 4;
const MAX_BUNDLE_BYTES = 16 * 1024 * 1024;
const PATH_ENCODER = new TextEncoder();

interface BundleEnv extends Env {
  SCHEDULER_COORDINATOR?: DurableObjectNamespace;
}

interface CloudflareCacheStorage extends CacheStorage {
  default: Cache;
}

interface BufferedRecord {
  header: Uint8Array;
  body: Uint8Array;
}

function defaultCache(): Cache {
  return (caches as CloudflareCacheStorage).default;
}

function writeUint16(target: Uint8Array, offset: number, value: number): void {
  target[offset] = value & 0xff;
  target[offset + 1] = value >>> 8 & 0xff;
}

function writeUint32(target: Uint8Array, offset: number, value: number): void {
  target[offset] = value & 0xff;
  target[offset + 1] = value >>> 8 & 0xff;
  target[offset + 2] = value >>> 16 & 0xff;
  target[offset + 3] = value >>> 24 & 0xff;
}

function bundleHeader(recordCount: number): Uint8Array {
  const header = new Uint8Array(BUNDLE_MAGIC.length + 4);
  header.set(BUNDLE_MAGIC);
  writeUint32(header, BUNDLE_MAGIC.length, recordCount);
  return header;
}

function recordHeader(pathname: string, bodyLength: number): Uint8Array {
  const path = PATH_ENCODER.encode(pathname);
  if (!path.length || path.length > 0xffff) throw new Error("radar bundle path is too long");
  if (!Number.isSafeInteger(bodyLength) || bodyLength <= 0 || bodyLength > 0xffffffff) {
    throw new Error("radar bundle tile length is invalid");
  }
  const header = new Uint8Array(6 + path.length);
  writeUint16(header, 0, path.length);
  writeUint32(header, 2, bodyLength);
  header.set(path, 6);
  return header;
}

async function mapWithConcurrency<T, R>(
  values: readonly T[],
  concurrency: number,
  operation: (value: T, index: number) => Promise<R>,
): Promise<R[]> {
  const results = new Array<R>(values.length);
  let nextIndex = 0;
  const workerCount = Math.min(values.length, Math.max(1, Math.trunc(concurrency)));
  await Promise.all(Array.from({ length: workerCount }, async () => {
    for (;;) {
      const index = nextIndex;
      nextIndex += 1;
      if (index >= values.length) return;
      results[index] = await operation(values[index]!, index);
    }
  }));
  return results;
}

function byteStream(prefix: Uint8Array, bodies: readonly Uint8Array[]): ReadableStream<Uint8Array> {
  return new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(prefix);
      for (const body of bodies) controller.enqueue(body);
      controller.close();
    },
  });
}

function recordStream(records: readonly BufferedRecord[]): ReadableStream<Uint8Array> {
  return new ReadableStream<Uint8Array>({
    start(controller) {
      for (const record of records) {
        controller.enqueue(record.header);
        controller.enqueue(record.body);
      }
      controller.close();
    },
  });
}

function pathnameFromTileUrl(value: unknown): string | null {
  if (typeof value !== "string" || !value.startsWith("/")) return null;
  const pathname = new URL(value, "https://radar.internal").pathname;
  if (!pathname.startsWith("/v1/radar/tile/jma/")) return null;
  return radarTileTargetForPath(pathname) ? pathname : null;
}

export function radarBundlePaths(payload: unknown, baseTime: string): string[] {
  if (!/^\d{14}$/.test(baseTime) || !payload || typeof payload !== "object") return [];
  const root = payload as Record<string, unknown>;
  if (!Array.isArray(root.frames)) return [];

  const unique = new Set<string>();
  for (const rawFrame of root.frames) {
    if (!rawFrame || typeof rawFrame !== "object") continue;
    const frame = rawFrame as Record<string, unknown>;
    if (frame.baseTime !== baseTime || !Array.isArray(frame.tiles)) continue;
    for (const rawTile of frame.tiles) {
      if (!rawTile || typeof rawTile !== "object") continue;
      const pathname = pathnameFromTileUrl((rawTile as Record<string, unknown>).url);
      if (pathname) unique.add(pathname);
    }
  }
  return unique.size <= MAX_TOTAL_PATHS ? [...unique] : [];
}

async function bufferedTileRecord(pathname: string): Promise<BufferedRecord> {
  const target = radarTileTargetForPath(pathname);
  if (!target || !pathname.startsWith("/v1/radar/tile/jma/")) {
    throw new Error("invalid radar tile path");
  }
  const response = await fetch(target.upstream, {
    headers: { "User-Agent": "HomePanel-Cloud/2.6" },
    cf: { cacheEverything: true, cacheTtl: target.ttl },
  });
  if (!response.ok) {
    await response.body?.cancel();
    throw new Error(`radar tile ${pathname} failed: HTTP ${response.status}`);
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (!bytes.length) throw new Error(`radar tile ${pathname} was empty`);
  return { header: recordHeader(pathname, bytes.length), body: bytes };
}

export async function radarBundleShardResponse(request: Request, env: Env): Promise<Response> {
  void env;
  let parsed: unknown;
  try {
    parsed = await request.json();
  } catch {
    return Response.json({ error: "invalid_json" }, { status: 400 });
  }
  const paths = parsed && typeof parsed === "object" && Array.isArray((parsed as { paths?: unknown }).paths)
    ? (parsed as { paths: unknown[] }).paths
    : [];
  if (!paths.length || paths.length > MAX_PATHS_PER_SHARD || paths.some(path => typeof path !== "string")) {
    return Response.json({ error: "invalid_paths" }, { status: 400 });
  }

  try {
    const records = await mapWithConcurrency(
      paths as string[],
      MAX_UPSTREAM_CONCURRENCY,
      pathname => bufferedTileRecord(pathname),
    );
    return new Response(recordStream(records), {
      headers: { "Content-Type": "application/octet-stream" },
    });
  } catch (error) {
    console.error("radar bundle shard failed", error instanceof Error ? error.message : String(error));
    return Response.json({ error: "tile_fetch_failed" }, { status: 502 });
  }
}

async function fetchShardBytes(
  namespace: DurableObjectNamespace,
  chunk: string[],
  index: number,
): Promise<Uint8Array> {
  const stub = namespace.get(namespace.idFromName(`radar-bundle-${index}`));
  const response = await stub.fetch("https://scheduler.internal/radar-bundle-shard", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ paths: chunk }),
  });
  if (!response.ok) {
    await response.body?.cancel();
    throw new Error(`radar bundle shard ${index} failed: HTTP ${response.status}`);
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (!bytes.length) throw new Error(`radar bundle shard ${index} was empty`);
  return bytes;
}

export async function radarBundleResponseForPayload(
  requestUrl: string,
  payload: unknown,
  baseTime: string,
  env: Env,
  ctx: ExecutionContext,
): Promise<Response> {
  const paths = radarBundlePaths(payload, baseTime);
  if (!paths.length) return Response.json({ error: "radar_bundle_unavailable" }, { status: 404 });
  const namespace = (env as BundleEnv).SCHEDULER_COORDINATOR;
  if (!namespace) return Response.json({ error: "radar_bundle_unavailable" }, { status: 503 });

  const cache = defaultCache();
  const cacheKey = new Request(requestUrl, { method: "GET" });
  const cached = await cache.match(cacheKey);
  if (cached) return cached;

  const chunks: string[][] = [];
  for (let offset = 0; offset < paths.length; offset += MAX_PATHS_PER_SHARD) {
    chunks.push(paths.slice(offset, offset + MAX_PATHS_PER_SHARD));
  }

  let shardBodies: Uint8Array[];
  try {
    shardBodies = await mapWithConcurrency(
      chunks,
      MAX_SHARD_CONCURRENCY,
      (chunk, index) => fetchShardBytes(namespace, chunk, index),
    );
  } catch (error) {
    console.error("radar bundle assembly failed", error instanceof Error ? error.message : String(error));
    return Response.json({ error: "radar_bundle_failed" }, { status: 502 });
  }

  const header = bundleHeader(paths.length);
  const totalBytes = shardBodies.reduce((total, bytes) => total + bytes.length, header.length);
  if (totalBytes > MAX_BUNDLE_BYTES) {
    console.error("radar bundle exceeded response limit", totalBytes);
    return Response.json({ error: "radar_bundle_too_large" }, { status: 502 });
  }

  const response = new Response(byteStream(header, shardBodies), {
    headers: {
      "Content-Type": "application/vnd.homepanel.radar-bundle",
      "Cache-Control": "public, max-age=1800, immutable",
      "Content-Length": String(totalBytes),
      "X-HomePanel-Radar-Records": String(paths.length),
    },
  });
  ctx.waitUntil(cache.put(cacheKey, response.clone()).catch((error: unknown) => {
    console.error("radar bundle cache put failed", error instanceof Error ? error.message : String(error));
  }));
  return response;
}

export async function radarBundleResponse(
  request: Request,
  env: Env,
  ctx: ExecutionContext,
): Promise<Response> {
  const match = new URL(request.url).pathname.match(BUNDLE_PATH);
  if (!match) return Response.json({ error: "not_found" }, { status: 404 });

  const state = await readState(env, "radar");
  if (!state?.payload) return Response.json({ error: "radar_unavailable" }, { status: 503 });
  let payload: unknown;
  try {
    payload = JSON.parse(state.payload);
  } catch {
    return Response.json({ error: "radar_state_invalid" }, { status: 503 });
  }
  return radarBundleResponseForPayload(request.url, payload, match[1]!, env, ctx);
}
