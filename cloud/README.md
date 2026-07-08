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
```

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
