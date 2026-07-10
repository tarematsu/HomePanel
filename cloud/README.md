# HomePanel Cloud

Cloudflare Workers and D1 backend for HomePanel.

## Configure

Set these in `wrangler.jsonc` or the deploy environment:

- Worker `name`
- `d1_databases[0].database_name`
- Cloudflare and GitHub secrets/variables

`database_id` may stay as the placeholder in source control. The deploy script resolves the real value in this order:

1. `HOMEPANEL_D1_DATABASE_ID` or `D1_DATABASE_ID`
2. A real UUID already stored in `wrangler.jsonc`
3. `wrangler d1 list --json`

## Cloudflare Variables

```text
CITY_NAME
WEATHERNEWS_URL
STATIONHEAD_MONITOR_URL
STATIONHEAD_HEALTH_URL
STATIONHEAD_HEALTH_STALE_MS
STATIONHEAD_ALERT_FROM
HOMEPANEL_PUBLIC_URL
HOMEPANEL_PRIMARY_DEVICE_ID
RADAR_CENTER_LAT
RADAR_CENTER_LON
RADAR_ZOOM
SWITCHBOT_CONTROL_PLUG_IDS
SWITCHBOT_EXIT_CONFIRM_SECONDS
SWITCHBOT_FALLBACK_POLL_SECONDS
UPDATE_BUCKET_PREFIX
```

`STATIONHEAD_HEALTH_URL` is optional when `STATIONHEAD_MONITOR_URL` points to the Stationhead deployment. HomePanel derives `/api/health` from the playback URL. The independent `stationhead_health` scheduler job runs every five minutes and stores its result separately from playback data.

## Cloudflare Secrets

```text
HOMEPANEL_INGEST_SECRET
HOMEPANEL_DEVICE_TOKENS
API_TOKEN
DEVICE_TOKEN
SWITCHBOT_TOKEN
SWITCHBOT_SECRET
OCTOPUS_EMAIL
OCTOPUS_PASSWORD
OCTOPUS_ACCOUNT_NUMBER
UPDATE_SIGNING_SECRET
SPOTIFY_CLIENT_ID
SPOTIFY_CLIENT_SECRET
SPOTIFY_REDIRECT_URI
SPOTIFY_TOKEN_ENCRYPTION_KEY
RESEND_API_KEY
STATIONHEAD_ALERT_TO
```

When both `RESEND_API_KEY` and `STATIONHEAD_ALERT_TO` are configured, HomePanel sends one notification when Stationhead becomes unhealthy and one when it recovers. Monitoring and state storage continue without email configuration.

## Stationhead monitoring

- Source state: `current_state.source = 'stationhead_health'`
- Polling cadence: five minutes
- Manual refresh source: `stationhead_health`
- Read endpoint: `GET /v1/stationhead-health` with a configured HomePanel token
- Playback collection and collector-health monitoring are independent jobs

The Stationhead application remains responsible for exposing `/api/health`; HomePanel Cloud owns polling, transition detection, and optional notification delivery.

## R2

- Create an R2 bucket in Cloudflare, typically `homepanel-updates`.
- Set `HOMEPANEL_UPDATE_BUCKET` in CI if the bucket name differs from source defaults.
- Store the current manifest at `updates/latest/update-manifest.json`.
- Store release files under `updates/releases/<yymmddhhmm>/`.

## Local checks

```powershell
cd cloud
npm install --no-audit --no-fund
npm run check
npm test
npm run migrate:local
```

## CI secrets

- `CLOUDFLARE_API_TOKEN`
- `HOMEPANEL_D1_DATABASE_ID`
- `CLOUDFLARE_BUILDS_API_TOKEN`
- `HOMEPANEL_UPDATE_BUCKET`

Set `CLOUDFLARE_ACCOUNT_ID` when the token cannot resolve a single account automatically.

## Notes

- Keep production URLs, email addresses, and account identifiers out of source control.
- Do not rely on local `.env` or `.dev.vars` files for production credentials.
