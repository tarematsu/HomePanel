import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { stripJsonc } from "./jsonc.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = join(root, "..");
const wranglerCli = join(root, "node_modules", "wrangler", "bin", "wrangler.js");
const generatedDir = join(root, ".wrangler", "generated");
const generatedConfig = join(generatedDir, "homepanel-existing.jsonc");
const migrationsDir = join(root, "migrations");
const databaseIdPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const placeholderDatabaseId = "00000000-0000-0000-0000-000000000000";
const placeholderUpdateBucket = "replace-with-your-r2-bucket-name";
const productionBranch = process.env.HOMEPANEL_PRODUCTION_BRANCH?.trim() || "main";
const buildBranch = process.env.WORKERS_CI_BRANCH?.trim() || "";
const cloudflareManagedBuild = process.env.WORKERS_CI === "1";
const previewBuild = cloudflareManagedBuild && Boolean(buildBranch) && buildBranch !== productionBranch;
const migrateLocal = process.argv.includes("--migrate-local");
const migrateRemoteOnly = process.argv.includes("--migrate-only");
const forceRemoteMigrations = process.argv.includes("--with-migrations")
  || process.env.HOMEPANEL_APPLY_D1_MIGRATIONS?.trim() === "1";
const secretsFile = process.env.HOMEPANEL_SECRETS_FILE?.trim() || "";
// Schema changes remain owned by the dedicated migration workflow. Routine
// Worker deploys opt out explicitly instead of rediscovering the same list.
const skipRemoteMigrations = process.argv.includes("--without-migrations")
  || process.env.HOMEPANEL_SKIP_D1_MIGRATIONS?.trim() === "1";

if (skipRemoteMigrations && (migrateRemoteOnly || forceRemoteMigrations)) {
  throw new Error("Remote D1 migrations cannot be both forced and skipped");
}

function cloudflareEnvironment() {
  const env = { ...process.env, CI: "true" };
  const token = process.env.CLOUDFLARE_API_TOKEN?.trim()
    || process.env.CLOUDFLARE_BUILDS_API_TOKEN?.trim();
  const accountId = process.env.CLOUDFLARE_ACCOUNT_ID?.trim()
    || process.env.CLOUDFLARE_BUILDS_ACCOUNT_ID?.trim()
    || process.env.ACCOUNT_ID?.trim();
  if (token) env.CLOUDFLARE_API_TOKEN = token;
  if (accountId) env.CLOUDFLARE_ACCOUNT_ID = accountId;
  return env;
}

function wrangler(args, capture = false) {
  return execFileSync(process.execPath, [wranglerCli, ...args], {
    cwd: root,
    encoding: capture ? "utf8" : undefined,
    env: cloudflareEnvironment(),
    stdio: capture ? ["inherit", "pipe", "inherit"] : "inherit",
  });
}

function parseJsonc(text, label) {
  try {
    return JSON.parse(stripJsonc(text));
  } catch (error) {
    throw new Error(`${label} is not valid JSON/JSONC: ${error instanceof Error ? error.message : String(error)}`);
  }
}

function parseJsonOutput(text) {
  const starts = [text.indexOf("{"), text.indexOf("[")].filter(index => index >= 0);
  const start = Math.min(...starts);
  const end = Math.max(text.lastIndexOf("}"), text.lastIndexOf("]"));
  if (!Number.isFinite(start) || end < start) throw new Error("Wrangler did not return JSON");
  return JSON.parse(text.slice(start, end + 1));
}

function generatedPath(target) {
  const value = relative(generatedDir, target).replaceAll("\\", "/");
  return value.startsWith(".") ? value : `./${value}`;
}

function isDatabaseId(value) {
  return typeof value === "string" && databaseIdPattern.test(value);
}

function isRealDatabaseId(value) {
  return isDatabaseId(value) && value !== placeholderDatabaseId;
}

function configuredWorkerName(baseConfig) {
  const configured = process.env.HOMEPANEL_WORKER_NAME?.trim() || process.env.WORKER_NAME?.trim();
  if (configured) return configured;
  if (typeof baseConfig.name === "string" && baseConfig.name.trim()) return baseConfig.name.trim();
  throw new Error("Configure a Worker name in wrangler.jsonc or HOMEPANEL_WORKER_NAME");
}

