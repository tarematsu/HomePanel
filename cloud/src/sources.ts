import { fetchText } from "./http";
import { fetchOctopus } from "./octopus_source";
import { fetchRadar } from "./radar_source";
import { fetchStationhead } from "./spotify_source";

export interface Env {
  DB: D1Database;
  UPDATE_BUCKET?: R2Bucket;
  HOMEPANEL_INGEST_SECRET?: string;
  HOMEPANEL_DEVICE_TOKENS?: string;
  HOMEPANEL_PRIMARY_DEVICE_ID?: string;
  HOMEPANEL_PUBLIC_URL?: string;
  API_TOKEN?: string;
  DEVICE_TOKEN?: string;
  SWITCHBOT_TOKEN?: string;
  SWITCHBOT_SECRET?: string;
  OCTOPUS_EMAIL?: string;
  OCTOPUS_PASSWORD?: string;
  OCTOPUS_ACCOUNT_NUMBER?: string;
  UPDATE_SIGNING_SECRET?: string;
  UPDATE_BUCKET_PREFIX?: string;
  SPOTIFY_CLIENT_ID?: string;
  SPOTIFY_CLIENT_SECRET?: string;
  SPOTIFY_REDIRECT_URI?: string;
  SPOTIFY_TOKEN_ENCRYPTION_KEY?: string;
  CITY_NAME?: string;
  WEATHERNEWS_URL?: string;
  STATIONHEAD_MONITOR_URL?: string;
  RADAR_CENTER_LAT?: string;
  RADAR_CENTER_LON?: string;
  RADAR_ZOOM?: string;
}

export type StateStatus = "ok" | "stale" | "error";

export interface SourceResult {
  source: string;
  payload: unknown;
  observedAt: number;
}

export const JST_MS = 9 * 60 * 60 * 1000;

function numberOrNull(value: unknown): number | null {
  if (value === null || value === undefined || value === "") return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

const TARGET_HOURS = new Set([5, 6, 7, 8, 9]);

function stripHtml(value: string): string {
  return value
    .replace(/<script[\s\S]*?<\/script>/gi, " ")
    .replace(/<style[\s\S]*?<\/style>/gi, " ")
    .replace(/<[^>]+>/g, " ")
    .replace(/&nbsp;/g, " ")
    .replace(/&amp;/g, "&")
    .replace(/&#39;|&apos;/g, "'")
    .replace(/&quot;/g, '"')
    .replace(/\s+/g, " ")
    .trim();
}

function parseWeatherNews(html: string, now = new Date()): { forecastDate: string; hourly: Record<string, unknown> } {
  const bodyStart = html.indexOf('id="flick_list"');
  if (bodyStart < 0) throw new Error("WeatherNews hourly table was not found");
  let bodyEnd = html.indexOf('class="wTable day2"', bodyStart);
  if (bodyEnd < 0) bodyEnd = html.length;
  const body = html.slice(bodyStart, bodyEnd);
  const jst = new Date(now.getTime() + JST_MS);
  const targetDt = new Date(jst.getTime() + (jst.getUTCHours() >= 9 ? 86_400_000 : 0));
  const targetDay = targetDt.getUTCDate();
  const forecastDate = `${targetDt.getUTCMonth() + 1}/${targetDay}`;
  const hourly: Record<string, unknown> = {};
  const groups = [...body.matchAll(/<div class="wTable__group">([\s\S]*?)(?=<div class="wTable__group">|$)/gi)];

  for (const group of groups) {
    const content = group[1] ?? "";
    const day = numberOrNull(content.match(/class="wTable__item">(\d{1,2})日/)?.[1]);
    if (day !== targetDay) continue;
    const rows = [...content.matchAll(/<div class="wTable__row">([\s\S]*?)<\/div>\s*(?=<div class="wTable__row">|<\/div>)/gi)];
    for (const rowMatch of rows) {
      const row = rowMatch[1] ?? "";
      const hour = numberOrNull(row.match(/class="wTable__item time">(\d{1,2})</)?.[1]);
      if (hour === null || !TARGET_HOURS.has(hour)) continue;
      const icon = row.match(/wxicon\/(\d+)\.png/i)?.[1] ?? "";
      const rainMm = numberOrNull(row.match(/class="wTable__item r">(-?\d+(?:\.\d+)?)/)?.[1]);
      const temp = numberOrNull(row.match(/class="wTable__item t">(-?\d+(?:\.\d+)?)/)?.[1]);
      const explicitPop = numberOrNull(
        row.match(/class="wTable__item (?:p|pop)">\s*(\d{1,3})/i)?.[1]
        ?? stripHtml(row).match(/(?:降水確率\s*)?(\d{1,3})\s*%/)?.[1],
      );
      const wetIcon = /^(200|201|202|203|204|205|210|211|212|213|214|215|216|217|218|219|220|221|222|223|224|225)/.test(icon);
      const pop = explicitPop ?? (rainMm !== null && rainMm > 0 ? 100 : wetIcon ? 60 : 10);
      hourly[String(hour)] = { pop: Math.max(0, Math.min(100, pop)), rainMm, temp, humidity: null, icon };
    }
    break;
  }

  if (!Object.keys(hourly).length) throw new Error("WeatherNews hourly rows were not found");
  return { forecastDate, hourly };
}

export async function fetchWeather(env: Env): Promise<SourceResult> {
  const now = new Date();
  const url = env.WEATHERNEWS_URL?.trim();
  if (!url) throw new Error("WEATHERNEWS_URL is not configured");
  const { forecastDate, hourly } = parseWeatherNews(await fetchText(url), now);
  return {
    source: "weather",
    payload: {
      city: env.CITY_NAME?.trim() || "Configured location",
      forecastDate,
      hourly,
      generatedAt: now.toISOString(),
    },
    observedAt: now.getTime(),
  };
}

export async function fetchNews(): Promise<SourceResult> {
  const url = "https://news.web.nhk/n-data/conf/na/rss/cat0.xml";
  const xml = await fetchText(url);
  const items = [...xml.matchAll(/<item>([\s\S]*?)<\/item>/gi)].slice(0, 10).map(match => {
    const body = match[1] ?? "";
    const value = (tag: string): string => stripHtml(body.match(new RegExp(`<${tag}[^>]*>([\\s\\S]*?)<\\/${tag}>`, "i"))?.[1] ?? "");
    return { title: value("title"), description: value("description"), link: value("link"), publishedAt: value("pubDate") };
  }).filter(item => item.title);
  if (!items.length) throw new Error("NHK RSS contained no items");
  return { source: "news", payload: { title: "NHKニュース", items }, observedAt: Date.now() };
}

export async function executeSource(name: string, env: Env): Promise<SourceResult> {
  switch (name) {
    case "weather": return fetchWeather(env);
    case "news": return fetchNews();
    case "octopus": return fetchOctopus(env);
    case "stationhead": return fetchStationhead(env);
    case "radar": return fetchRadar(env);
    default: throw new Error(`Unknown job: ${name}`);
  }
}
