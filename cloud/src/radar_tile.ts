import { cachedHmacKey, constantTimeEqual } from "./crypto_cache";
import { json } from "./http";
import type { Env } from "./sources";

const SIGNED_URL_LIFETIME_SECONDS = 30 * 60;

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

function tileRequest(pathname: string): { upstream: string; ttl: number } | null {
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

async function signature(secret: string, pathname: string, expires: number): Promise<string> {
  const digest = await crypto.subtle.sign(
    "HMAC",
    await cachedHmacKey(secret),
    new TextEncoder().encode(`${pathname}\n${expires}`),
  );
  return [...new Uint8Array(digest)].map(value => value.toString(16).padStart(2, "0")).join("");
}

export async function signedRadarTilePath(
  env: Env,
  pathname: string,
  expires = Math.floor(Date.now() / 1000) + SIGNED_URL_LIFETIME_SECONDS,
): Promise<string> {
  if (!tileRequest(pathname)) throw new Error("invalid radar tile path");
  const secret = signingSecret(env);
  if (!secret) throw new Error("radar tile signing unavailable");
  const signed = await signature(secret, pathname, expires);
  return `${pathname}?expires=${expires}&signature=${signed}`;
}

export async function verifyRadarTileRequest(
  request: Request,
  env: Env,
  nowSeconds = Math.floor(Date.now() / 1000),
): Promise<boolean> {
  const url = new URL(request.url);
  if (!tileRequest(url.pathname)) return false;
  const secret = signingSecret(env);
  if (!secret) return false;
  const expires = Number(url.searchParams.get("expires"));
  if (!Number.isSafeInteger(expires)
      || expires < nowSeconds
      || expires > nowSeconds + SIGNED_URL_LIFETIME_SECONDS + 60) {
    return false;
  }
  const supplied = url.searchParams.get("signature") ?? "";
  const expected = await signature(secret, url.pathname, expires);
  return constantTimeEqual(supplied, expected);
}

export async function proxyRadarTile(request: Request, env: Env): Promise<Response> {
  const url = new URL(request.url);
  const target = tileRequest(url.pathname);
  if (!target) return json({ error: "invalid radar tile" }, { status: 404 });
  if (!await verifyRadarTileRequest(request, env)) {
    return json({ error: "invalid or expired radar tile signature" }, { status: 403 });
  }
  const upstream = await fetch(target.upstream, {
    headers: { "User-Agent": "HomePanel-Cloud/2.2" },
    cf: { cacheEverything: true, cacheTtl: target.ttl },
  });
  if (!upstream.ok || !upstream.body) return new Response(null, { status: upstream.status || 502 });
  const headers = new Headers(upstream.headers);
  headers.set("Content-Type", "image/png");
  headers.set("Cache-Control", `private, max-age=${Math.min(target.ttl, SIGNED_URL_LIFETIME_SECONDS)}`);
  headers.delete("Set-Cookie");
  return new Response(upstream.body, { status: 200, headers });
}
