#!/usr/bin/env node

import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

const API_ROOT = 'https://api.cloudflare.com/client/v4';
const GRAPHQL_URL = `${API_ROOT}/graphql`;

function parseList(value) {
  return String(value || '').split(',').map((item) => item.trim()).filter(Boolean);
}

function isoDate(date) {
  return date.toISOString().slice(0, 10);
}

function shiftUtcDate(date, days) {
  const next = new Date(date);
  next.setUTCDate(next.getUTCDate() + days);
  return next;
}

function numeric(value) {
  const number = Number(value || 0);
  return Number.isFinite(number) ? number : 0;
}

function parseDatabases(source, file) {
  const result = [];
  for (const block of source.matchAll(/\{[^{}]*"database_name"[^{}]*"database_id"[^{}]*\}/gs)) {
    const name = block[0].match(/"database_name"\s*:\s*"([^"]+)"/)?.[1];
    const id = block[0].match(/"database_id"\s*:\s*"([^"]+)"/)?.[1];
    if (name && id) result.push({ id, name, config: file });
  }
  return result;
}

function aggregate(groups, referencedIds, dates) {
  const daily = new Map(dates.map((date) => [date, { date, rowsRead: 0, readQueries: 0 }]));
  const byDatabase = new Map();
  for (const group of groups) {
    const date = String(group.dimensions?.date || '');
    const databaseId = String(group.dimensions?.databaseId || '');
    if (!referencedIds.has(databaseId) || !daily.has(date)) continue;
    const sum = group.sum || {};
    const rowsRead = numeric(sum.rowsRead);
    const readQueries = numeric(sum.readQueries);
    const total = daily.get(date);
    total.rowsRead += rowsRead;
    total.readQueries += readQueries;
    const key = `${date}:${databaseId}`;
    const item = byDatabase.get(key) || { date, databaseId, rowsRead: 0, readQueries: 0 };
    item.rowsRead += rowsRead;
    item.readQueries += readQueries;
    byDatabase.set(key, item);
  }
  return { daily, byDatabase };
}

if (process.argv.includes('--self-test')) {
  const { daily } = aggregate([
    { dimensions: { date: '2026-01-01', databaseId: 'a' }, sum: { rowsRead: 12, readQueries: 2 } },
    { dimensions: { date: '2026-01-01', databaseId: 'b' }, sum: { rowsRead: 5, readQueries: 1 } },
    { dimensions: { date: '2026-01-01', databaseId: 'other' }, sum: { rowsRead: 999 } },
  ], new Set(['a', 'b']), ['2026-01-01']);
  if (daily.get('2026-01-01').rowsRead !== 17) throw new Error('D1 aggregation self-test failed');
  const parsed = parseDatabases('{"d1_databases":[{"database_name":"db","database_id":"id"}]}', 'x');
  if (parsed.length !== 1 || parsed[0].id !== 'id') throw new Error('D1 config self-test failed');
  console.log('D1 daily budget self-test passed');
  process.exit(0);
}

const token = String(process.env.CLOUDFLARE_API_TOKEN || '').trim();
const budget = Number(process.env.D1_DAILY_READ_BUDGET || 0);
const configFiles = parseList(process.env.D1_WRANGLER_FILES);
const outputDir = path.resolve(process.env.DIAGNOSTICS_OUTPUT_DIR || 'cloudflare-diagnostics');
if (!token || !Number.isFinite(budget) || budget <= 0 || configFiles.length === 0) {
  throw new Error('CLOUDFLARE_API_TOKEN, D1_DAILY_READ_BUDGET and D1_WRANGLER_FILES are required');
}
await mkdir(outputDir, { recursive: true });

async function api(url, options = {}) {
  const response = await fetch(url, {
    ...options,
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: 'application/json',
      'Content-Type': 'application/json',
      'User-Agent': 'github-actions-cloudflare-diagnostics',
      ...(options.headers || {}),
    },
  });
  const text = await response.text();
  let body;
  try { body = JSON.parse(text); } catch { body = null; }
  if (!response.ok || body?.success === false || body?.errors?.length) {
    throw new Error(`Cloudflare API failed (${response.status}): ${text.slice(0, 1200)}`);
  }
  return body;
}

const referenced = new Map();
for (const file of configFiles) {
  const source = await readFile(file, 'utf8');
  for (const database of parseDatabases(source, file)) {
    const current = referenced.get(database.id) || { ...database, configs: [] };
    current.configs.push(file);
    referenced.set(database.id, current);
  }
}
if (referenced.size === 0) throw new Error(`No D1 databases found in ${configFiles.join(', ')}`);

