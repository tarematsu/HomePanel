import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  buildParameterizedInsert,
  columnNamesFromPragma,
  oversizedSqlStatements,
  quoteSqliteIdentifier,
} from './d1-import-utils.mjs';

const cloudRoot = join(dirname(fileURLToPath(import.meta.url)), '..');
const wranglerCli = join(cloudRoot, 'node_modules', 'wrangler', 'bin', 'wrangler.js');
const targetConfig = join(cloudRoot, '.wrangler', 'generated', 'homepanel-existing.jsonc');
const postImportSql = join(cloudRoot, 'scripts', 'video-post-import.sql');
const sourceDatabase = process.env.VIDEO_SOURCE_D1_NAME?.trim() || 'twivideo-swiper-db';
const targetDatabase = process.env.HOMEPANEL_D1_DATABASE_NAME?.trim() || 'homepanel-data';
const exportDirectory = process.env.VIDEO_D1_EXPORT_DIR?.trim()
  || join(cloudRoot, '.wrangler', 'video-d1-export');
const runtimeStateTable = 'video_runtime_state';

const importOrder = Object.freeze([
  'videos',
  'ranking_entries',
  'collection_runs',
  'reports',
  'worker_locks',
  'collection_capture_snapshots',
  'collection_capture_network_events',
  'd1_maintenance_state',
  'playback_feed_state',
  'collection_run_timings',
  'video_blocklist',
  'video_death_list',
  'video_liveness_state',
  'manual_import_jobs',
  'manual_import_job_chunks',
  'video_orientations',
  'status_counts',
  'automatic_reset_state'
]);
const expectedTables = new Set(importOrder);

function cloudflareEnvironment() {
  const env = { ...process.env, CI: 'true' };
  const token = process.env.CLOUDFLARE_API_TOKEN?.trim()
    || process.env.CLOUDFLARE_BUILDS_API_TOKEN?.trim();
  const accountId = process.env.CLOUDFLARE_ACCOUNT_ID?.trim()
    || process.env.CLOUDFLARE_BUILDS_ACCOUNT_ID?.trim();
  if (token) env.CLOUDFLARE_API_TOKEN = token;
  if (accountId) env.CLOUDFLARE_ACCOUNT_ID = accountId;
  return env;
}

function cloudflareCredentials() {
  const token = process.env.CLOUDFLARE_API_TOKEN?.trim()
    || process.env.CLOUDFLARE_BUILDS_API_TOKEN?.trim();
  const accountId = process.env.CLOUDFLARE_ACCOUNT_ID?.trim()
    || process.env.CLOUDFLARE_BUILDS_ACCOUNT_ID?.trim();
  if (!token) throw new Error('Cloudflare API token is required for parameterized D1 imports');
  if (!accountId) throw new Error('Cloudflare account ID is required for parameterized D1 imports');
  return { token, accountId };
}

function wrangler(args, capture = false) {
  return execFileSync(process.execPath, [wranglerCli, ...args], {
    cwd: cloudRoot,
    env: cloudflareEnvironment(),
    encoding: capture ? 'utf8' : undefined,
    stdio: capture ? ['ignore', 'pipe', 'inherit'] : 'inherit'
  });
}

function parseJsonOutput(text) {
  const starts = [text.indexOf('{'), text.indexOf('[')].filter((index) => index >= 0);
  const start = Math.min(...starts);
  const end = Math.max(text.lastIndexOf('}'), text.lastIndexOf(']'));
  if (!Number.isFinite(start) || end < start) throw new Error('Wrangler did not return JSON');
  return JSON.parse(text.slice(start, end + 1));
}

function resultRows(payload) {
  const entries = Array.isArray(payload) ? payload : [payload];
  const rows = [];
  for (const entry of entries) {
    if (Array.isArray(entry?.results)) rows.push(...entry.results);
    else if (Array.isArray(entry?.result?.results)) rows.push(...entry.result.results);
  }
  return rows;
}

function query(database, sql, target = false) {
  const args = ['d1', 'execute', database, '--remote', '--command', sql, '--json'];
  if (target) args.push('--config', targetConfig);
  return resultRows(parseJsonOutput(wrangler(args, true)));
}

