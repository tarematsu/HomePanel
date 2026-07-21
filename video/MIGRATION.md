# VP and HomePanel Worker consolidation

This directory is the imported snapshot of `tarematsu/VP`.

- Source commit: `9984a5db4104019a2537a3018aa7b754f9ad4228`
- Imported into HP as: `video/`
- Unified Worker: `homepanel-cloud`
- Unified D1 database: `homepanel-data`
- Retired video Worker: `videoscraper`
- Retired video D1 database: `twivideo-swiper-db`

## Runtime boundary

The unified entry point is `cloud/src/unified_worker.js`.

- `/admin`, `/v1`, and `/v1/*` continue to use the existing HomePanel Worker implementation.
- `/api/*` and the static application routes continue to use the imported video implementation.
- Queue events are delegated to the video implementation after migration activation.
- HomePanel keeps its existing Worker name, URL, secrets, `DB`, R2, and Durable Object namespace.
- Video uses the same `DB` binding after its schema and rows were migrated into `homepanel-data`.
- Video assets, Browser Rendering, and manual-import queues are attached to `homepanel-cloud`.
- Video liveness is registered as the `video_liveness` job in HomePanel's existing `SchedulerCoordinator` Durable Object.
- The liveness job runs every 720 seconds and checks one video URL per run.
- No Cloudflare Cron Trigger is used for video liveness or automatic video collection.
- A D1 activation flag keeps video fetch and queue handlers disabled unless the verified unified runtime is active.

## Scheduling and free-plan budget

The shared HomePanel scheduler owns the next alarm and coalesces jobs that become due together. Video liveness therefore does not create a second scheduler or a separate Cron Trigger.

The liveness job deliberately keeps the previous conservative work budget:

- interval: 12 minutes;
- batch size: one URL;
- probe: first-byte range request;
- concurrency: one;
- timeout: eight seconds;
- overlap protection: D1 lock;
- failure retry: HomePanel scheduler exponential backoff, capped at the normal 12-minute interval.

This produces at most 120 normal liveness probes per day before retries. Failures do not fan out into parallel checks.

## Production state

The videoscraper data migration and unified-runtime activation completed successfully. The legacy `videoscraper` Worker and `twivideo-swiper-db` database were subsequently deleted. `homepanel-cloud`, `homepanel-data`, `homepanel-updates`, existing HomePanel secrets, and the scheduler Durable Object namespace remain authoritative.

No tablet URL change is required.

Original VP workflows remain under `video/.github/workflows` as historical migration references. Active monorepo workflows belong under the repository root `.github/workflows/` directory.
