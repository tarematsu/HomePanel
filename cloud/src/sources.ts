import { fetchText } from "./http";
import { fetchOctopus } from "./octopus_source";
import { fetchRadar } from "./radar_source";
import { fetchStationhead } from "./spotify_source";

export interface Env {
  DB: D1Database;
  DATA_BUCKET?: R2Bucket;
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
  STATIONHEAD_HEALTH_URL?: string;
  STATIONHEAD_HEALTH_STALE_MS?: string;
  STATIONHEAD_ALERT_TO?: string;
  STATIONHEAD_ALERT_FROM?: string;
  RESEND_API_KEY?: string;
  RADAR_CENTER_LAT?: string;
  RADAR_CENTER_LON?: string;
  RADAR_ZOOM?: string;
  GITHUB_RADAR_DISPATCH_TOKEN?: string;
}

export type StateStatus = "ok" | "stale" | "error";

export interface SourceResult {
  source: string;
  payload: unknown;
  observedAt: number;
}

export const JST_MS = 9 * 60 * 60 * 1000;

function twoDigits(value: number): string {
  return value < 10 ? `0${value}` : String(value);
}

export function jstDayKey(timestampMs: number): string {
  const date = new Date(timestampMs + JST_MS);
  return `${date.getUTCFullYear()}-${twoDigits(date.getUTCMonth() + 1)}-${twoDigits(date.getUTCDate())}`;
}

function numberOrNull(value: unknown): number | null {
  if (value === null || value === undefined || value === "") return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

const SCRIPT_HTML = /<script[\s\S]*?<\/script>/gi;
const STYLE_HTML = /<style[\s\S]*?<\/style>/gi;
const HTML_TAG = /<[^>]+>/g;
const HTML_SPACE = /\s+/g;
const WEATHER_GROUP = /<div class="wTable__group">([\s\S]*?)(?=<div class="wTable__group">|$)/gi;
const WEATHER_ROW = /<div class="wTable__row">([\s\S]*?)<\/div>\s*(?=<div class="wTable__row">|<\/div>)/gi;
const WEATHER_DAY = /class="wTable__item">(\d{1,2})日/;
const WEATHER_HOUR = /class="wTable__item time">(\d{1,2})</;
const WEATHER_ICON = /wxicon\/(\d+)\.png/i;
const WEATHER_RAIN = /class="wTable__item r">(-?\d+(?:\.\d+)?)/;
const WEATHER_TEMP = /class="wTable__item t">(-?\d+(?:\.\d+)?)/;
const WEATHER_POP = /class="wTable__item (?:p|pop)">\s*(\d{1,3})/i;
const WEATHER_POP_TEXT = /(?:降水確率\s*)?(\d{1,3})\s*%/;
const NEWS_ITEM = /<item>([\s\S]*?)<\/item>/gi;
const NEWS_TITLE = /<title[^>]*>([\s\S]*?)<\/title>/i;
const NEWS_DESCRIPTION = /<description[^>]*>([\s\S]*?)<\/description>/i;
const NEWS_LINK = /<link[^>]*>([\s\S]*?)<\/link>/i;
const NEWS_PUBLISHED = /<pubDate[^>]*>([\s\S]*?)<\/pubDate>/i;

function stripHtml(value: string): string {
  return value
    .replace(SCRIPT_HTML, " ")
    .replace(STYLE_HTML, " ")
    .replace(HTML_TAG, " ")
    .replace(/&nbsp;/g, " ")
    .replace(/&amp;/g, "&")
    .replace(/&#39;|&apos;/g, "'")
    .replace(/&quot;/g, '"')
    .replace(HTML_SPACE, " ")
    .trim();
}

function parseWeatherNews(html: string, now = new Date()): { forecastDate: string; hourly: Record<string, unknown> } {
  const bodyStart = html.indexOf('id="flick_list"');
  if (bodyStart < 0) throw new Error("WeatherNews hourly table was not found");
  const jst = new Date(now.getTime() + JST_MS);
  const nextJst = new Date(jst.getTime() + 86_400_000);
  const startDay = jst.getUTCDate();
  const endDay = nextJst.getUTCDate();
  const forecastDate = `${jst.getUTCMonth() + 1}/${startDay}〜${nextJst.getUTCMonth() + 1}/${endDay}`;
  const hourly: Record<string, unknown> = {};
  let hourlyCount = 0;

  WEATHER_GROUP.lastIndex = bodyStart;
  for (let groupMatch = WEATHER_GROUP.exec(html); groupMatch; groupMatch = WEATHER_GROUP.exec(html)) {
    const content = groupMatch[1] ?? "";
    const day = numberOrNull(WEATHER_DAY.exec(content)?.[1]);
    const target = day === startDay ? 1 : day === endDay ? 2 : 0;
    if (!target) continue;

    WEATHER_ROW.lastIndex = 0;
    for (let rowMatch = WEATHER_ROW.exec(content); rowMatch; rowMatch = WEATHER_ROW.exec(content)) {
      const row = rowMatch[1] ?? "";
      const hour = numberOrNull(WEATHER_HOUR.exec(row)?.[1]);
      if (hour === null || (target === 1 ? hour < 22 : hour > 9)) continue;
      const icon = WEATHER_ICON.exec(row)?.[1] ?? "";
      const rainMm = numberOrNull(WEATHER_RAIN.exec(row)?.[1]);
      const temp = numberOrNull(WEATHER_TEMP.exec(row)?.[1]);
      const explicitMatch = WEATHER_POP.exec(row)?.[1];
      const explicitPop = numberOrNull(
        explicitMatch ?? WEATHER_POP_TEXT.exec(stripHtml(row))?.[1],
      );
      const pop = explicitPop ?? (rainMm !== null && rainMm > 0 ? 100 : icon.startsWith("3") ? 60 : 10);
      const key = String(hour);
      if (!Object.prototype.hasOwnProperty.call(hourly, key)) hourlyCount += 1;
      hourly[key] = { pop: Math.max(0, Math.min(100, pop)), rainMm, temp, humidity: null, icon };
    }
  }

  WEATHER_GROUP.lastIndex = 0;
  WEATHER_ROW.lastIndex = 0;
  if (!hourlyCount) throw new Error("WeatherNews hourly rows were not found");
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

function rssValue(body: string, pattern: RegExp): string {
  return stripHtml(pattern.exec(body)?.[1] ?? "");
}

export async function fetchNews(): Promise<SourceResult> {
  const url = "https://news.web.nhk/n-data/conf/na/rss/cat0.xml";
  const xml = await fetchText(url);
  const items: Array<{ title: string; description: string; link: string; publishedAt: string }> = [];
  NEWS_ITEM.lastIndex = 0;
  let inspected = 0;
  for (let match = NEWS_ITEM.exec(xml); match && inspected < 10; match = NEWS_ITEM.exec(xml)) {
    inspected += 1;
    const body = match[1] ?? "";
    const title = rssValue(body, NEWS_TITLE);
    if (!title) continue;
    items.push({
      title,
      description: rssValue(body, NEWS_DESCRIPTION),
      link: rssValue(body, NEWS_LINK),
      publishedAt: rssValue(body, NEWS_PUBLISHED),
    });
  }
  NEWS_ITEM.lastIndex = 0;
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
