import { fetchJson } from "./http";
import { signedRadarTilePath } from "./radar_tile";
import type { Env, SourceResult } from "./sources";

const DEFAULT_RADAR_CENTER = { lat: 35.8923181, lon: 139.4858691 };
const DEFAULT_RADAR_ZOOM = 10;
const RADAR_TILE_URL_LIFETIME_SECONDS = 30 * 60;
const RADAR_FORECAST_WINDOW_MS = 60 * 60 * 1000;
const RADAR_FRAME_INTERVAL_MS = 5 * 1000;
const RADAR_FRAME_PREFIX = "radar/frames/";
const RADAR_MANIFEST_KEY = "radar/manifest.json";
const RADAR_MANIFEST_MAX_AGE_MS = 2 * 60 * 60 * 1000;
const RADAR_OUTPUT_WIDTH = 1920;
const RADAR_OUTPUT_HEIGHT = 1280;
const JMA_OBSERVED_TIMES_URL = "https://www.jma.go.jp/bosai/jmatile/data/nowc/targetTimes_N1.json";
const JMA_FORECAST_TIMES_URL = "https://www.jma.go.jp/bosai/jmatile/data/nowc/targetTimes_N2.json";

export type RadarTimeEntry = { basetime: string; validtime: string; elements?: string[] };
type RadarTileLayout = { x: number; y: number; destX: number; destY: number };
type RadarManifestFrame = {
  baseTime: string;
  validTime: string;
  validAt: number;
  variant: string;
  url: string;
};

function jmaTimestampToMillis(value: string): number {
  if (!/^\d{14}$/.test(value)) return 0;
  return Date.UTC(
    Number(value.slice(0, 4)), Number(value.slice(4, 6)) - 1, Number(value.slice(6, 8)),
    Number(value.slice(8, 10)), Number(value.slice(10, 12)), Number(value.slice(12, 14)),
  );
}

