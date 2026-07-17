import { fetchJson } from "./http";
import type { Env, SourceResult } from "./sources";

const DEFAULT_RADAR_CENTER = { lat: 35.8923181, lon: 139.4858691 };
const DEFAULT_RADAR_ZOOM = 10;
const RADAR_HISTORY_WINDOW_MS = 60 * 60 * 1000;
const RADAR_FRAME_INTERVAL_MS = 5 * 1000;
const RADAR_FRAME_PREFIX = "radar/frames/";
const RADAR_RENDER_VERSION = "v2";
const RADAR_OUTPUT_WIDTH = 1920;
const RADAR_OUTPUT_HEIGHT = 1280;

 type TimeEntry = { basetime: string; validtime: string; elements?: string[] };
type RadarTileLayout = { x: number; y: number; destX: number; destY: number };

function jmaTimestampToMillis(value: string): number {
  if (!/^\d{14}$/.test(value)) return 0;
  return Date.UTC(
    Number(value.slice(0, 4)), Number(value.slice(4, 6)) - 1, Number(value.slice(6, 8)),
    Number(value.slice(8, 10)), Number(value.slice(10, 12)), Number(value.slice(12, 14)),
  );
}

function radarTileLayout(lat: number, lon: number, zoom: number, width: number, height: number): RadarTileLayout[] {
  const scale = 2 ** zoom;
  const worldX = (lon + 180) / 360 * scale * 256;
  const latitude = Math.max(-85.05112878, Math.min(85.05112878, lat)) * Math.PI / 180;
  const worldY = (1 - Math.asinh(Math.tan(latitude)) / Math.PI) / 2 * scale * 256;
  const left = worldX - width / 2;
  const top = worldY - height / 2;
  const minX = Math.floor(left / 256);
  const maxX = Math.floor((left + width - 1) / 256);
  const minY = Math.floor(top / 256);
  const maxY = Math.floor((top + height - 1) / 256);
  const output: RadarTileLayout[] = [];
  for (let y = minY; y <= maxY; y += 1) for (let x = minX; x <= maxX; x += 1) {
    output.push({ x, y, destX: Math.round(x * 256 - left), destY: Math.round(y * 256 - top) });
  }
  return output;
}

function envNumber(value: string | undefined, fallback: number, minimum: number, maximum: number): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.max(minimum, Math.min(maximum, parsed));
}

function coordinateKey(value: number): string {
  return value.toFixed(6).replace("-", "m").replace(".", "p");
}

function frameVariant(center: { lat: number; lon: number }, zoom: number): string {
  return `${RADAR_RENDER_VERSION}-z${zoom}-lat${coordinateKey(center.lat)}-lon${coordinateKey(center.lon)}`;
}

function frameKey(variant: string, validTime: string): string {
  return `${RADAR_FRAME_PREFIX}${variant}/${validTime}.webp`;
}

function framePath(variant: string, validTime: string): string {
  return `/v1/radar/frame/${variant}/${validTime}.webp`;
}

async function fetchImage(url: string): Promise<Response> {
  const response = await fetch(url, {
    headers: { "User-Agent": "HomePanel-Cloud/2.5" },
    cf: { cacheEverything: true, cacheTtl: 300 },
  });
  if (!response.ok || !response.body) throw new Error(`radar image HTTP ${response.status}`);
  return response;
}

async function renderFrame(
  env: Env,
  entry: TimeEntry,
  layout: RadarTileLayout[],
  zoom: number,
  variant: string,
): Promise<void> {
  const bucket = env.UPDATE_BUCKET;
  if (!bucket) throw new Error("radar R2 bucket unavailable");
  const key = frameKey(variant, entry.validtime);
  if (await bucket.head(key)) return;

  const mapResponses = await Promise.all(layout.map(tile =>
    fetchImage(`https://cyberjapandata.gsi.go.jp/xyz/pale/${zoom}/${tile.x}/${tile.y}.png`)));
  const radarResponses = await Promise.all(layout.map(tile =>
    fetchImage(`https://www.jma.go.jp/bosai/jmatile/data/nowc/${entry.basetime}/none/${entry.validtime}/surf/hrpns/${zoom}/${tile.x}/${tile.y}.png`)));
  const baseResponse = mapResponses[0]!.clone();

  let image = env.IMAGES.input(baseResponse.body!)
    .transform({ width: 480, height: 320, fit: "squeeze" });
  for (let index = 0; index < layout.length; index += 1) {
    const tile = layout[index]!;
    image = image.draw(
      env.IMAGES.input(mapResponses[index]!.body!).transform({ width: 256, height: 256, fit: "squeeze" }),
      { left: tile.destX, top: tile.destY },
    );
  }
  for (let index = 0; index < layout.length; index += 1) {
    const tile = layout[index]!;
    image = image.draw(
      env.IMAGES.input(radarResponses[index]!.body!).transform({ width: 256, height: 256, fit: "squeeze" }),
      { left: tile.destX, top: tile.destY },
    );
  }

  const output = await image
    .transform({ width: RADAR_OUTPUT_WIDTH, height: RADAR_OUTPUT_HEIGHT, fit: "squeeze" })
    .output({ format: "image/webp", quality: 80 });
  const response = output.response();
  if (!response.body) throw new Error("Cloudflare Images returned an empty radar frame");
  await bucket.put(key, response.body, {
    httpMetadata: { contentType: "image/webp", cacheControl: "public, max-age=604800, immutable" },
    customMetadata: {
      baseTime: entry.basetime,
      validTime: entry.validtime,
      variant,
    },
  });
}