async function discoverAccounts() {
  const override = String(process.env.CLOUDFLARE_ACCOUNT_ID || '').trim();
  const accounts = override
    ? [{ id: override, name: 'configured' }]
    : (await api(`${API_ROOT}/accounts?per_page=50`)).result || [];
  const matches = [];
  for (const account of accounts) {
    let databases;
    try {
      databases = (await api(`${API_ROOT}/accounts/${account.id}/d1/database?per_page=100`)).result || [];
    } catch (error) {
      if (override) throw error;
      continue;
    }
    const ids = new Set(databases.map((database) => database.uuid || database.id));
    const present = [...referenced.keys()].filter((id) => ids.has(id));
    if (present.length) matches.push({ id: account.id, name: account.name || account.id, present });
  }
  const found = new Set(matches.flatMap((account) => account.present));
  const missing = [...referenced.keys()].filter((id) => !found.has(id));
  if (missing.length) throw new Error(`Referenced D1 databases are not visible: ${missing.join(', ')}`);
  return matches;
}

const query = `query D1DailyUsage($accountTag: string!, $start: Date!, $end: Date!) {
  viewer {
    accounts(filter: { accountTag: $accountTag }) {
      d1AnalyticsAdaptiveGroups(
        limit: 10000
        filter: { date_geq: $start, date_leq: $end }
        orderBy: [date_ASC]
      ) {
        sum { readQueries rowsRead }
        dimensions { date databaseId }
      }
    }
  }
}`;

async function usageForAccount(accountId, start, end) {
  const body = await api(GRAPHQL_URL, {
    method: 'POST',
    body: JSON.stringify({ query, variables: { accountTag: accountId, start, end } }),
  });
  return body.data?.viewer?.accounts?.[0]?.d1AnalyticsAdaptiveGroups || [];
}

const now = new Date();
const today = isoDate(now);
const yesterday = isoDate(shiftUtcDate(now, -1));
const accounts = await discoverAccounts();
const groups = (await Promise.all(accounts.map((account) => usageForAccount(account.id, yesterday, today)))).flat();
const { daily, byDatabase } = aggregate(groups, new Set(referenced.keys()), [yesterday, today]);
const latestComplete = daily.get(yesterday);
const currentPartial = daily.get(today);
const elapsedHours = Math.max(1, now.getUTCHours() + now.getUTCMinutes() / 60 + now.getUTCSeconds() / 3600);
const projectedToday = Math.round(currentPartial.rowsRead * Math.min(24, 24 / elapsedHours));
const violations = [];
if (latestComplete.rowsRead > budget) violations.push(`${yesterday} rows read ${latestComplete.rowsRead} > ${budget}`);
if (currentPartial.rowsRead > budget) violations.push(`${today} rows read ${currentPartial.rowsRead} > ${budget}`);
const projectionWarning = projectedToday > budget;

const namedRows = [...byDatabase.values()].map((item) => ({
  ...item,
  databaseName: referenced.get(item.databaseId)?.name || item.databaseId,
})).sort((a, b) => a.date.localeCompare(b.date) || b.rowsRead - a.rowsRead);
const report = {
  generatedAt: now.toISOString(),
  budgetRowsRead: budget,
  accounts: accounts.map(({ id, name }) => ({ id, name })),
  databases: [...referenced.values()],
  latestComplete,
  currentPartial,
  projectedTodayRowsRead: projectedToday,
  projectionWarning,
  violations,
  byDatabase: namedRows,
};
await writeFile(path.join(outputDir, 'd1-daily-budget.json'), `${JSON.stringify(report, null, 2)}\n`);

const fmt = new Intl.NumberFormat('en-US');
const lines = [
  '## D1 daily read budget',
  '',
  `- Budget: **${fmt.format(budget)} rows/day**`,
  `- Latest complete day (${yesterday}): **${fmt.format(latestComplete.rowsRead)}**`,
  `- Current partial day (${today}): **${fmt.format(currentPartial.rowsRead)}**`,
  `- Projected today: **${fmt.format(projectedToday)}**${projectionWarning ? ' ⚠️' : ''}`,
  '',
  '| Date | Database | Rows read | Read queries |',
  '|---|---|---:|---:|',
  ...namedRows.map((item) => `| ${item.date} | ${item.databaseName} | ${fmt.format(item.rowsRead)} | ${fmt.format(item.readQueries)} |`),
  '',
];
await writeFile(path.join(outputDir, 'd1-daily-budget.md'), `${lines.join('\n')}\n`);
const summaryPath = process.env.GITHUB_STEP_SUMMARY;
if (summaryPath) await writeFile(summaryPath, `${lines.join('\n')}\n`, { flag: 'a' });

console.log(`D1_DAILY_BUDGET budget=${budget} yesterday=${latestComplete.rowsRead} today=${currentPartial.rowsRead} projected=${projectedToday}`);
if (projectionWarning) {
  console.log(`::warning title=D1 daily read projection::projected=${projectedToday} budget=${budget}`);
}
if (violations.length) {
  for (const violation of violations) console.error(`::error title=D1 daily read budget exceeded::${violation}`);
  process.exit(1);
}