function executeTargetCommand(sql) {
  wrangler([
    'd1', 'execute', targetDatabase, '--remote', '--command', sql,
    '--config', targetConfig
  ]);
}

function executeTargetFile(path) {
  wrangler([
    'd1', 'execute', targetDatabase, '--remote', '--file', path,
    '--config', targetConfig
  ]);
}

async function cloudflareApi(path, init = {}) {
  const { token } = cloudflareCredentials();
  let lastError;
  for (let attempt = 1; attempt <= 3; attempt += 1) {
    try {
      const response = await fetch(`https://api.cloudflare.com/client/v4${path}`, {
        ...init,
        headers: {
          Authorization: `Bearer ${token}`,
          Accept: 'application/json',
          ...(init.headers || {})
        },
        signal: AbortSignal.timeout(30_000)
      });
      const text = await response.text();
      let payload;
      try {
        payload = text ? JSON.parse(text) : {};
      } catch {
        throw new Error(`Cloudflare API returned non-JSON (${response.status})`);
      }
      if (!response.ok || payload?.success === false) {
        const errors = Array.isArray(payload?.errors)
          ? payload.errors.map((error) => `${error?.code || 'error'}: ${error?.message || 'unknown error'}`).join('; ')
          : '';
        const error = new Error(`Cloudflare API ${response.status}: ${errors || 'request failed'}`);
        if (response.status < 500 && response.status !== 429) throw error;
        lastError = error;
      } else {
        return payload;
      }
    } catch (error) {
      lastError = error;
      if (attempt === 3) break;
    }
    await new Promise((resolve) => setTimeout(resolve, attempt * 1_000));
  }
  throw lastError || new Error('Cloudflare API request failed');
}

let targetDatabaseIdPromise;
function targetDatabaseId() {
  targetDatabaseIdPromise ??= (async () => {
    const configured = process.env.HOMEPANEL_D1_DATABASE_ID?.trim();
    if (configured) return configured;

    const { accountId } = cloudflareCredentials();
    const payload = await cloudflareApi(
      `/accounts/${encodeURIComponent(accountId)}/d1/database?name=${encodeURIComponent(targetDatabase)}`
    );
    const databases = Array.isArray(payload?.result) ? payload.result : [];
    const matches = databases.filter((database) => String(database?.name || '') === targetDatabase);
    if (matches.length !== 1) {
      throw new Error(`Could not resolve a unique D1 database ID for ${targetDatabase}`);
    }
    const id = String(matches[0]?.uuid || matches[0]?.id || '').trim();
    if (!id) throw new Error(`Cloudflare did not return a database ID for ${targetDatabase}`);
    return id;
  })();
  return targetDatabaseIdPromise;
}

async function executeTargetPrepared(sql, params) {
  const { accountId } = cloudflareCredentials();
  const databaseId = await targetDatabaseId();
  const payload = await cloudflareApi(
    `/accounts/${encodeURIComponent(accountId)}/d1/database/${encodeURIComponent(databaseId)}/query`,
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sql, params })
    }
  );
  const results = Array.isArray(payload?.result) ? payload.result : [];
  if (!results.length || results.some((result) => result?.success === false)) {
    throw new Error('Parameterized D1 insert did not return a successful result');
  }
}

function tableNames(database, target = false) {
  const rows = query(database, `
    SELECT name
      FROM sqlite_schema
     WHERE type = 'table'
       AND name NOT LIKE 'sqlite_%'
       AND name NOT LIKE '_cf_%'
       AND name <> 'd1_migrations'
     ORDER BY name;
  `, target);
  return rows.map((row) => String(row.name));
}

function tableCounts(database, target = false) {
  const counts = {};
  for (const table of importOrder) {
    const rows = query(database, `SELECT COUNT(*) AS row_count FROM ${quoteSqliteIdentifier(table)};`, target);
    const count = Number(rows[0]?.row_count);
    if (!Number.isSafeInteger(count) || count < 0) {
      throw new Error(`Invalid row count returned for ${database}.${table}: ${JSON.stringify(rows)}`);
    }
    counts[table] = count;
  }
  return counts;
}

