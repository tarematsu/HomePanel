import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

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
const previewBuild = process.env.WORKERS_CI === "1" && Boolean(buildBranch) && buildBranch !== productionBranch;
const migrateLocal = process.argv.includes("--migrate-local");

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

function stripJsonc(text) {
  let output = "";
  let inString = false;
  let quote = "";
  let escaped = false;
  for (let index = 0; index < text.length; index += 1) {
    const char = text[index];
    const next = text[index + 1];
    if (inString) {
      output += char;
      if (escaped) {
        escaped = false;
      } else if (char === "\\") {
        escaped = true;
      } else if (char === quote) {
        inString = false;
      }
      continue;
    }
    if (char === '"' || char === "'") {
      inString = true;
      quote = char;
      output += char;
      continue;
    }
    if (char === "/" && next === "/") {
      while (index < text.length && text[index] !== "\n") index += 1;
      output += "\n";
      continue;
    }
    if (char === "/" && next === "*") {
      index += 2;
      while (index < text.length && !(text[index] === "*" && text[index + 1] === "/")) index += 1;
      index += 1;
      output += " ";
      continue;
    }
    output += char;
  }
  return output.replace(/,\s*([}\]])/g, "$1");
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
config.d1_databases = [{
  binding: "DB",
  database_name: databaseName,
  database_id: databaseId,
  migrations_dir: generatedPath(migrationsDir),
}];
if (updateBucket) {
  config.r2_buckets = [{
    binding: "UPDATE_BUCKET",
    bucket_name: updateBucket,
  }];
}
const controlPlugIds = legacyControlPlugIds();
if (controlPlugIds.length) {
  config.vars ??= {};
  config.vars.SWITCHBOT_CONTROL_PLUG_IDS = controlPlugIds.join(",");
  console.log(`Reusing ${controlPlugIds.length} SwitchBot control plug ID(s) from config.json`);
}
writeFileSync(generatedConfig, `${JSON.stringify(config, null, 2)}\n`);
console.log(`Using Worker '${workerName}' and D1 '${databaseName}' (${databaseId})`);
if (updateBucket) console.log(`Using R2 bucket '${updateBucket}' for update assets`);
console.log(`Generated config paths: main=${config.main}, migrations=${config.d1_databases[0].migrations_dir}`);
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

wrangler(["d1", "migrations", "apply", databaseName, "--remote", "--config", generatedConfig]);
if (process.argv.includes("--migrate-only")) process.exit(0);
wrangler(["deploy", "--config", generatedConfig]);
console.log("Cloudflare runtime secrets were left unchanged");
