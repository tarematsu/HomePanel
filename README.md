# HomePanel

HomePanel is split into two deployable parts.

- `cloud/`: Cloudflare Workers and D1 backend
- `native/`: Windows native application

## Layout

- `cloud/` serves dashboard data, telemetry ingestion, update manifests, and device control endpoints.
- `native/` renders the dashboard UI, hosts WebView2, and talks to the cloud endpoints.

## Requirements

- Cloud credentials and runtime configuration must be stored in Cloudflare or GitHub secrets/variables.
- Native update checks expect a reachable Cloudflare update endpoint.

## Cloud checks

```powershell
cd cloud
npm install --no-audit --no-fund
npm run check
npm test
```

## Native build

```powershell
cmake -S native -B native/build-ci -G "Visual Studio 17 2022" -A x64
cmake --build native/build-ci --config Release --parallel
```

For cloud-specific deployment details, see [cloud/README.md](/C:/HomePanel/cloud/README.md).
