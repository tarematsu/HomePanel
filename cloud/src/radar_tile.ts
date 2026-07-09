import { json } from "./http";

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

export async function proxyRadarTile(request: Request): Promise<Response> {
  const target = tileRequest(new URL(request.url).pathname);
  if (!target) return json({ error: "invalid radar tile" }, { status: 404 });
  const upstream = await fetch(target.upstream, {
    headers: { "User-Agent": "HomePanel-Cloud/2.2" },
    cf: { cacheEverything: true, cacheTtl: target.ttl },
  });
  if (!upstream.ok || !upstream.body) return new Response(null, { status: upstream.status || 502 });
  const headers = new Headers(upstream.headers);
  headers.set("Content-Type", "image/png");
  headers.set("Cache-Control", `public, max-age=${target.ttl}`);
  headers.delete("Set-Cookie");
  return new Response(upstream.body, { status: 200, headers });
}
