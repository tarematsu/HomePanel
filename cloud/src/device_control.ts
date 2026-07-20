import type { Env } from "./sources";
import { DEVICE_ID_PATTERN, deviceIdFromRequest as deviceIdFrom } from "./auth";
import { claimPendingDeviceCommands } from "./device_command_delivery";
import { json } from "./http";

const ALLOWED_COMMANDS = new Set(["check_update"]);

interface DeviceConfigRow {
  version: number;
  payload: string;
  updated_at: number;
}

function configEtag(deviceId: string, version: number): string {
  return `"device-config-${deviceId}-${version}"`;
}

function etagHeaderIncludes(value: string | null, tag: string): boolean {
  if (value === null) return false;
  let start = 0;
  while (start <= value.length) {
    const delimiter = value.indexOf(",", start);
    const final = delimiter < 0;
    let end = final ? value.length : delimiter;
    while (start < end && (value.charCodeAt(start) === 32 || value.charCodeAt(start) === 9)) start += 1;
    while (end > start && (value.charCodeAt(end - 1) === 32 || value.charCodeAt(end - 1) === 9)) end -= 1;
    if (end - start === tag.length && value.startsWith(tag, start)) return true;
    if (final) return false;
    start = delimiter + 1;
  }
  return false;
}

function objectOrNull(value: unknown): Record<string, unknown> | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function canonicalValue(value: unknown): unknown {
  if (Array.isArray(value)) {
    const normalized = new Array<unknown>(value.length);
    for (let index = 0; index < value.length; index += 1) normalized[index] = canonicalValue(value[index]);
    return normalized;
  }
  if (value && typeof value === "object") {
    const object = value as Record<string, unknown>;
    const normalized = Object.create(null) as Record<string, unknown>;
    for (const key of Object.keys(object).sort()) normalized[key] = canonicalValue(object[key]);
    return normalized;
  }
  return value;
}

function canonicalJson(value: unknown): string {
  return JSON.stringify(canonicalValue(value));
}

export async function enqueueCommandOnce(
  env: Env,
  deviceId: string,
  command: string,
  serialized: string | null,
  expiresInSeconds: number,
): Promise<{ id: number; deduplicated: boolean }> {
  const now = Date.now();
  const expiresAt = now + Math.max(60, Math.min(86_400, expiresInSeconds)) * 1000;
  for (let attempt = 0; attempt < 2; attempt += 1) {
    const inserted = await env.DB.prepare(
      `INSERT INTO device_commands(device_id, command, payload, created_at, expires_at)
       SELECT ?1, ?2, ?3, ?4, ?5
        WHERE NOT EXISTS (
          SELECT 1 FROM device_commands
           WHERE device_id=?1 AND command=?2 AND payload IS ?3
             AND completed_at IS NULL AND (expires_at IS NULL OR expires_at>?4)
        ) RETURNING id`,
    ).bind(deviceId, command, serialized, now, expiresAt).all<{ id: number }>();
    const insertedId = Number(inserted.results?.[0]?.id ?? 0);
    if (insertedId > 0) return { id: insertedId, deduplicated: false };
    const existing = await env.DB.prepare(
      `SELECT id FROM device_commands
        WHERE device_id=?1 AND command=?2 AND payload IS ?3
          AND completed_at IS NULL AND (expires_at IS NULL OR expires_at>?4)
        ORDER BY id DESC LIMIT 1`,
    ).bind(deviceId, command, serialized, now).first<{ id: number }>();
    const existingId = Number(existing?.id ?? 0);
    if (existingId > 0) return { id: existingId, deduplicated: true };
  }
  throw new Error("device command could not be queued");
}

export async function getDeviceConfig(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  const row = await env.DB.prepare(
    "SELECT version, payload, updated_at FROM device_configs WHERE device_id=?1",
  ).bind(deviceId).first<DeviceConfigRow>();
  const version = row?.version ?? 0;
  let config: unknown = {};
  if (row?.payload) {
    try { config = JSON.parse(row.payload); } catch { config = {}; }
  }
  const body = JSON.stringify({ deviceId, version, updatedAt: row?.updated_at ?? 0, config });
  const etag = configEtag(deviceId, version);
  const headers = {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "private, max-age=0, must-revalidate",
    ETag: etag,
  };
  if (etagHeaderIncludes(request.headers.get("If-None-Match"), etag)) return new Response(null, { status: 304, headers });
  return new Response(body, { status: 200, headers });
}