function assertSourceSchema(names) {
  const actual = new Set(names);
  const missing = importOrder.filter((name) => !actual.has(name));
  const unexpected = names.filter((name) => !expectedTables.has(name));
  if (missing.length || unexpected.length) {
    throw new Error(
      `Source video schema does not match the migration allowlist. `
      + `Missing: ${missing.join(', ') || 'none'}. `
      + `Unexpected: ${unexpected.join(', ') || 'none'}.`
    );
  }
}

function assertTargetSchema(names) {
  const actual = new Set(names);
  const missing = [...importOrder, runtimeStateTable].filter((name) => !actual.has(name));
  if (missing.length) {
    throw new Error(`Target HomePanel D1 is missing video tables: ${missing.join(', ')}`);
  }
}

function assertCountsEqual(source, target) {
  const mismatches = importOrder
    .filter((table) => Number(source[table] || 0) !== Number(target[table] || 0))
    .map((table) => `${table}: source=${source[table] || 0}, target=${target[table] || 0}`);
  if (mismatches.length) throw new Error(`Video D1 row-count verification failed: ${mismatches.join('; ')}`);
}

function exportTable(table) {
  const path = join(exportDirectory, `${table}.sql`);
  wrangler([
    'd1', 'export', sourceDatabase, '--remote', '--table', table,
    '--no-schema', '--output', path, '--skip-confirmation'
  ]);
  return path;
}

function oversizedStatementsInFile(path) {
  return oversizedSqlStatements(readFileSync(path, 'utf8'));
}

async function importTableWithParameters(table, expectedRows) {
  const pragmaRows = query(sourceDatabase, `PRAGMA table_info(${quoteSqliteIdentifier(table)});`);
  const columns = columnNamesFromPragma(pragmaRows);
  const primaryKeyColumns = pragmaRows
    .filter((row) => Number(row?.pk) > 0)
    .sort((left, right) => Number(left.pk) - Number(right.pk))
    .map((row) => String(row.name));
  const orderColumns = primaryKeyColumns.length ? primaryKeyColumns : [columns[0]];
  const projection = columns.map(quoteSqliteIdentifier).join(',');
  const orderBy = orderColumns.map(quoteSqliteIdentifier).join(',');

  console.log(`Importing ${table} with bound parameters because its export contains an oversized SQL statement.`);
  for (let offset = 0; offset < expectedRows; offset += 1) {
    const rows = query(
      sourceDatabase,
      `SELECT ${projection} FROM ${quoteSqliteIdentifier(table)} ORDER BY ${orderBy} LIMIT 1 OFFSET ${offset};`
    );
    if (rows.length !== 1) {
      throw new Error(`Expected one source row for ${table} at offset ${offset}, received ${rows.length}`);
    }
    const statement = buildParameterizedInsert(table, columns, rows[0]);
    await executeTargetPrepared(statement.sql, statement.params);
  }
  console.log(`Imported ${expectedRows} row(s) into ${table} with bound parameters.`);
}

function restoreImportGuards() {
  executeTargetFile(postImportSql);
}

function resetTargetVideoData() {
  executeTargetCommand(`
    UPDATE video_runtime_state SET active = 0, activated_at = NULL WHERE id = 1;
    DROP TRIGGER IF EXISTS video_death_skip_ranking;
    DROP TRIGGER IF EXISTS status_counts_delta_on_block_insert;
    DROP TRIGGER IF EXISTS status_counts_dirty_on_block_delete;
    DROP TRIGGER IF EXISTS manual_import_jobs_max_urls_insert;
    DROP TRIGGER IF EXISTS manual_import_jobs_max_urls_update;
    DELETE FROM collection_capture_network_events;
    DELETE FROM collection_capture_snapshots;
    DELETE FROM collection_run_timings;
    DELETE FROM reports;
    DELETE FROM ranking_entries;
    DELETE FROM manual_import_job_chunks;
    DELETE FROM manual_import_jobs;
    DELETE FROM video_blocklist;
    DELETE FROM video_death_list;
    DELETE FROM video_orientations;
    DELETE FROM worker_locks;
    DELETE FROM status_counts;
    DELETE FROM automatic_reset_state;
    DELETE FROM video_liveness_state;
    DELETE FROM playback_feed_state;
    DELETE FROM d1_maintenance_state;
    DELETE FROM collection_runs;
    DELETE FROM videos;
  `);
}

