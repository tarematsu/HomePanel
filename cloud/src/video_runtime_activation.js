const ACTIVE_CACHE_TTL_MS = 5_000;
const activeCache = new WeakMap();

export async function readVideoRuntimeActive(env) {
  const db = env?.DB;
  if (!db || typeof db.prepare !== 'function') {
    throw new Error('video runtime activation database is unavailable');
  }
  const row = await db.prepare(
    'SELECT active FROM video_runtime_state WHERE id = 1'
  ).first();
  return Number(row?.active ?? 0) === 1;
}

export async function videoRuntimeActive(env, now = Date.now()) {
  const db = env?.DB;
  if (!db || typeof db.prepare !== 'function') return false;

  const cached = activeCache.get(db);
  if (cached && cached.expiresAt > now) return cached.active;

  try {
    const active = await readVideoRuntimeActive(env);
    activeCache.set(db, { active, expiresAt: now + ACTIVE_CACHE_TTL_MS });
    return active;
  } catch (error) {
    console.error('video-runtime-activation-check-failed', {
      error: error instanceof Error ? error.message : String(error)
    });
    activeCache.set(db, { active: false, expiresAt: now + ACTIVE_CACHE_TTL_MS });
    return false;
  }
}

export function inactiveVideoRuntimeResponse() {
  return Response.json({
    ok: false,
    error: 'Video migration has not completed',
    retryable: true
  }, {
    status: 503,
    headers: {
      'Cache-Control': 'no-store',
      'Retry-After': '60',
      'X-Content-Type-Options': 'nosniff'
    }
  });
}

export function retryInactiveVideoBatch(batch) {
  console.log('video-runtime-inactive-queue-retried', {
    messages: batch?.messages?.length || 0
  });
  if (typeof batch?.retryAll === 'function') batch.retryAll();
}