function configuredDatabaseName(baseConfig) {
  const configured = process.env.HOMEPANEL_D1_DATABASE_NAME?.trim() || process.env.D1_DATABASE_NAME?.trim();
  if (configured) return configured;
  const declared = baseConfig.d1_databases?.find(entry => entry?.binding === "DB")?.database_name;
  if (typeof declared === "string" && declared.trim()) return declared.trim();
  throw new Error("Configure a D1 database name in wrangler.jsonc or HOMEPANEL_D1_DATABASE_NAME");
}

function configuredDatabaseId() {
  return process.env.HOMEPANEL_D1_DATABASE_ID?.trim()
    || process.env.D1_DATABASE_ID?.trim()
    || "";
}

function declaredDatabaseId(baseConfig) {
  return baseConfig.d1_databases?.find(entry => entry?.binding === "DB")?.database_id;
}

function configuredUpdateBucket(baseConfig) {
  const configured = process.env.HOMEPANEL_UPDATE_BUCKET?.trim();
  if (configured) return configured;
  const declared = baseConfig.r2_buckets?.find(entry => entry?.binding === "UPDATE_BUCKET")?.bucket_name;
  if (typeof declared === "string" && declared.trim() && declared !== placeholderUpdateBucket) return declared.trim();
  return "";
}

function databaseEntries(payload) {
  if (Array.isArray(payload)) return payload;
  if (Array.isArray(payload?.result)) return payload.result;
  if (Array.isArray(payload?.databases)) return payload.databases;
  return payload && typeof payload === "object" ? [payload] : [];
}

function resolveLocalDatabaseId(baseConfig) {
  const configured = configuredDatabaseId();
  if (configured) {
    if (!isDatabaseId(configured)) {
      throw new Error("HOMEPANEL_D1_DATABASE_ID / D1_DATABASE_ID is not a valid D1 UUID");
    }
    console.log("Using D1 database ID supplied by the build environment for local migrations");
    return configured;
  }

  const declared = declaredDatabaseId(baseConfig);
  if (isDatabaseId(declared)) {
    console.log("Using D1 database ID from wrangler.jsonc for local migrations");
    return declared;
  }

  console.log("Using placeholder D1 database ID for local migrations; remote lookup skipped");
  return placeholderDatabaseId;
}

function resolveDatabaseId(baseConfig, databaseName) {
  const configured = configuredDatabaseId();
  if (configured) {
    if (!isRealDatabaseId(configured)) {
      throw new Error("HOMEPANEL_D1_DATABASE_ID / D1_DATABASE_ID is not a valid non-placeholder D1 UUID");
    }
    console.log("Using D1 database ID supplied by the build environment");
    return configured;
  }

  const declared = declaredDatabaseId(baseConfig);
  if (isRealDatabaseId(declared)) {
    console.log("Using D1 database ID from wrangler.jsonc");
    return declared;
  }

  const listed = parseJsonOutput(wrangler(["d1", "list", "--json"], true));
  const entry = databaseEntries(listed).find(item => item?.name === databaseName);
  const databaseId = entry?.uuid ?? entry?.id ?? entry?.database_id;
  if (!isRealDatabaseId(databaseId)) {
    throw new Error(
      `Existing D1 database '${databaseName}' was not found in the Cloudflare account. ` +
      "Set HOMEPANEL_D1_DATABASE_ID or a real database_id in wrangler.jsonc.",
    );
  }
  return databaseId;
}

function legacyControlPlugIds() {
  const path = join(repositoryRoot, "config.json");
  if (!existsSync(path)) return [];
  try {
    const config = JSON.parse(readFileSync(path, "utf8"));
    const values = config?.presence?.controlPlugDeviceIds;
    if (!Array.isArray(values)) return [];
    return [...new Set(values.map(value => typeof value === "string" ? value : value?.deviceId).filter(Boolean))];
  } catch {
    console.warn("Legacy config.json could not be parsed; control plug IDs were not imported");
    return [];
  }
}

