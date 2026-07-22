import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const repo = new URL('../../', import.meta.url);
const read = (relative) => readFileSync(new URL(relative, repo), 'utf8');
const workflow = read('.github/workflows/fetch-cloudflare-observability.yml');
const audit = read('.github/scripts/audit-cloudflare-telemetry.py');
const metrics = read('.github/scripts/query-cloudflare-observability.py');
const liveTail = read('.github/scripts/capture-cloudflare-live-tail.mjs');
const d1Budget = read('.github/scripts/check-d1-daily-budget.mjs');
const wrangler = read('cloud/wrangler.jsonc');

assert.match(workflow, /^  workflow_run:\n/m);
assert.match(workflow, /^  schedule:\n/m);
assert.match(workflow, /^  workflow_dispatch:\n/m);
assert.doesNotMatch(workflow, /^  pull_request:\n/m);
assert.doesNotMatch(workflow, /^  push:\n/m);
assert.match(workflow, /LIVE_TAIL_SECONDS: "75"/);
assert.match(workflow, /D1_DAILY_READ_BUDGET: "50000"/);
assert.match(workflow, /D1_WRANGLER_FILES: cloud\/wrangler\.jsonc/);
assert.match(workflow, /failure\(\) \|\| github\.event_name == 'workflow_dispatch'/);
assert.doesNotMatch(workflow, /npm ci|wrangler d1 insights|aws s3api|R2_BUCKET/);

assert.match(audit, /merge_events/);
assert.match(audit, /coverage_ok/);
assert.match(audit, /LIVE_TAIL_LOG/);
assert.match(audit, /DURABLE_OBJECT_CPU_BUDGET_MS/);
assert.match(metrics, /microseconds_to_ms/);
assert.match(metrics, /workersInvocationsAdaptive/);
assert.match(liveTail, /LIVE_TAIL_WORKER/);
assert.match(liveTail, /LIVE_TAIL_PROBES/);
assert.match(liveTail, /\[redacted\]/);
assert.match(d1Budget, /D1_DAILY_READ_BUDGET/);
assert.match(d1Budget, /latestComplete\.rowsRead > budget/);
assert.match(d1Budget, /currentPartial\.rowsRead > budget/);
assert.match(d1Budget, /projectionWarning/);

assert.match(wrangler, /"observability"\s*:\s*\{/u);
assert.match(wrangler, /"persist"\s*:\s*true/u);
assert.match(wrangler, /"invocation_logs"\s*:\s*true/u);
assert.doesNotMatch(wrangler, /"logpush"\s*:\s*true/u);

const commands = [
  ['python3', ['-m', 'py_compile',
    fileURLToPath(new URL('../../.github/scripts/audit-cloudflare-telemetry.py', import.meta.url)),
    fileURLToPath(new URL('../../.github/scripts/query-cloudflare-observability.py', import.meta.url))]],
  [process.execPath, ['--check', fileURLToPath(new URL('../../.github/scripts/capture-cloudflare-live-tail.mjs', import.meta.url))]],
  [process.execPath, [fileURLToPath(new URL('../../.github/scripts/check-d1-daily-budget.mjs', import.meta.url)), '--self-test']],
];
for (const [command, args] of commands) {
  const result = spawnSync(command, args, { encoding: 'utf8' });
  assert.equal(result.status, 0, `${command} ${args.join(' ')}\n${result.stdout}\n${result.stderr}`);
}

console.log('Cloudflare diagnostics configuration validated.');
