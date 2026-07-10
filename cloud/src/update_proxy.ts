import type { Env } from "./sources";
import { cachedHmacKey, constantTimeEqual } from "./crypto_cache";

const SIGNED_URL_LIFETIME_SECONDS = 600;
const ALLOWED_FILES = new Set(["HomePanel.exe", "HomePanelUpdater.exe", "WebView2Loader.dll", "update-manifest.json"]);
const REQUIRED_UPDATE_FILES = ["HomePanel.exe", "HomePanelUpdater.exe", "WebView2Loader.dll"] as const;
const RELEASE_VERSION_PATTERN = /^[0-9]{10}$/;
const SHA256_PATTERN = /^[0-9a-f]{64}$/i;
const DEFAULT_UPDATE_PREFIX = "updates";

interface UpdateFile { name: string; sha256: string; size: number; requireAuthenticode?: boolean; url?: string }
interface UpdateManifest { version: string; signed?: boolean; files: UpdateFile[] }

function json(value: unknown, status = 200): Response {
  return new Response(JSON.stringify(value), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
    },
  });
}

function signingSecret(env: Env): string {
  return env.UPDATE_SIGNING_SECRET?.trim() || env.HOMEPANEL_INGEST_SECRET || env.DEVICE_TOKEN || "";
}

async function signature(secret: string, version: string, name: string, expires: number): Promise<string> {
  const digest = await crypto.subtle.sign(
    "HMAC",
    await cachedHmacKey(secret),
    new TextEncoder().encode(`${version}\n${name}\n${expires}`),
  );
  return [...new Uint8Array(digest)].map(value => value.toString(16).padStart(2, "0")).join("");
}

async function sha256(value: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, "0")).join("");
}

function updatePrefix(env: Env): string {
  return (env.UPDATE_BUCKET_PREFIX?.trim() || DEFAULT_UPDATE_PREFIX).replace(/^\/+|\/+$/g, "");
}

function updateKey(env: Env, suffix: string): string {
  const prefix = updatePrefix(env);
  return prefix ? `${prefix}/${suffix}` : suffix;
}

function updateBucket(env: Env): R2Bucket {
  if (!env.UPDATE_BUCKET) throw new Error("update bucket unavailable");
  return env.UPDATE_BUCKET;
}

async function readObjectText(env: Env, key: string): Promise<string> {
  const object = await updateBucket(env).get(key);
  if (!object) throw new Error(`object unavailable: ${key}`);
  return object.text();
}

async function readObject(env: Env, key: string): Promise<R2ObjectBody> {
  const object = await updateBucket(env).get(key);
  if (!object?.body) throw new Error(`object unavailable: ${key}`);
  return object;
}

function parseManifest(text: string): UpdateManifest {
  const manifest = JSON.parse(text) as UpdateManifest;
  if (!manifest.version || !RELEASE_VERSION_PATTERN.test(manifest.version) ||
      !Array.isArray(manifest.files) || manifest.files.length !== REQUIRED_UPDATE_FILES.length) {
    throw new Error("invalid manifest");
  }
  const seen = new Set<string>();
  for (const file of manifest.files) {
    if (!REQUIRED_UPDATE_FILES.includes(file.name as typeof REQUIRED_UPDATE_FILES[number]) ||
        seen.has(file.name) || !SHA256_PATTERN.test(String(file.sha256 ?? "")) ||
        !Number.isSafeInteger(file.size) || file.size <= 0 || file.size > 64 * 1024 * 1024) {
      throw new Error("invalid manifest file");
    }
    seen.add(file.name);
  }
  if (REQUIRED_UPDATE_FILES.some(name => !seen.has(name))) throw new Error("incomplete manifest");
  return manifest;
}

function canonicalManifest(manifest: UpdateManifest): string {
  return JSON.stringify({
    version: manifest.version,
    signed: manifest.signed === true,
    files: [...manifest.files]
      .map(file => ({
        name: file.name,
        sha256: file.sha256.toLowerCase(),
        size: file.size,
        requireAuthenticode: file.requireAuthenticode === true,
      }))
      .sort((left, right) => left.name.localeCompare(right.name)),
  });
}




export async function readUpdateManifestIdentity(env: Env): Promise<{ version: string; manifestHash: string }> {
  const manifest = parseManifest(await readObjectText(env, updateKey(env, "latest/update-manifest.json")));
  return { version: manifest.version, manifestHash: await sha256(canonicalManifest(manifest)) };
}

export async function readUpdateManifestVersion(env: Env): Promise<string> {
  return (await readUpdateManifestIdentity(env)).version;
}

function unavailable(kind: "manifest" | "file", error: unknown): Response {
  console.error(`update ${kind} unavailable`, error instanceof Error ? error.message : String(error));
  return json({ error: `update ${kind} unavailable` }, 503);
}

export async function updateManifestResponse(request: Request, env: Env): Promise<Response> {
  try {
    const secret = signingSecret(env);
    if (!secret) throw new Error("signing unavailable");
    const manifest = parseManifest(await readObjectText(env, updateKey(env, "latest/update-manifest.json")));

    const origin = new URL(request.url).origin;
    const expires = Math.floor(Date.now() / 1000) + SIGNED_URL_LIFETIME_SECONDS;
    for (const file of manifest.files) {
      const signed = await signature(secret, manifest.version, file.name, expires);
      file.url = `${origin}/v1/update/file/${encodeURIComponent(file.name)}?version=${encodeURIComponent(manifest.version)}&expires=${expires}&signature=${signed}`;
    }
    return json(manifest);
  } catch (error) {
    return unavailable("manifest", error);
  }
}

export async function updateFileResponse(request: Request, env: Env, name: string): Promise<Response> {
  try {
    if (!ALLOWED_FILES.has(name) || name === "update-manifest.json") return json({ error: "not found" }, 404);
    const secret = signingSecret(env);
    if (!secret) throw new Error("signing unavailable");
    const url = new URL(request.url);
    const version = url.searchParams.get("version")?.trim() ?? "";
    if (!RELEASE_VERSION_PATTERN.test(version)) return json({ error: "invalid release version" }, 400);
    const expires = Number(url.searchParams.get("expires"));
    const supplied = url.searchParams.get("signature") ?? "";
    const now = Math.floor(Date.now() / 1000);
    if (!Number.isSafeInteger(expires) || expires < now || expires > now + SIGNED_URL_LIFETIME_SECONDS + 60) {
      return json({ error: "update link expired" }, 403);
    }
    const expected = await signature(secret, version, name, expires);
    if (!constantTimeEqual(supplied, expected)) return json({ error: "invalid update signature" }, 403);

    const downloaded = await readObject(env, updateKey(env, `releases/${version}/${name}`));
    const headers = new Headers({
      "Content-Type": downloaded.httpMetadata?.contentType || "application/octet-stream",
      "Content-Disposition": `attachment; filename=\"${name}\"`,
      "Cache-Control": "private, no-store",
      "X-Content-Type-Options": "nosniff",
    });
    if (Number.isFinite(downloaded.size) && downloaded.size > 0) {
      headers.set("Content-Length", String(downloaded.size));
    }
    return new Response(downloaded.body, { status: 200, headers });
  } catch (error) {
    return unavailable("file", error);
  }
}