if (!existsSync(migrationsDir)) {
  throw new Error(`Cloudflare D1 migrations directory is missing: ${migrationsDir}`);
}

mkdirSync(generatedDir, { recursive: true });
const config = parseJsonc(readFileSync(join(root, "wrangler.jsonc"), "utf8"), "wrangler.jsonc");
const workerName = configuredWorkerName(config);
const databaseName = configuredDatabaseName(config);
const databaseId = migrateLocal
  ? resolveLocalDatabaseId(config)
  : resolveDatabaseId(config, databaseName);
const updateBucket = configuredUpdateBucket(config);
config.name = workerName;
config.keep_vars = true;
config.main = generatedPath(join(root, config.main));
if (typeof config.assets?.directory === "string") {
  config.assets.directory = generatedPath(join(root, config.assets.directory));
}
config.d1_databases = [{
  binding: "DB",
  database_name: databaseName,
  database_id: databaseId,
  migrations_dir: generatedPath(migrationsDir),
}];
const retainedR2Buckets = Array.isArray(config.r2_buckets)
  ? config.r2_buckets.filter(entry => entry?.binding !== "UPDATE_BUCKET")
  : [];
config.r2_buckets = updateBucket
  ? [...retainedR2Buckets, {
    binding: "UPDATE_BUCKET",
    bucket_name: updateBucket,
  }]
  : retainedR2Buckets;
if (!config.r2_buckets.length) delete config.r2_buckets;
const controlPlugIds = legacyControlPlugIds();
if (controlPlugIds.length) {
  config.vars ??= {};
  config.vars.SWITCHBOT_CONTROL_PLUG_IDS = controlPlugIds.join(",");
  console.log(`Reusing ${controlPlugIds.length} SwitchBot control plug ID(s) from config.json`);
}
writeFileSync(generatedConfig, `${JSON.stringify(config, null, 2)}\n`);
console.log(`Using Worker '${workerName}' and D1 '${databaseName}' (${databaseId})`);
if (updateBucket) console.log(`Using R2 bucket '${updateBucket}' for update assets`);
if (retainedR2Buckets.length) console.log(`Preserving ${retainedR2Buckets.length} non-update R2 binding(s)`);
console.log(`Generated config paths: main=${config.main}, migrations=${config.d1_databases[0].migrations_dir}`);
if (config.assets?.directory) console.log(`Generated assets path: ${config.assets.directory}`);
console.log("Local .env files are ignored; runtime credentials remain in Cloudflare secrets");

if (process.argv.includes("--prepare-only")) process.exit(0);
if (migrateLocal) {
  wrangler(["d1", "migrations", "apply", databaseName, "--local", "--config", generatedConfig]);
  process.exit(0);
}
if (previewBuild) {
  console.log(`Preview branch '${buildBranch}': skipping production D1 migrations`);
  wrangler(["versions", "upload", "--config", generatedConfig]);
  process.exit(0);
}

const applyRemoteMigrations = !skipRemoteMigrations && (
  migrateRemoteOnly || forceRemoteMigrations || !cloudflareManagedBuild
);
if (applyRemoteMigrations) {
  wrangler(["d1", "migrations", "apply", databaseName, "--remote", "--config", generatedConfig]);
} else if (skipRemoteMigrations) {
  console.log("Routine Worker deploy: skipping remote D1 migration discovery");
  console.log("Remote migrations are applied by the dedicated Apply D1 migrations workflow");
} else {
  console.log("Cloudflare managed production build: skipping routine remote migration discovery");
  console.log("Remote migrations are applied by the dedicated Apply D1 migrations workflow");
}
if (migrateRemoteOnly) process.exit(0);
const deployArgs = ["deploy", "--config", generatedConfig];
if (secretsFile) {
  deployArgs.push("--secrets-file", secretsFile);
  console.log(`Bootstrapping runtime secrets from ${secretsFile}`);
}
wrangler(deployArgs);
console.log("Cloudflare runtime secrets were left unchanged unless HOMEPANEL_SECRETS_FILE was supplied");
