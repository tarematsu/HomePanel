import type { Env } from "./sources";

const BUNDLE_PATH = /^\/v1\/radar\/bundle\/(\d{14})\.hpb$/;
const R2_LATEST_BUNDLE_KEY = "radar-bundles/v1/latest.hpb";

interface CloudflareCacheStorage extends CacheStorage {
  default: Cache;
}

function defaultCache(): Cache {
  return (caches as CloudflareCacheStorage).default;
}

function bundleResponse(body: BodyInit, byteLength: number, recordCount: number): Response {
  return new Response(body, {
    headers: {
      "Content-Type": "application/vnd.homepanel.radar-bundle",
      "Cache-Control": "public, max-age=1800, immutable",
      "Content-Length": String(byteLength),
      "X-HomePanel-Radar-Records": String(recordCount),
    },
  });
}

export async function cachedRadarBundleResponse(
  request: Request,
  env: Env,
  ctx: ExecutionContext,
): Promise<Response | null> {
  const match = new URL(request.url).pathname.match(BUNDLE_PATH);
  if (!match) return null;

  const cache = defaultCache();
  const cacheKey = new Request(request.url, { method: "GET" });
  try {
    const cached = await cache.match(cacheKey);
    if (cached) return cached;
  } catch (error) {
    console.error("radar bundle edge cache read failed", error instanceof Error ? error.message : String(error));
  }

  const bucket = env.DATA_BUCKET;
  if (!bucket) return null;
  try {
    const object = await bucket.get(R2_LATEST_BUNDLE_KEY);
    if (!object) return null;
    const storedBaseTime = object.customMetadata?.baseTime;
    const recordCount = Number(object.customMetadata?.records);
    if (storedBaseTime !== match[1] || !Number.isSafeInteger(recordCount) || recordCount <= 0 || object.size <= 0) {
      await object.body.cancel();
      return null;
    }
    const response = bundleResponse(object.body, object.size, recordCount);
    ctx.waitUntil(cache.put(cacheKey, response.clone()).catch((error: unknown) => {
      console.error("radar bundle edge cache write failed", error instanceof Error ? error.message : String(error));
    }));
    return response;
  } catch (error) {
    console.error("radar bundle R2 fast-path read failed", error instanceof Error ? error.message : String(error));
    return null;
  }
}
