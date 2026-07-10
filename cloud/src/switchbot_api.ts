import { fetchJson } from "./http";
import { cachedHmacKey } from "./crypto_cache";
import { readState, type StateRow } from "./snapshot";
import type { DeviceState, SwitchBotEnv, SwitchBotState } from "./switchbot_types";

interface ApiResponse<T> {
  statusCode: number;
  body: T;
  message?: string;
}

export interface ApiDevice {
  deviceId: string;
  deviceName?: string;
  deviceType?: string;
  hubDeviceId?: string;
  enableCloudService?: boolean;
}

export interface SwitchBotSnapshot {
  row: StateRow | null;
  state: SwitchBotState | null;
}

export function configuredIds(value?: string): string[] {
  return [...new Set((value ?? "").split(/[\s,;]+/).map(item => item.trim()).filter(Boolean))];
}

export function numberValue(value: unknown): number | null {
  if (value === null || value === undefined || value === "") return null;
  const result = Number(value);
  return Number.isFinite(result) ? result : null;
}

function stringValue(value: unknown): string | null {
  return typeof value === "string" && value.length ? value : null;
}

function booleanValue(value: unknown): boolean | null {
  return typeof value === "boolean" ? value : null;
}

async function signedHeaders(env: SwitchBotEnv): Promise<Headers> {
  const token = env.SWITCHBOT_TOKEN?.trim();
  const secret = env.SWITCHBOT_SECRET?.trim();
  if (!token || !secret) throw new Error("SwitchBot secrets are not configured");
  const timestamp = String(Date.now());
  const nonce = crypto.randomUUID();
  const input = `${token}${timestamp}${nonce}`;
  const signature = await crypto.subtle.sign("HMAC", await cachedHmacKey(secret), new TextEncoder().encode(input));
  return new Headers({
    Authorization: token,
    sign: btoa(String.fromCharCode(...new Uint8Array(signature))),
    nonce,
    t: timestamp,
    "Content-Type": "application/json",
  });
}

export async function switchBotApi<T>(env: SwitchBotEnv, path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetchJson<ApiResponse<T>>(`https://api.switch-bot.com/v1.1${path}`, {
    ...init,
    headers: await signedHeaders(env),
  });
  if (response.statusCode !== 100) throw new Error(`SwitchBot API ${response.statusCode}: ${response.message ?? "unknown error"}`);
  return response.body;
}

export async function sendSwitchBotCommand(env: SwitchBotEnv, deviceId: string, command: "turnOn" | "turnOff"): Promise<void> {
  await switchBotApi(env, `/devices/${encodeURIComponent(deviceId)}/commands`, {
    method: "POST",
    body: JSON.stringify({ command, parameter: "default", commandType: "command" }),
  });
}

export function normalizeDevice(device: ApiDevice, status: Record<string, unknown>, previous: DeviceState | null): DeviceState {
  const detected = booleanValue(status.moveDetected) ?? booleanValue(status.Detected);
  return {
    deviceId: device.deviceId,
    deviceName: device.deviceName ?? previous?.deviceName ?? device.deviceId,
    deviceType: device.deviceType ?? previous?.deviceType ?? "Unknown",
    hubDeviceId: device.hubDeviceId ?? previous?.hubDeviceId ?? null,
    cloudEnabled: device.enableCloudService ?? previous?.cloudEnabled ?? null,
    battery: numberValue(status.battery) ?? previous?.battery ?? null,
    motion: detected ?? previous?.motion ?? null,
    openState: stringValue(status.openState) ?? previous?.openState ?? null,
    doorMode: stringValue(status.doorMode) ?? previous?.doorMode ?? null,
    brightness: stringValue(status.brightness) ?? previous?.brightness ?? null,
    power: stringValue(status.power) ?? stringValue(status.powerState) ?? previous?.power ?? null,
    watts: numberValue(status.weight) ?? numberValue(status.watts) ?? previous?.watts ?? null,
    voltage: numberValue(status.voltage) ?? previous?.voltage ?? null,
    electricCurrent: numberValue(status.electricCurrent) ?? previous?.electricCurrent ?? null,
    onlineStatus: stringValue(status.onlineStatus) ?? previous?.onlineStatus ?? null,
    observedAt: Date.now(),
    error: null,
  };
}

export async function loadSwitchBotSnapshot(env: SwitchBotEnv): Promise<SwitchBotSnapshot> {
  const row = await readState(env, "switchbot");
  if (!row) return { row: null, state: null };
  try {
    return { row, state: JSON.parse(row.payload) as SwitchBotState };
  } catch {
    return { row, state: null };
  }
}
