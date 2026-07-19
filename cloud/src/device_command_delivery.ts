import type { Env } from "./sources";

export const COMMAND_REDELIVERY_MS = 90_000;

interface DeviceCommandRow {
  id: number;
  command: string;
  payload: string | null;
  created_at: number;
  expires_at: number | null;
}

export async function claimPendingDeviceCommands(
  env: Env,
  deviceId: string,
  now = Date.now(),
): Promise<Array<Record<string, unknown>>> {
  const rows = await env.DB.prepare(
    `WITH pending AS (
       SELECT id
         FROM device_commands
        WHERE device_id=?1
          AND completed_at IS NULL
          AND (expires_at IS NULL OR expires_at>?2)
          AND (delivered_at IS NULL OR delivered_at<=?3)
        ORDER BY id
        LIMIT 10
     )
     UPDATE device_commands
        SET delivered_at=?2
      WHERE id IN (SELECT id FROM pending)
        AND device_id=?1
        AND completed_at IS NULL
        AND (expires_at IS NULL OR expires_at>?2)
        AND (delivered_at IS NULL OR delivered_at<=?3)
     RETURNING id, command, payload, created_at, expires_at`,
  ).bind(deviceId, now, now - COMMAND_REDELIVERY_MS).all<DeviceCommandRow>();

  const results = rows.results ?? [];
  results.sort((left, right) => Number(left.id) - Number(right.id));
  return results.map(row => {
    let payload: unknown = null;
    try { payload = row.payload ? JSON.parse(row.payload) : null; } catch { payload = null; }
    return {
      id: row.id,
      command: row.command,
      payload,
      createdAt: row.created_at,
      expiresAt: row.expires_at,
    };
  });
}
