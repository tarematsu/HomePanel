#!/usr/bin/env node

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdir, writeFile } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const OUT = process.env.D1_INSIGHTS_OUTPUT_DIR || 'd1-insights';
const DATABASE = process.env.D1_DATABASE_NAME || 'homepanel-data';
const LOOKBACK_MINUTES = Math.max(1, Number.parseInt(process.env.LOOKBACK_MINUTES || '60', 10) || 60);
const LIMIT = Math.max(1, Math.min(100, Number.parseInt(process.env.D1_INSIGHTS_LIMIT || '20', 10) || 20));

function number(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
}

export function fingerprint(query) {
  return createHash('sha256').update(String(query || '')).digest('hex').slice(0, 12);
}

export function sanitizeQuery(query) {
  return String(query || '')
    .replace(/'(?:''|[^'])*'/g, '?')
    .replace(/"(?:""|[^"])*"/g, '?')
    .replace(/\b(?:[0-9a-f]{8}-[0-9a-f-]{27,}|0x[0-9a-f]+|\d{4,})\b/gi, '?')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 420);
}

export function normalizeRows(input) {
  if (!Array.isArray(input)) throw new Error('Wrangler D1 insights did not return an array');
  return input.map((row) => ({
    fingerprint: fingerprint(row?.query),
    query: sanitizeQuery(row?.query),
    totalRowsRead: number(row?.totalRowsRead),
    avgRowsRead: number(row?.avgRowsRead),
    totalRowsWritten: number(row?.totalRowsWritten),
    avgRowsWritten: number(row?.avgRowsWritten),
    numberOfTimesRun: number(row?.numberOfTimesRun),
    totalDurationMs: number(row?.totalDurationMs),
    avgDurationMs: number(row?.avgDurationMs),
    queryEfficiency: number(row?.queryEfficiency),
  })).sort((left, right) => right.totalRowsRead - left.totalRowsRead || right.numberOfTimesRun - left.numberOfTimesRun);
}

function parseJsonOutput(output) {
  const text = String(output || '').trim();
  const start = text.indexOf('[');
  const end = text.lastIndexOf(']');
  if (start < 0 || end < start) throw new Error(`Unable to locate D1 insights JSON in Wrangler output: ${text.slice(0, 500)}`);
  return JSON.parse(text.slice(start, end + 1));
}

export function markdown(rows, generatedAt = new Date().toISOString()) {
  const totalRowsRead = rows.reduce((sum, row) => sum + row.totalRowsRead, 0);
  const totalExecutions = rows.reduce((sum, row) => sum + row.numberOfTimesRun, 0);
  const lines = [
    '## D1 query insights',
    '',
    `- Database: \`${DATABASE}\``,
    `- Lookback: \`${LOOKBACK_MINUTES} minutes\``,
    `- Generated: \`${generatedAt}\``,
    `- Top-query rows read: \`${totalRowsRead}\``,
    `- Top-query executions: \`${totalExecutions}\``,
    '',
  ];
  if (!rows.length) {
    lines.push('No D1 query insight rows were returned for this window.');
    return `${lines.join('\n')}\n`;
  }
  lines.push('| SQL fingerprint | Total reads | Avg reads | Runs | Total writes | Avg ms | Sanitized SQL |');
  lines.push('|---|---:|---:|---:|---:|---:|---|');
  for (const row of rows) {
    const sql = row.query.replaceAll('|', '\\|') || '(empty query)';
    lines.push(`| \`${row.fingerprint}\` | ${row.totalRowsRead} | ${row.avgRowsRead.toFixed(2)} | ${row.numberOfTimesRun} | ${row.totalRowsWritten} | ${row.avgDurationMs.toFixed(3)} | \`${sql}\` |`);
  }
  return `${lines.join('\n')}\n`;
}

async function run() {
  const wrangler = process.platform === 'win32'
    ? 'cloud\\node_modules\\.bin\\wrangler.cmd'
    : 'cloud/node_modules/.bin/wrangler';
  const args = [
    'd1', 'insights', DATABASE,
    '--time-period', `${LOOKBACK_MINUTES}m`,
    '--sort-type', 'sum',
    '--sort-by', 'reads',
    '--sort-direction', 'DESC',
    '--limit', String(LIMIT),
    '--json',
    '--config', 'cloud/wrangler.jsonc',
  ];
  const result = spawnSync(wrangler, args, {
    encoding: 'utf8',
    env: process.env,
    maxBuffer: 10 * 1024 * 1024,
  });
  const combined = `${result.stdout || ''}\n${result.stderr || ''}`.trim();
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`wrangler d1 insights failed (${result.status}): ${combined.slice(0, 2000)}`);
  const rows = normalizeRows(parseJsonOutput(result.stdout || combined));
  const generatedAt = new Date().toISOString();
  await mkdir(OUT, { recursive: true });
  await writeFile(`${OUT}/insights.json`, `${JSON.stringify({ database: DATABASE, lookbackMinutes: LOOKBACK_MINUTES, generatedAt, rows }, null, 2)}\n`);
  await writeFile(`${OUT}/summary.md`, markdown(rows, generatedAt));
  process.stdout.write(markdown(rows, generatedAt));
}

function selfTest() {
  const rows = normalizeRows([{
    query: "SELECT * FROM device_commands WHERE device_id='secret-device' AND id=12345",
    totalRowsRead: 25,
    avgRowsRead: 12.5,
    totalRowsWritten: 0,
    avgRowsWritten: 0,
    numberOfTimesRun: 2,
    totalDurationMs: 3,
    avgDurationMs: 1.5,
    queryEfficiency: 0.5,
  }]);
  assert.equal(rows[0].totalRowsRead, 25);
  assert.doesNotMatch(rows[0].query, /secret-device|12345/);
  assert.match(markdown(rows, '2026-07-23T00:00:00.000Z'), /D1 query insights/);
  assert.match(markdown(rows), /SQL fingerprint/);
  console.log('D1 insights reporter self-test passed');
}

if (process.argv.includes('--self-test')) {
  selfTest();
} else if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  run().catch((error) => {
    console.error(`::error title=Collect D1 query insights::${String(error?.message || error).replaceAll('\n', ' ').slice(0, 1500)}`);
    process.exitCode = 1;
  });
}
