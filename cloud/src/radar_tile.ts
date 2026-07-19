import { cachedHmacKey } from "./crypto_cache";
import { json } from "./http";
import type { Env } from "./sources";

const SIGNED_URL_LIFETIME_SECONDS = 30 * 60;
const UTF8_ENCODER = new TextEncoder();
const HEX_DIGITS = "0123456789abcdef";

export interface RadarTileTarget {
  upstream: string;
  ttl: number;
}

function validTileCoordinates(
  zoomText: string | undefined,
  xText: string | undefined,
  yText: string | undefined,
): { zoom: number; x: number; y: number } | null {
  const zoom = Number(zoomText);
  const x = Number(xText);
  const y = Number(yText);
  if (!Number.isSafeInteger(zoom) || zoom < 0 || zoom > 18) return null;
  if (!Number.isSafeInteger(x) || !Number.isSafeInteger(y)) return null;
  const maximum = 2 ** zoom;
  if (x < 0 || y < 0 || x >= maximum || y >= maximum) return null;
  return { zoom, x, y };
}

export function radarTileTargetForPath(pathname: string): RadarTileTarget | null {
  let match = pathname.match(/^\/v1\/radar\/tile\/gsi\/(\d{1,2})\/(\d+)\/(\d+)\.png$/);
  if (match) {
    const coordinates = validTileCoordinates(match[1], match[2], match[3]);
    if (!coordinates) return null;
    return {
      upstream: `https://cyberjapandata.gsi.go.jp/xyz/pale/${coordinates.zoom}/${coordinates.x}/${coordinates.y}.png`,
      ttl: 86_400,
    };
  }
  match = pathname.match(/^\/v1\/radar\/tile\/jma\/(\d{14})\/(\d{14})\/(\d{1,2})\/(\d+)\/(\d+)\.png$/);
  if (match) {
    const [, baseTime, validTime, zoomText, xText, yText] = match;
    const coordinates = validTileCoordinates(zoomText, xText, yText);
    if (!coordinates || !baseTime || !validTime) return null;
    return {
      upstream: `https://www.jma.go.jp/bosai/jmatile/data/nowc/${baseTime}/none/${validTime}/surf/hrpns/${coordinates.zoom}/${coordinates.x}/${coordinates.y}.png`,
      ttl: 300,
    };
  }
  return null;
}

function signingSecret(env: Env): string {
  return env.UPDATE_SIGNING_SECRET?.trim()
    || env.HOMEPANEL_INGEST_SECRET?.trim()
    || env.DEVICE_TOKEN?.trim()
    || env.HOMEPANEL_DEVICE_TOKENS?.trim()
    || "";
}

async function signatureDigest(secret: string, pathname: string, expires: number): Promise<ArrayBuffer> {
  return crypto.subtle.sign(
    "HMAC",
    await cachedHmacKey(secret),
    UTF8_ENCODER.encode(`${pathname}\n${expires}`),
  );
}

function hexDigest(digest: ArrayBuffer): string {
  const bytes = new Uint8Array(digest);
  let output = "";
  for (const value of bytes) {
    output += HEX_DIGITS.charAt(value >>> 4) + HEX_DIGITS.charAt(value & 15);
  }
  return output;
}

function digestMatchesHex(supplied: string, digest: ArrayBuffer): boolean {
  const bytes = new Uint8Array(digest);
  const expectedLength = bytes.length * 2;
  let diff = supplied.length ^ expectedLength;
  for (let index = 0; index < bytes.length; index += 1) {
    const value = bytes[index]!;
    const suppliedOffset = index * 2;
    const suppliedHigh = suppliedOffset < supplied.length ? supplied.charCodeAt(suppliedOffset) : 0;
    const suppliedLow = suppliedOffset + 1 < supplied.length ? supplied.charCodeAt(suppliedOffset + 1) : 0;
    diff |= suppliedHigh ^ HEX_DIGITS.charCodeAt(value >>> 4);
    diff |= suppliedLow ^ HEX_DIGITS.charCodeAt(value & 15);
  }
  return diff === 0;
}

async function verifyRadarTileUrl(
  url: URL,
  env: Env,
  nowSeconds: number,
  target: RadarTileTarget | null = radarTileTargetForPath(url.pathname),
): Promise<boolean> {
  if (!target) return false;
  const secret = signingSecret(env);
  if (!secret) return false;
  const expires = Number(url.searchParams.get("expires"));
  if (!Number.isSafeInteger(expires)
      || expires < nowSeconds
      || expires > nowSeconds + SIGNED_URL_LIFETIME_SECONDS + 60) {
    return false;
  }
  const supplied = url.searchParams.get("signature") ?? "";
  return digestMatchesHex(supplied, await signatureDigest(secret, url.pathname, expires));
}

export async function signedRadarTilePath(
  env: Env,
  pathname: string,
  expires = Math.floor(Date.now() / 1000) + SIGNED_URL_LIFETIME_SECONDS,
): Promise<string> {
  if (!radarTileTargetForPath(pathname)) throw new Error("invalid radar tile path");
  const secret = signingSecret(env);
  if (!secret) throw new Error("radar tile signing unavailable");
  const signed = hexDigest(await signatureDigest(secret, pathname, expires));
  return `${pathname}?expires=${expires}&signature=${signed}`;
}

export async function verifyRadarTileRequest(
  request: Request,
  env: Env,
  nowSeconds = Math.floor(Date.now() / 1000),
): Promise<boolean> {
  return verifyRadarTileUrl(new URL(request.url), env, nowSeconds);
}

export async function proxyRadarTile(request: Request, env: Env): Promise<Response> {
  const url = new URL(request.url);
  const target = radarTileTargetForPath(url.pathname);
  if (!target) return json({ error: "invalid radar tile" }, { status: 404 });
  if (!await verifyRadarTileUrl(url, env, Math.floor(Date.now() / 1000), target)) {
    return json({ error: "invalid or expired radar tile signature" }, { status: 403 });
  }
  const upstream = await fetch(target.upstream, {
    headers: { "User-Agent": "HomePanel-Cloud/2.6" },
    cf: { cacheEverything: true, cacheTtl: target.ttl },
  });
  if (!upstream.ok || !upstream.body) return new Response(null, { status: upstream.status || 502 });
  const headers = new Headers(upstream.headers);
  headers.set("Content-Type", "image/png");
  headers.set("Cache-Control", `private, max-age=${Math.min(target.ttl, SIGNED_URL_LIFETIME_SECONDS)}`);
  headers.delete("Set-Cookie");
  return new Response(upstream.body, { status: 200, headers });
}
