import type { Env } from "./sources";

interface StateHashCacheEntry {
  stableJson: string;
  hash: string;
}

const STATE_HASH_CACHES = new WeakMap<D1Database, Map<string, StateHashCacheEntry>>();

export async function memoizedStateHash(
  env: Env,
  source: string,
  stableJson: string,
  digest: () => Promise<string>,
): Promise<string> {
  let cache = STATE_HASH_CACHES.get(env.DB);
  if (!cache) {
    cache = new Map();
    STATE_HASH_CACHES.set(env.DB, cache);
  }
  const current = cache.get(source);
  if (current?.stableJson === stableJson) return current.hash;
  const hash = await digest();
  cache.set(source, { stableJson, hash });
  return hash;
}