export function selectRadarForecastEntries(
  observed: RadarTimeEntry[],
  forecast: RadarTimeEntry[],
): RadarTimeEntry[] {
  const observedAvailable = observed
    .filter(entry => entry.elements?.includes("hrpns") && jmaTimestampToMillis(entry.validtime) > 0)
    .sort((a, b) => a.validtime.localeCompare(b.validtime));
  const forecastAvailable = forecast
    .filter(entry => entry.elements?.includes("hrpns") && jmaTimestampToMillis(entry.validtime) > 0);
  const forecastBaseTimes = new Set(forecastAvailable.map(entry => entry.basetime));
  const current = observedAvailable
    .slice()
    .reverse()
    .find(entry => entry.basetime === entry.validtime && forecastBaseTimes.has(entry.basetime));
  if (!current) return [];

  const currentAt = jmaTimestampToMillis(current.validtime);
  const forecastEnd = currentAt + RADAR_FORECAST_WINDOW_MS;
  const futureByValidTime = new Map<string, RadarTimeEntry>();
  for (const entry of forecastAvailable) {
    if (entry.basetime !== current.basetime) continue;
    const validAt = jmaTimestampToMillis(entry.validtime);
    if (validAt > currentAt && validAt <= forecastEnd) {
      futureByValidTime.set(entry.validtime, entry);
    }
  }
  const future = [...futureByValidTime.values()]
    .sort((a, b) => a.validtime.localeCompare(b.validtime));
  return future.length ? [current, ...future] : [];
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

function frameKey(variant: string, validTime: string): string {
  return `${RADAR_FRAME_PREFIX}${variant}/${validTime}.webp`;
}

function framePath(variant: string, validTime: string): string {
  return `/v1/radar/frame/${variant}/${validTime}.webp`;
}

function manifestFrame(value: unknown): RadarManifestFrame | null {
  if (!value || typeof value !== "object") return null;
  const item = value as Record<string, unknown>;
  const baseTime = typeof item.baseTime === "string" ? item.baseTime : "";
  const validTime = typeof item.validTime === "string" ? item.validTime : "";
  const variant = typeof item.variant === "string" ? item.variant : "";
  const url = typeof item.url === "string" ? item.url : "";
  const validAt = typeof item.validAt === "number" ? item.validAt : Number.NaN;
  if (!/^\d{14}$/.test(baseTime) || !/^\d{14}$/.test(validTime)) return null;
  if (!/^[a-z0-9-]{1,96}$/.test(variant)) return null;
  if (!Number.isFinite(validAt) || validAt <= 0) return null;
  if (url !== framePath(variant, validTime)) return null;
  return { baseTime, validTime, validAt, variant, url };
}

function manifestNumber(
  root: Record<string, unknown>,
  name: string,
  fallback: number,
  minimum: number,
  maximum: number,
): number {
  const value = root[name];
  return typeof value === "number" && Number.isFinite(value)
    ? Math.max(minimum, Math.min(maximum, value))
    : fallback;
}

async function radarFromManifest(env: Env): Promise<SourceResult | null> {
  const bucket = env.UPDATE_BUCKET;
  if (!bucket) return null;

  const object = await bucket.get(RADAR_MANIFEST_KEY);
  if (!object) return null;

  let value: unknown;
  try {
    value = JSON.parse(await object.text());
  } catch (error) {
    console.error("radar manifest parse failed", error instanceof Error ? error.message : String(error));
    return null;
  }
  if (!value || typeof value !== "object") return null;
  const root = value as Record<string, unknown>;
  const generatedAt = typeof root.generatedAt === "string" ? Date.parse(root.generatedAt) : Number.NaN;
  const currentAt = typeof root.currentAt === "number" ? root.currentAt : Number.NaN;
  const now = Date.now();
  if (!Number.isFinite(generatedAt)
      || !Number.isFinite(currentAt)
      || generatedAt > now + 5 * 60 * 1000
      || currentAt > now + 5 * 60 * 1000
      || now - generatedAt > RADAR_MANIFEST_MAX_AGE_MS
      || now - currentAt > RADAR_MANIFEST_MAX_AGE_MS) {
    return null;
  }

  const rawFrames = Array.isArray(root.frames) ? root.frames : [];
  const parsedFrames = rawFrames
    .map(manifestFrame)
    .filter((frame): frame is RadarManifestFrame => frame !== null)
    .sort((a, b) => a.validAt - b.validAt);
  const currentFrame = parsedFrames.find(frame => frame.validAt === currentAt && frame.baseTime === frame.validTime);
  if (!currentFrame) return null;
  const frames = parsedFrames.filter(frame => (
    frame.baseTime === currentFrame.baseTime
      && frame.validAt >= currentAt
      && frame.validAt <= currentAt + RADAR_FORECAST_WINDOW_MS
  ));
  if (frames.length < 2) return null;

  const centerValue = root.center;
  const centerRoot = centerValue && typeof centerValue === "object"
    ? centerValue as Record<string, unknown>
    : {};
  const center = {
    lat: manifestNumber(centerRoot, "lat", DEFAULT_RADAR_CENTER.lat, -85.05112878, 85.05112878),
    lon: manifestNumber(centerRoot, "lon", DEFAULT_RADAR_CENTER.lon, -180, 180),
  };
  const zoom = Math.trunc(manifestNumber(root, "zoom", DEFAULT_RADAR_ZOOM, 4, 14));
  const provider = typeof root.provider === "string" && root.provider.trim()
    ? root.provider.trim()
    : "JMA current radar and one-hour forecast rendered by GitHub Actions and cached in Cloudflare R2";
  const precomposed = root.precomposed === true;

  return {
    source: "radar",
    payload: {
      provider,
      precomposed,
      width: 256,
      height: 256,
      outputWidth: RADAR_OUTPUT_WIDTH,
      outputHeight: RADAR_OUTPUT_HEIGHT,
      center,
      zoom,
      forecastWindowMs: RADAR_FORECAST_WINDOW_MS,
      frameIntervalMs: RADAR_FRAME_INTERVAL_MS,
      playbackRate: 1,
      frames: frames.map(frame => ({
        baseTime: frame.baseTime,
        validTime: frame.validTime,
        validAt: frame.validAt,
        tiles: [{ x: 0, y: 0, destX: 0, destY: 0, url: frame.url }],
      })),
      legend: [0, 1, 2, 4, 8, 16, 32, 64],
    },
    observedAt: currentAt,
  };
}

async function radarFromJmaTiles(env: Env): Promise<SourceResult> {
  const [observed, forecast] = await Promise.all([
    fetchJson<RadarTimeEntry[]>(JMA_OBSERVED_TIMES_URL),
    fetchJson<RadarTimeEntry[]>(JMA_FORECAST_TIMES_URL),
  ]);
  const entries = selectRadarForecastEntries(observed, forecast);
  if (entries.length < 2) throw new Error("JMA current-to-one-hour forecast frames are unavailable");
  const currentAt = jmaTimestampToMillis(entries[0]!.validtime);
  const width = 480, height = 320;
  const zoom = Math.trunc(envNumber(env.RADAR_ZOOM, DEFAULT_RADAR_ZOOM, 4, 14));
  const center = {
    lat: envNumber(env.RADAR_CENTER_LAT, DEFAULT_RADAR_CENTER.lat, -85.05112878, 85.05112878),
    lon: envNumber(env.RADAR_CENTER_LON, DEFAULT_RADAR_CENTER.lon, -180, 180),
  };
  const layout = radarTileLayout(center.lat, center.lon, zoom, width, height);
  const expires = Math.floor(Date.now() / 1000) + RADAR_TILE_URL_LIFETIME_SECONDS;
  const frames = await Promise.all(entries.map(async entry => ({
    baseTime: entry.basetime,
    validTime: entry.validtime,
    validAt: jmaTimestampToMillis(entry.validtime),
    tiles: await Promise.all(layout.map(async tile => {
      const pathname = `/v1/radar/tile/jma/${entry.basetime}/${entry.validtime}/${zoom}/${tile.x}/${tile.y}.png`;
      return { ...tile, url: await signedRadarTilePath(env, pathname, expires) };
    })),
  })));
  return {
    source: "radar",
    payload: {
      provider: "JMA current radar and one-hour forecast via Cloudflare Cache",
      precomposed: false,
      width,
      height,
      center,
      zoom,
      forecastWindowMs: RADAR_FORECAST_WINDOW_MS,
      frameIntervalMs: RADAR_FRAME_INTERVAL_MS,
      playbackRate: 1,
      frames,
      legend: [0, 1, 2, 4, 8, 16, 32, 64],
    },
    observedAt: currentAt,
  };
}

export async function radarFrameResponse(pathname: string, env: Env): Promise<Response> {
  const match = pathname.match(/^\/v1\/radar\/frame\/([a-z0-9-]{1,96})\/(\d{14})\.webp$/);
  if (!match || !env.UPDATE_BUCKET) return new Response(null, { status: 404 });
  const object = await env.UPDATE_BUCKET.get(frameKey(match[1]!, match[2]!));
  if (!object?.body) return new Response(null, { status: 404 });
  const headers = new Headers();
  object.writeHttpMetadata(headers);
  headers.set("Content-Type", "image/webp");
  headers.set("Cache-Control", "private, max-age=10800, immutable");
  if (object.httpEtag) headers.set("ETag", object.httpEtag);
  return new Response(object.body, { headers });
}

export async function fetchRadar(env: Env): Promise<SourceResult> {
  try {
    const manifest = await radarFromManifest(env);
    if (manifest) return manifest;
  } catch (error) {
    console.error("radar manifest read failed", error instanceof Error ? error.message : String(error));
  }
  return radarFromJmaTiles(env);
}
