import {
  canonicalVideoKey,
  normalizeMediaHost,
  normalizeVideoUrl
} from './extractor.js';
import { createJsonKeyPayloads } from './key-payloads.js';
import { mediaHostFor } from './source-locator.js';

const MAX_BODY_BYTES = 4096;
const BLOCK_ACTION_HEADER = 'x-videoscraper-action';

export function ensurePlaybackExclusionTable() {
  return undefined;
}

export const ensureVideoBlocklistTable = ensurePlaybackExclusionTable;

export function isSameOriginPlaybackBlockRequest(request) {
  const requestUrl = new URL(request.url);
  const origin = request.headers.get('origin');
  if (!origin || origin !== requestUrl.origin) return false;

  const fetchSite = request.headers.get('sec-fetch-site');
  if (fetchSite && !['same-origin', 'none'].includes(fetchSite)) return false;
  return request.headers.get(BLOCK_ACTION_HEADER) === 'block';
}

export function normalizeExcludedMediaUrl(value, mediaHost = mediaHostFor()) {
  const host = normalizeMediaHost(mediaHost);
  const mediaUrl = normalizeVideoUrl(value, host);
  const canonicalKey = mediaUrl && canonicalVideoKey(mediaUrl, host);
  if (!mediaUrl || !canonicalKey) throw new Error('Invalid media URL');
  return { mediaUrl, canonicalKey };
}

async function readJsonBody(request) {
  const contentType = request.headers.get('content-type') || '';
  if (!contentType.toLowerCase().startsWith('application/json')) {
    throw new Error('Content-Type must be application/json');
  }

  const declaredLength = Number(request.headers.get('content-length') || 0);
  if (declaredLength > MAX_BODY_BYTES) throw new Error('Request body is too large');

  const text = await request.text();
  if (new TextEncoder().encode(text).byteLength > MAX_BODY_BYTES) {
    throw new Error('Request body is too large');
  }
  return JSON.parse(text || '{}');
}

export async function blockPlaybackMedia(env, request, options = {}) {
  const hasAdminToken = Boolean(env.ADMIN_TOKEN)
    && request.headers.get('authorization') === `Bearer ${env.ADMIN_TOKEN}`;
  if (!options.trusted && !hasAdminToken && !isSameOriginPlaybackBlockRequest(request)) {
    return { status: 403, data: { ok: false, error: 'Request rejected' } };
  }

  let body;
  try {
    body = await readJsonBody(request);
  } catch (error) {
    return { status: 400, data: { ok: false, error: String(error?.message || error) } };
  }

  let normalized;
  try {
    normalized = normalizeExcludedMediaUrl(body.mediaUrl, mediaHostFor(env));
  } catch (error) {
    return { status: 400, data: { ok: false, error: String(error?.message || error) } };
  }

  const state = await env.DB.prepare(
    `WITH active_video AS (
       SELECT video.id, video.media_url AS mediaUrl, video.canonical_key AS canonicalKey
         FROM videos AS video
        WHERE video.status = 'active'
          AND video.canonical_key = ?
        LIMIT 1
     )
     SELECT EXISTS(
              SELECT 1 FROM video_blocklist WHERE canonical_key = ?
            ) AS alreadyBlocked,
            active_video.id,
            active_video.mediaUrl,
            active_video.canonicalKey
       FROM (SELECT 1) AS singleton
       LEFT JOIN active_video ON 1 = 1`
  ).bind(normalized.canonicalKey, normalized.canonicalKey).first();

  if (Number(state?.alreadyBlocked || 0) > 0) {
    return {
      status: 200,
      data: { ok: true, alreadyBlocked: true, canonicalKey: normalized.canonicalKey }
    };
  }

  if (!state?.id) {
    return { status: 404, data: { ok: false, error: 'Video is not active' } };
  }

  const blockedAt = new Date().toISOString();
  const results = await env.DB.batch([
    env.DB.prepare(
      `INSERT INTO video_blocklist (canonical_key, media_url, video_id, blocked_at, reason)
       VALUES (?, ?, ?, ?, 'playback-block')
       ON CONFLICT(canonical_key) DO NOTHING`
    ).bind(state.canonicalKey, state.mediaUrl, state.id, blockedAt),
    env.DB.prepare(
      `DELETE FROM ranking_entries WHERE video_id = ?`
    ).bind(state.id)
  ]);

  if (Number(results[0]?.meta?.changes || 0) === 0) {
    return {
      status: 200,
      data: { ok: true, alreadyBlocked: true, canonicalKey: state.canonicalKey }
    };
  }

  return {
    status: 200,
    data: {
      ok: true,
      blocked: true,
      videoId: state.id,
      canonicalKey: state.canonicalKey,
      blockedAt,
      statusCountsRefreshed: true
    }
  };
}

function uniqueLookupItems(items) {
  const lookupItems = [];
  const seenKeys = new Set();
  for (const item of items) {
    const key = String(item?.key || '');
    if (!key || seenKeys.has(key)) continue;
    seenKeys.add(key);
    lookupItems.push({ key });
  }
  return lookupItems;
}

export async function filterExcludedItems(db, items) {
  const sourceItems = Array.isArray(items) ? items : [];
  if (!sourceItems.length) return { items: [], blockedCount: 0 };

  const payloads = createJsonKeyPayloads(uniqueLookupItems(sourceItems));
  if (!payloads.length) return { items: sourceItems, blockedCount: 0 };

  const statements = payloads.map((payload) => (
    db.prepare(
      `SELECT canonical_key AS canonicalKey
         FROM video_blocklist
        WHERE canonical_key IN (SELECT value FROM json_each(?))`
    ).bind(payload)
  ));
  const results = statements.length === 1
    ? [await statements[0].all()]
    : await db.batch(statements);
  const blockedKeys = new Set(
    results.flatMap((result) => result?.results || []).map((row) => row.canonicalKey)
  );

  const filtered = [];
  let blockedCount = 0;
  for (const item of sourceItems) {
    if (blockedKeys.has(item?.key)) blockedCount += 1;
    else filtered.push(item);
  }

  return { items: filtered, blockedCount };
}

export const filterBlockedItems = filterExcludedItems;