if (!readFileSync(postImportSql, 'utf8').trim()) {
  throw new Error(`Post-import SQL is empty: ${postImportSql}`);
}

rmSync(exportDirectory, { recursive: true, force: true });
mkdirSync(exportDirectory, { recursive: true });

const sourceNames = tableNames(sourceDatabase);
assertSourceSchema(sourceNames);
const sourceCountsBefore = tableCounts(sourceDatabase);
const exports = Object.fromEntries(importOrder.map((table) => [table, exportTable(table)]));
const oversizedImports = Object.fromEntries(
  importOrder.map((table) => [table, oversizedStatementsInFile(exports[table])])
);
const sourceCountsAfter = tableCounts(sourceDatabase);
assertCountsEqual(sourceCountsBefore, sourceCountsAfter);

const targetNames = tableNames(targetDatabase, true);
assertTargetSchema(targetNames);
const targetCountsBefore = tableCounts(targetDatabase, true);

let guardsDropped = false;
let importError;
try {
  resetTargetVideoData();
  guardsDropped = true;
  for (const table of importOrder) {
    const oversized = oversizedImports[table];
    if (oversized.length) {
      console.log(`Detected oversized ${table} SQL statement(s): ${JSON.stringify(oversized)}`);
      await importTableWithParameters(table, sourceCountsBefore[table]);
    } else {
      executeTargetFile(exports[table]);
    }
  }
} catch (error) {
  importError = error;
} finally {
  if (guardsDropped) {
    try {
      restoreImportGuards();
    } catch (restoreError) {
      if (!importError) importError = restoreError;
      else console.error('Failed to restore video import guards', restoreError);
    }
  }
}
if (importError) throw importError;

const targetCountsAfter = tableCounts(targetDatabase, true);
assertCountsEqual(sourceCountsBefore, targetCountsAfter);

const foreignKeyFailures = query(targetDatabase, 'PRAGMA foreign_key_check;', true);
if (foreignKeyFailures.length) {
  throw new Error(`Foreign-key verification failed: ${JSON.stringify(foreignKeyFailures)}`);
}
const schemaRows = query(targetDatabase, `
  SELECT COUNT(*) AS object_count
    FROM sqlite_schema
   WHERE type IN ('table', 'index', 'trigger')
     AND name NOT LIKE 'sqlite_%'
     AND name NOT LIKE '_cf_%';
`, true);
const schemaObjectCount = Number(schemaRows[0]?.object_count ?? 0);
if (!Number.isSafeInteger(schemaObjectCount) || schemaObjectCount < importOrder.length + 1) {
  throw new Error(`D1 schema inventory is incomplete: ${JSON.stringify(schemaRows)}`);
}

executeTargetCommand(`
  UPDATE video_runtime_state
     SET active = 1,
         activated_at = CURRENT_TIMESTAMP
   WHERE id = 1;
`);
const activationRows = query(targetDatabase, `
  SELECT active, activated_at
    FROM video_runtime_state
   WHERE id = 1;
`, true);
const activation = activationRows[0];
if (Number(activation?.active ?? 0) !== 1 || !activation?.activated_at) {
  throw new Error(`Video runtime activation failed: ${JSON.stringify(activationRows)}`);
}

const manifest = {
  migratedAt: new Date().toISOString(),
  sourceDatabase,
  targetDatabase,
  tables: importOrder,
  parameterizedTables: importOrder.filter((table) => oversizedImports[table].length > 0),
  sourceCounts: sourceCountsBefore,
  targetCountsBefore,
  targetCounts: targetCountsAfter,
  foreignKeyCheck: 'ok',
  schemaCheck: 'ok',
  schemaObjectCount,
  runtimeActivation: 'ok',
  activatedAt: String(activation.activated_at)
};
writeFileSync(join(exportDirectory, 'migration-manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(JSON.stringify(manifest, null, 2));