async function ensureRenderedFrames(
  env: Env,
  entries: TimeEntry[],
  layout: RadarTileLayout[],
  zoom: number,
  variant: string,
): Promise<Set<string>> {
  const bucket = env.UPDATE_BUCKET;
  if (!bucket) throw new Error("radar R2 bucket unavailable");
  const ready = new Set<string>();
  const missing: TimeEntry[] = [];
  for (const entry of entries) {
    if (await bucket.head(frameKey(variant, entry.validtime))) ready.add(entry.validtime);
    else missing.push(entry);
  }
  for (let index = 0; index < missing.length; index += 2) {
    await Promise.all(missing.slice(index, index + 2).map(async entry => {
      try {
        await renderFrame(env, entry, layout, zoom, variant);
        ready.add(entry.validtime);
      } catch (error) {
        console.error(
          "radar frame render failed",
          entry.validtime,
          error instanceof Error ? error.message : String(error),
        );
      }
    }));
  }
  return ready;
}

export async function radarFrameResponse(pathname: string, env: Env): Promise<Response> {
  const match = pathname.match(/^\/v1\/radar\/frame\/([a-z0-9-]+)\/(\d{14})\.webp$/);
  if (!match || !env.UPDATE_BUCKET) return new Response(null, { status: 404 });
  const object = await env.UPDATE_BUCKET.get(frameKey(match[1]!, match[2]!));
  if (!object?.body) return new Response(null, { status: 404 });
  const headers = new Headers();
  object.writeHttpMetadata(headers);
  headers.set("Content-Type", "image/webp");
  headers.set("Cache-Control", "private, max-age=604800, immutable");
  if (object.httpEtag) headers.set("ETag", object.httpEtag);
  return new Response(object.body, { headers });
}

export async function fetchRadar(env: Env): Promise<SourceResult> {
  const observed = await fetchJson<TimeEntry[]>(
    "https://www.jma.go.jp/bosai/jmatile/data/nowc/targetTimes_N1.json",
  );
  const available = observed
    .filter(entry => entry.elements?.includes("hrpns"))
    .sort((a, b) => a.validtime.localeCompare(b.validtime));
  const current = available.at(-1);
  if (!current) throw new Error("JMA nowcast current frame is unavailable");
  const currentAt = jmaTimestampToMillis(current.validtime);
  const historyStart = currentAt - RADAR_HISTORY_WINDOW_MS;
  const sourceWidth = 480, sourceHeight = 320;
  const zoom = Math.trunc(envNumber(env.RADAR_ZOOM, DEFAULT_RADAR_ZOOM, 4, 14));
  const center = {
    lat: envNumber(env.RADAR_CENTER_LAT, DEFAULT_RADAR_CENTER.lat, -85.05112878, 85.05112878),
    lon: envNumber(env.RADAR_CENTER_LON, DEFAULT_RADAR_CENTER.lon, -180, 180),
  };
  const variant = frameVariant(center, zoom);
  const layout = radarTileLayout(center.lat, center.lon, zoom, sourceWidth, sourceHeight);
  const entries = available.filter(entry => {
    const validAt = jmaTimestampToMillis(entry.validtime);
    return validAt >= historyStart && validAt <= currentAt;
  });
  const ready = await ensureRenderedFrames(env, entries, layout, zoom, variant);
  const renderedEntries = entries.filter(entry => ready.has(entry.validtime));
  if (!renderedEntries.length) throw new Error("no rendered radar frames are available");

  const frames = renderedEntries.map(entry => ({
    baseTime: entry.basetime,
    validTime: entry.validtime,
    validAt: jmaTimestampToMillis(entry.validtime),
    tiles: [{ x: 0, y: 0, destX: 0, destY: 0, url: framePath(variant, entry.validtime) }],
  }));
  return {
    source: "radar",
    payload: {
      provider: "JMA radar rendered by Cloudflare Images and cached in R2",
      width: 256,
      height: 256,
      outputWidth: RADAR_OUTPUT_WIDTH,
      outputHeight: RADAR_OUTPUT_HEIGHT,
      center,
      zoom,
      historyWindowMs: RADAR_HISTORY_WINDOW_MS,
      frameIntervalMs: RADAR_FRAME_INTERVAL_MS,
      playbackRate: 1,
      frames,
      legend: [0, 1, 2, 4, 8, 16, 32, 64],
    },
    observedAt: Date.now(),
  };
}
