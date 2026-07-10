import { fetchJson } from "./http";
import { signedRadarTilePath } from "./radar_tile";
import type { Env, SourceResult } from "./sources";

const DEFAULT_RADAR_CENTER = { lat: 35.8923181, lon: 139.4858691 };
const DEFAULT_RADAR_ZOOM = 10;
const RADAR_TILE_URL_LIFETIME_SECONDS = 30 * 60;

function jmaTimestampToMillis(value: string): number {
  if (!/^\d{14}$/.test(value)) return 0;
  return Date.UTC(
    Number(value.slice(0, 4)), Number(value.slice(4, 6)) - 1, Number(value.slice(6, 8)),
    Number(value.slice(8, 10)), Number(value.slice(10, 12)), Number(value.slice(12, 14)),
  );
}

function radarTileLayout(lat: number, lon: number, zoom: number, width: number, height: number): Array<{ x: number; y: number; destX: number; destY: number }> {
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
  const output: Array<{ x: number; y: number; destX: number; destY: number }> = [];
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

export async function fetchRadar(env: Env): Promise<SourceResult> {
  type TimeEntry = { basetime: string; validtime: string; elements?: string[] };
  const [observed, forecast] = await Promise.all([
    fetchJson<TimeEntry[]>("https://www.jma.go.jp/bosai/jmatile/data/nowc/targetTimes_N1.json"),
    fetchJson<TimeEntry[]>("https://www.jma.go.jp/bosai/jmatile/data/nowc/targetTimes_N2.json"),
  ]);
  const current = observed.find(entry => entry.elements?.includes("hrpns"));
  if (!current) throw new Error("JMA nowcast current frame is unavailable");
  const width = 480, height = 320;
  const zoom = Math.trunc(envNumber(env.RADAR_ZOOM, DEFAULT_RADAR_ZOOM, 4, 14));
  const center = {
    lat: envNumber(env.RADAR_CENTER_LAT, DEFAULT_RADAR_CENTER.lat, -85.05112878, 85.05112878),
    lon: envNumber(env.RADAR_CENTER_LON, DEFAULT_RADAR_CENTER.lon, -180, 180),
  };
  const layout = radarTileLayout(center.lat, center.lon, zoom, width, height);
  const entries = [current, ...forecast
    .filter(entry => entry.elements?.includes("hrpns") && entry.basetime === current.basetime && jmaTimestampToMillis(entry.validtime) > jmaTimestampToMillis(current.validtime))
    .sort((a, b) => a.validtime.localeCompare(b.validtime))]
    .slice(0, 13);
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
      provider: "JMA High-resolution Precipitation Nowcast via Cloudflare Cache",
      width,
      height,
      center,
      zoom,
      frameIntervalMs: 1000,
      playbackRate: 0.5,
      frames,
      legend: [0, 1, 2, 4, 8, 16, 32, 64],
    },
    observedAt: Date.now(),
  };
}
