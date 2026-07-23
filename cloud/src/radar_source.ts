import { fetchJson } from "./http";
import { prewarmRadarBundle } from "./radar_bundle_prewarm";
import { signedRadarTilePath } from "./radar_tile";
import type { Env, SourceResult } from "./sources";

const DEFAULT_RADAR_CENTER = { lat: 35.8923181, lon: 139.4858691 };
const DEFAULT_RADAR_ZOOM = 10;
const RADAR_TILE_URL_LIFETIME_SECONDS = 30 * 60;
const RADAR_FORECAST_WINDOW_MS = 60 * 60 * 1000;
const RADAR_FRAME_INTERVAL_MS = 5 * 1000;
const RADAR_FRAME_PREFIX = "radar/frames/";
const RADAR_OUTPUT_WIDTH = 1920;
const RADAR_OUTPUT_HEIGHT = 1280;
const RADAR_LEGEND = [0, 1, 2, 4, 8, 16, 32, 64] as const;
const RADAR_FRAME_PATH = /^\/v1\/radar\/frame\/([a-z0-9-]{1,96})\/(\d{14})\.webp$/;
const JMA_OBSERVED_TIMES_URL = "https://www.jma.go.jp/bosai/jmatile/data/nowc/targetTimes_N1.json";
const JMA_FORECAST_TIMES_URL = "https://www.jma.go.jp/bosai/jmatile/data/nowc/targetTimes_N2.json";

export type RadarTimeEntry = { basetime: string; validtime: string; elements?: string[] };
type RadarTileLayout = { x: number; y: number; destX: number; destY: number };

function jmaTimestampToMillis(value: string): number {
  if (value.length !== 14) return 0;
  const digits = new Uint8Array(14);
  for (let index = 0; index < 14; index += 1) {
    const digit = value.charCodeAt(index) - 48;
    if (digit < 0 || digit > 9) return 0;
    digits[index] = digit;
  }
  const pair = (at: number) => digits[at]! * 10 + digits[at + 1]!;
  const year = digits[0]! * 1000 + digits[1]! * 100 + digits[2]! * 10 + digits[3]!;
  return Date.UTC(year, pair(4) - 1, pair(6), pair(8), pair(10), pair(12));
}

function hasRadarElement(entry: RadarTimeEntry): boolean {
  return entry.elements?.includes("hrpns") === true;
}

export function selectRadarForecastEntries(
  observed: RadarTimeEntry[],
  forecast: RadarTimeEntry[],
): RadarTimeEntry[] {
  const forecastAvailable: RadarTimeEntry[] = [];
  const forecastBaseTimes = new Set<string>();
  for (const entry of forecast) {
    if (!hasRadarElement(entry) || jmaTimestampToMillis(entry.validtime) <= 0) continue;
    forecastAvailable.push(entry);
    forecastBaseTimes.add(entry.basetime);
  }

  let current: RadarTimeEntry | null = null;
  for (const entry of observed) {
    if (!hasRadarElement(entry)
        || entry.basetime !== entry.validtime
        || !forecastBaseTimes.has(entry.basetime)
        || jmaTimestampToMillis(entry.validtime) <= 0) {
      continue;
    }
    if (!current || entry.validtime.localeCompare(current.validtime) > 0) current = entry;
  }
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
  const future = Array.from(futureByValidTime.values());
  future.sort((left, right) => left.validtime.localeCompare(right.validtime));
  if (!future.length) return [];
  future.unshift(current);
  return future;
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
  const columns = maxX - minX + 1;
  const output = new Array<RadarTileLayout>(columns * (maxY - minY + 1));
  let index = 0;
  for (let y = minY; y <= maxY; y += 1) for (let x = minX; x <= maxX; x += 1) {
    output[index] = { x, y, destX: Math.round(x * 256 - left), destY: Math.round(y * 256 - top) };
    index += 1;
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

export async function radarFrameResponse(pathname: string, env: Env): Promise<Response> {
  const match = pathname.match(RADAR_FRAME_PATH);
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
  const [observed, forecast] = await Promise.all([
    fetchJson<RadarTimeEntry[]>(JMA_OBSERVED_TIMES_URL),
    fetchJson<RadarTimeEntry[]>(JMA_FORECAST_TIMES_URL),
  ]);
  const entries = selectRadarForecastEntries(observed, forecast);
  if (entries.length < 2) throw new Error("JMA current-to-one-hour forecast frames are unavailable");
  const currentAt = jmaTimestampToMillis(entries[0]!.validtime);
  const width = 480;
  const height = 320;
  const zoom = Math.trunc(envNumber(env.RADAR_ZOOM, DEFAULT_RADAR_ZOOM, 4, 14));
  const center = {
    lat: envNumber(env.RADAR_CENTER_LAT, DEFAULT_RADAR_CENTER.lat, -85.05112878, 85.05112878),
    lon: envNumber(env.RADAR_CENTER_LON, DEFAULT_RADAR_CENTER.lon, -180, 180),
  };
  const layout = radarTileLayout(center.lat, center.lon, zoom, width, height);
  const expires = Math.floor(Date.now() / 1000) + RADAR_TILE_URL_LIFETIME_SECONDS;
  const framePromises = new Array<Promise<Record<string, unknown>>>(entries.length);
  for (let entryIndex = 0; entryIndex < entries.length; entryIndex += 1) {
    const entry = entries[entryIndex]!;
    framePromises[entryIndex] = (async () => {
      const tilePromises = new Array<Promise<Record<string, unknown>>>(layout.length);
      for (let tileIndex = 0; tileIndex < layout.length; tileIndex += 1) {
        const tile = layout[tileIndex]!;
        const pathname = `/v1/radar/tile/jma/${entry.basetime}/${entry.validtime}/${zoom}/${tile.x}/${tile.y}.png`;
        tilePromises[tileIndex] = signedRadarTilePath(env, pathname, expires).then(url => ({ ...tile, url }));
      }
      return {
        baseTime: entry.basetime,
        validTime: entry.validtime,
        validAt: jmaTimestampToMillis(entry.validtime),
        tiles: await Promise.all(tilePromises),
      };
    })();
  }
  const frames = await Promise.all(framePromises);
  const payload = {
    provider: "JMA current radar and one-hour forecast via Cloudflare radar bundle",
    precomposed: false,
    bundleUrl: `/v1/radar/bundle/${entries[0]!.basetime}.hpb`,
    width,
    height,
    outputWidth: RADAR_OUTPUT_WIDTH,
    outputHeight: RADAR_OUTPUT_HEIGHT,
    center,
    zoom,
    forecastWindowMs: RADAR_FORECAST_WINDOW_MS,
    frameIntervalMs: RADAR_FRAME_INTERVAL_MS,
    playbackRate: 1,
    frames,
    legend: RADAR_LEGEND,
  };
  await prewarmRadarBundle(env, payload, entries[0]!.basetime);
  return {
    source: "radar",
    payload,
    observedAt: currentAt,
  };
}
