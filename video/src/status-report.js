import {
  buildLivenessStatus,
  prepareLivenessStateRead
} from './liveness-monitor.js';
import {
  LIVENESS_INTERVAL_SECONDS,
  LIVENESS_SCHEDULE
} from './liveness-schedule.js';
import {
  emptyStatusCounts,
  prepareStatusCountsRead
} from './status-counts.js';
import {
  buildManualImportSiteStatus,
  manualImportRunsStatement
} from './manual-import-status.js';

const statusReportInFlight = new WeakMap();

export function needsStatusCountRefresh(countRow) {
  return !countRow?.countsUpdatedAt || Number(countRow.countsDirty || 0) > 0;
}

async function buildStatusReport(env) {
  const [counts, manualRuns, state] = await env.DB.batch([
    prepareStatusCountsRead(env.DB),
    manualImportRunsStatement(env.DB),
    prepareLivenessStateRead(env.DB)
  ]);
  const countRow = counts?.results?.[0] || emptyStatusCounts();
  const livenessState = state?.results?.[0] || null;
  const countsStale = needsStatusCountRefresh(countRow);
  if (countsStale) {
    console.warn('status-counts-stale-deferred-to-cleanup', {
      updatedAt: countRow.countsUpdatedAt || null
    });
  }

  const manualRows = manualRuns?.results || [];
  const manualSiteSummary = buildManualImportSiteStatus(manualRows);
  const liveness = buildLivenessStatus(
    livenessState,
    Number(countRow.deathVideos || 0)
  );
  const unhealthySites = [];
  for (const key in manualSiteSummary.sites) {
    const status = manualSiteSummary.sites[key].status;
    if (status !== 'ok') unhealthySites.push({ key, status });
  }
  const collectionHealthy = unhealthySites.length === 0;

  const excludedCount = Number(countRow.blockedVideos || 0);
  const data = {
    ok: collectionHealthy && !countsStale,
    mode: 'manual-import-site-stats',
    automaticCollection: false,
    schedules: {},
    counts: {
      ...countRow,
      stale: countsStale,
      repair: countsStale ? 'daily-cleanup' : null
    },
    collectionHealth: {
      healthy: collectionHealthy,
      status: collectionHealthy ? 'ok' : 'degraded',
      unhealthySites
    },
    storagePolicy: {
      storedUrlLimit: null,
      deduplication: 'media-host-and-path',
      storedCountField: 'counts.activeVideos',
      playbackFeedLimit: 2000,
      playbackFeedCountField: 'counts.feedVideos',
      note: 'Manual imports are grouped by sourceUrl hostname; playback feed remains a separate recent-item window.'
    },
    ...manualSiteSummary
  };

  data.playbackExclusions = {
    count: excludedCount,
    type: 'manual-playback-exclusion-list',
    behavior: 'hidden-from-playback-and-skipped-during-persistence',
    publicSummaryEndpoint: '/api/status/exclusions',
    detailsEndpoint: '/api/admin/status/exclusions',
    detailsRequireAdminToken: true
  };
  data.deathList = {
    ...liveness,
    schedule: LIVENESS_SCHEDULE,
    intervalSeconds: LIVENESS_INTERVAL_SECONDS,
    type: 'automatic-liveness-death-list',
    behavior: 'excluded-from-playback-and-collection-until-revived'
  };
  return data;
}

export function readStatusReport(env) {
  const db = env?.DB;
  if (!db || (typeof db !== 'object' && typeof db !== 'function')) {
    return buildStatusReport(env);
  }

  const existing = statusReportInFlight.get(db);
  if (existing) return existing;

  const pending = buildStatusReport(env);
  statusReportInFlight.set(db, pending);
  pending.finally(() => {
    if (statusReportInFlight.get(db) === pending) statusReportInFlight.delete(db);
  }).catch(() => {});
  return pending;
}