export async function putDeviceConfig(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  let input: unknown;
  try { input = await request.json(); } catch { return json({ error: "invalid json" }, { status: 400 }); }
  const config = objectOrNull(input);
  if (!config) return json({ error: "config must be an object" }, { status: 400 });
  const payload = canonicalJson(config);
  if (payload.length > 32_000) return json({ error: "config is too large" }, { status: 413 });

  const existing = await env.DB.prepare(
    "SELECT version, payload, updated_at FROM device_configs WHERE device_id=?1",
  ).bind(deviceId).first<DeviceConfigRow>();
  const currentVersion = Number(existing?.version ?? 0);
  const currentEtag = configEtag(deviceId, currentVersion);
  const suppliedEtags = request.headers.get("If-Match");
  if (suppliedEtags === null) {
    return json({ error: "device config precondition required", deviceId, currentVersion },
      { status: 428, headers: { ETag: currentEtag } });
  }
  if (!etagHeaderIncludes(suppliedEtags, currentEtag)) {
    return json({ error: "device config changed; reload before saving", deviceId, currentVersion },
      { status: 412, headers: { ETag: currentEtag } });
  }
  if (existing?.payload === payload) {
    return json({ saved: true, changed: false, deviceId, version: currentVersion, updatedAt: existing.updated_at },
      { headers: { ETag: currentEtag } });
  }

  const now = Date.now();
  const nextVersion = currentVersion + 1;
  const statement = existing
    ? env.DB.prepare(
      `UPDATE device_configs SET version=?2, payload=?3, updated_at=?4
        WHERE device_id=?1 AND version=?5`,
    ).bind(deviceId, nextVersion, payload, now, currentVersion)
    : env.DB.prepare(
      `INSERT OR IGNORE INTO device_configs(device_id, version, payload, updated_at)
       VALUES(?1, 1, ?2, ?3)`,
    ).bind(deviceId, payload, now);
  const result = await statement.run();
  if (Number(result.meta.changes ?? 0) !== 1) {
    const latest = await env.DB.prepare(
      "SELECT version FROM device_configs WHERE device_id=?1",
    ).bind(deviceId).first<{ version: number }>();
    const latestVersion = Number(latest?.version ?? 0);
    return json({ error: "device config changed; reload before saving", deviceId, currentVersion: latestVersion },
      { status: 412, headers: { ETag: configEtag(deviceId, latestVersion) } });
  }
  return json({ saved: true, changed: true, deviceId, version: nextVersion, updatedAt: now },
    { headers: { ETag: configEtag(deviceId, nextVersion) } });
}

export async function getDeviceCommands(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  return json({ deviceId, commands: await claimPendingDeviceCommands(env, deviceId) });
}

export async function createDeviceCommand(request: Request, env: Env): Promise<Response> {
  let input: Record<string, unknown>;
  try { input = objectOrNull(await request.json()) ?? {}; } catch { return json({ error: "invalid json" }, { status: 400 }); }
  const deviceId = String(input.deviceId ?? "").trim();
  const command = String(input.command ?? "").trim();
  if (!DEVICE_ID_PATTERN.test(deviceId) || !ALLOWED_COMMANDS.has(command)) {
    return json({ error: "invalid deviceId or command" }, { status: 400 });
  }
  const payload = input.payload ?? null;
  const serialized = payload === null ? null : canonicalJson(payload);
  if (serialized && serialized.length > 8_000) return json({ error: "command payload is too large" }, { status: 413 });
  const expiresInSeconds = Number(input.expiresInSeconds) || 3600;
  const queued = await enqueueCommandOnce(env, deviceId, command, serialized, expiresInSeconds);
  return json({ queued: true, ...queued, deviceId, command }, { status: 202 });
}

export async function acknowledgeDeviceCommand(request: Request, env: Env): Promise<Response> {
  const deviceId = deviceIdFrom(request);
  if (!deviceId) return json({ error: "valid deviceId is required" }, { status: 400 });
  let input: Record<string, unknown>;
  try { input = objectOrNull(await request.json()) ?? {}; } catch { return json({ error: "invalid json" }, { status: 400 }); }
  const id = input.id;
  if (typeof id !== "number" || !Number.isSafeInteger(id) || id <= 0 || typeof input.success !== "boolean") {
    return json({ error: "valid command id and boolean success are required" }, { status: 400 });
  }
  if (input.result !== undefined && input.result !== null && typeof input.result !== "string") {
    return json({ error: "command result must be a string" }, { status: 400 });
  }
  const result = typeof input.result === "string" ? input.result.slice(0, 1000) : "";
  const update = await env.DB.prepare(
    `UPDATE device_commands SET completed_at=?1, success=?2, result=?3
      WHERE id=?4 AND device_id=?5 AND completed_at IS NULL`,
  ).bind(Date.now(), input.success ? 1 : 0, result || null, id, deviceId).run();
  return json({ acknowledged: (update.meta.changes ?? 0) === 1 });
}
