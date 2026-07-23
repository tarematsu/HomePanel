import { radarBundlePaths } from "./radar_bundle";
import type { Env } from "./sources";

const BUNDLE_MAGIC = new TextEncoder().encode("HPRB0001");
const MAX_PATHS_PER_SHARD = 15;
const MAX_SHARD_CONCURRENCY = 4;
const MAX_BUNDLE_BYTES = 16 * 1024 * 1024;
const R2_LATEST_BUNDLE_KEY = "radar-bundles/v1/latest.hpb";

interface BundleEnv extends Env {
  SCHEDULER_COORDINATOR?: DurableObjectNamespace;
}

function writeUint32(target: Uint8Array, offset: number, value: number): void {
  target[offset] = value & 0xff;
  target[offset + 1] = value >>> 8 & 0xff;
  target[offset + 2] = value >>> 16 & 0xff;
  target[offset + 3] = value >>> 24 & 0xff;
}

async function mapWithConcurrency<T, R>(
  values: readonly T[],
  concurrency: number,
  operation: (value: T, index: number) => Promise<R>,
): Promise<R[]> {
  const results = new Array<R>(values.length);
  let nextIndex = 0;
  const workerCount = Math.min(values.length, Math.max(1, Math.trunc(concurrency)));
  await Promise.all(Array.from({ length: workerCount }, async () => {
    for (;;) {
      const index = nextIndex;
      nextIndex += 1;
      if (index >= values.length) return;
      results[index] = await operation(values[index]!, index);
    }
  }));
  return results;
}

async function fetchShard(
  namespace: DurableObjectNamespace,
  paths: string[],
  index: number,
): Promise<Uint8Array> {
  const stub = namespace.get(namespace.idFromName(`radar-bundle-${index}`));
  const response = await stub.fetch("https://scheduler.internal/radar-bundle-shard", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ paths }),
  });
  if (!response.ok) {
    await response.body?.cancel();
    throw new Error(`radar bundle prewarm shard ${index} failed: HTTP ${response.status}`);
  }
  const expectedLength = Number(
    response.headers.get("X-HomePanel-Radar-Shard-Bytes")
    ?? response.headers.get("Content-Length"),
  );
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (!bytes.length || !Number.isSafeInteger(expectedLength) || expectedLength !== bytes.length) {
    throw new Error(`radar bundle prewarm shard ${index} length is invalid`);
  }
  return bytes;
}

export async function prewarmRadarBundle(
  env: Env,
  payload: unknown,
  baseTime: string,
): Promise<boolean> {
  const namespace = (env as BundleEnv).SCHEDULER_COORDINATOR;
  if (!env.DATA_BUCKET || !namespace) return false;

  const paths = radarBundlePaths(payload, baseTime);
  if (!paths.length) throw new Error("radar bundle prewarm paths are unavailable");

  const chunks: string[][] = [];
  for (let offset = 0; offset < paths.length; offset += MAX_PATHS_PER_SHARD) {
    chunks.push(paths.slice(offset, offset + MAX_PATHS_PER_SHARD));
  }
  const shards = await mapWithConcurrency(chunks, MAX_SHARD_CONCURRENCY, (chunk, index) => (
    fetchShard(namespace, chunk, index)
  ));

  const header = new Uint8Array(BUNDLE_MAGIC.length + 4);
  header.set(BUNDLE_MAGIC);
  writeUint32(header, BUNDLE_MAGIC.length, paths.length);
  const totalBytes = shards.reduce((total, shard) => total + shard.length, header.length);
  if (totalBytes > MAX_BUNDLE_BYTES) throw new Error(`radar bundle prewarm exceeded ${MAX_BUNDLE_BYTES} bytes`);

  const bundle = new Uint8Array(totalBytes);
  bundle.set(header);
  let offset = header.length;
  for (const shard of shards) {
    bundle.set(shard, offset);
    offset += shard.length;
  }

  await env.DATA_BUCKET.put(R2_LATEST_BUNDLE_KEY, bundle, {
    httpMetadata: { contentType: "application/vnd.homepanel.radar-bundle" },
    customMetadata: {
      baseTime,
      records: String(paths.length),
    },
  });
  return true;
}
