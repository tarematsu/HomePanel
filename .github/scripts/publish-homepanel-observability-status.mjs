#!/usr/bin/env node

import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';

export const STATUS_ISSUE_TITLE = 'HomePanel Observability Status';
export const STATUS_MARKER = '<!-- homepanel-observability-status -->';

const VALID_OUTCOMES = new Set(['success', 'failure', 'cancelled', 'skipped']);
const MAX_SECTION_CHARS = 12_000;
const MAX_ISSUE_BODY_CHARS = 60_000;

export function normalizeOutcome(value) {
  const outcome = String(value || '').trim().toLowerCase();
  return VALID_OUTCOMES.has(outcome) ? outcome : 'unknown';
}

export function statusState(outcome) {
  const normalized = normalizeOutcome(outcome);
  return normalized === 'success' || normalized === 'skipped' ? 'success' : 'failure';
}

export function overallOutcome(outcomes) {
  return Object.values(outcomes).every((value) => statusState(value) === 'success')
    ? 'success'
    : 'failure';
}

function sanitize(text) {
  return String(text || '')
    .replace(/Bearer\s+[A-Za-z0-9._~+\/=:-]+/gi, 'Bearer [redacted]')
    .replace(/([?&](?:token|key|secret|signature|sig|auth)=)[^\s&#)]+/gi, '$1[redacted]')
    .replace(/(CLOUDFLARE_(?:API_TOKEN|BUILDS_API_TOKEN|ACCOUNT_ID)\s*[=:]\s*)\S+/gi, '$1[redacted]');
}

function clipped(text, maximum = MAX_SECTION_CHARS) {
  const value = sanitize(text).trim();
  if (value.length <= maximum) return value;
  return `${value.slice(0, maximum)}\n\n…truncated…`;
}

async function readOptional(path) {
  try {
    return clipped(await readFile(path, 'utf8'));
  } catch (error) {
    if (error?.code === 'ENOENT') return '';
    throw error;
  }
}

function section(title, body) {
  if (!body) return '';
  return `\n<details>\n<summary>${title}</summary>\n\n${body}\n\n</details>\n`;
}

export function buildIssueBody({
  generatedAt,
  targetSha,
  runUrl,
  trigger,
  lookbackMinutes,
  outcomes,
  summaries = {},
}) {
  const overall = overallOutcome(outcomes);
  const rows = Object.entries(outcomes)
    .map(([name, outcome]) => `| ${name} | ${normalizeOutcome(outcome)} |`)
    .join('\n');
  const body = `${STATUS_MARKER}
# HomePanel Observability Status

This issue is maintained automatically by the HomePanel Cloudflare Observability workflow.

- **Overall:** ${overall}
- **Generated:** ${generatedAt}
- **Trigger:** ${trigger}
- **Commit:** \`${targetSha}\`
- **Workflow run:** ${runUrl}
- **Telemetry lookback:** ${lookbackMinutes} minutes

| Gate | Outcome |
|---|---|
${rows}
${section('UTC daily request and D1 budgets', summaries.daily)}
${section('Cloudflare metrics and persisted errors', summaries.observability)}
${section('Current-version CPU and error policy', summaries.telemetry)}
`;
  return clipped(body, MAX_ISSUE_BODY_CHARS);
}

function requiredEnv(name) {
  const value = String(process.env[name] || '').trim();
  if (!value) throw new Error(`${name} is required`);
  return value;
}

async function githubRequest(method, path, payload) {
  const repository = requiredEnv('GITHUB_REPOSITORY');
  const token = requiredEnv('GITHUB_TOKEN');
  const response = await fetch(`https://api.github.com/repos/${repository}${path}`, {
    method,
    headers: {
      Accept: 'application/vnd.github+json',
      Authorization: `Bearer ${token}`,
      'Content-Type': 'application/json',
      'User-Agent': 'homepanel-observability-status',
      'X-GitHub-Api-Version': '2022-11-28',
    },
    body: payload == null ? undefined : JSON.stringify(payload),
  });
  const text = await response.text();
  let body = null;
  if (text) {
    try {
      body = JSON.parse(text);
    } catch {
      body = { message: text.slice(0, 500) };
    }
  }
  if (!response.ok) {
    throw new Error(`GitHub ${method} ${path} failed: ${response.status} ${text.slice(0, 500)}`);
  }
  return body;
}

async function publishCommitStatuses(targetSha, runUrl, outcomes) {
  const contexts = {
    policy: 'observability/policy-self-test',
    daily: 'observability/daily-d1-budget',
    query: 'observability/cloudflare-query',
    telemetry: 'observability/telemetry-policy',
  };
  for (const [key, context] of Object.entries(contexts)) {
    const outcome = normalizeOutcome(outcomes[key]);
    await githubRequest('POST', `/statuses/${encodeURIComponent(targetSha)}`, {
      state: statusState(outcome),
      context,
      description: `${key}: ${outcome}`.slice(0, 140),
      target_url: runUrl,
    });
  }
  const overall = overallOutcome(outcomes);
  await githubRequest('POST', `/statuses/${encodeURIComponent(targetSha)}`, {
    state: overall,
    context: 'observability/overall',
    description: `HomePanel observability: ${overall}`,
    target_url: runUrl,
  });
}

async function upsertStatusIssue(body) {
  const issues = await githubRequest('GET', '/issues?state=all&per_page=100&sort=updated&direction=desc');
  const existing = issues.find((issue) => (
    !issue.pull_request
    && issue.title === STATUS_ISSUE_TITLE
    && String(issue.body || '').includes(STATUS_MARKER)
  ));
  if (existing) {
    return githubRequest('PATCH', `/issues/${existing.number}`, {
      title: STATUS_ISSUE_TITLE,
      body,
      state: 'open',
    });
  }
  return githubRequest('POST', '/issues', {
    title: STATUS_ISSUE_TITLE,
    body,
  });
}

export async function publishFromEnvironment() {
  const targetSha = requiredEnv('OBSERVABILITY_TARGET_SHA');
  const runUrl = requiredEnv('OBSERVABILITY_RUN_URL');
  const outcomes = {
    policy: process.env.POLICY_OUTCOME,
    daily: process.env.DAILY_BUDGET_OUTCOME,
    query: process.env.OBSERVABILITY_QUERY_OUTCOME,
    telemetry: process.env.TELEMETRY_POLICY_OUTCOME,
  };
  const summaries = {
    daily: await readOptional('daily-usage/summary.md'),
    observability: await readOptional('observability-summary.md'),
    telemetry: await readOptional('telemetry-summary.md'),
  };
  const body = buildIssueBody({
    generatedAt: new Date().toISOString(),
    targetSha,
    runUrl,
    trigger: process.env.OBSERVABILITY_TRIGGER || 'unknown',
    lookbackMinutes: process.env.LOOKBACK_MINUTES || '60',
    outcomes,
    summaries,
  });
  await publishCommitStatuses(targetSha, runUrl, outcomes);
  const issue = await upsertStatusIssue(body);
  console.log(`Published HomePanel observability status to issue #${issue.number}`);
}

function selfTest() {
  assert.equal(statusState('success'), 'success');
  assert.equal(statusState('skipped'), 'success');
  assert.equal(statusState('failure'), 'failure');
  assert.equal(overallOutcome({ a: 'success', b: 'skipped' }), 'success');
  assert.equal(overallOutcome({ a: 'success', b: 'failure' }), 'failure');
  const body = buildIssueBody({
    generatedAt: '2026-07-23T00:00:00.000Z',
    targetSha: 'abc123',
    runUrl: 'https://github.com/tarematsu/HP/actions/runs/1',
    trigger: 'workflow_run',
    lookbackMinutes: '60',
    outcomes: { policy: 'success', daily: 'failure', query: 'skipped', telemetry: 'skipped' },
    summaries: {
      daily: '## Daily\n\n| Metric | Value |\n|---|---:|\n| D1 | 1 |',
      telemetry: 'Authorization: Bearer secret-value',
    },
  });
  assert.match(body, /HomePanel Observability Status/);
  assert.match(body, /\| daily \| failure \|/);
  assert.match(body, /\| query \| skipped \|/);
  assert.match(body, /abc123/);
  assert.match(body, /UTC daily request and D1 budgets/);
  assert.doesNotMatch(body, /secret-value/);
  assert.match(body, /Bearer \[redacted\]/);
  console.log('HomePanel observability status publisher self-test passed');
}

if (process.argv.includes('--self-test')) {
  selfTest();
} else if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  publishFromEnvironment().catch((error) => {
    console.error(`::error title=Publish HomePanel observability status::${String(error?.message || error).replaceAll('\n', ' ').slice(0, 1000)}`);
    process.exitCode = 1;
  });
}
