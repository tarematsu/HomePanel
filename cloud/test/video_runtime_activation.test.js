import { readFile } from 'node:fs/promises';
import { describe, expect, it, vi } from 'vitest';

import {
  inactiveVideoRuntimeResponse,
  videoRuntimeActive
} from '../src/video_runtime_activation.js';

function databaseWith(value) {
  const first = vi.fn().mockResolvedValue({ active: value });
  const prepare = vi.fn().mockReturnValue({ first });
  return { database: { prepare }, prepare, first };
}

describe('video runtime activation', () => {
  it('activates only when the migration marker is one', async () => {
    const enabled = databaseWith(1);
    const disabled = databaseWith(0);

    await expect(videoRuntimeActive({ DB: enabled.database }, 1)).resolves.toBe(true);
    await expect(videoRuntimeActive({ DB: disabled.database }, 1)).resolves.toBe(false);
    expect(enabled.prepare).toHaveBeenCalledWith(
      'SELECT active FROM video_runtime_state WHERE id = 1'
    );
  });

  it('fails closed when the activation state cannot be read', async () => {
    const database = {
      prepare() {
        return {
          async first() {
            throw new Error('missing table');
          }
        };
      }
    };

    await expect(videoRuntimeActive({ DB: database }, 1)).resolves.toBe(false);
  });

  it('returns a non-cacheable retry response before migration', async () => {
    const response = inactiveVideoRuntimeResponse();

    expect(response.status).toBe(503);
    expect(response.headers.get('cache-control')).toBe('no-store');
    expect(response.headers.get('retry-after')).toBe('60');
    await expect(response.json()).resolves.toMatchObject({
      ok: false,
      retryable: true
    });
  });

  it('gates video fetch, queue, and scheduled handlers', async () => {
    const source = await readFile(new URL('../src/unified_worker.js', import.meta.url), 'utf8');
    expect(source.match(/videoRuntimeActive\(env\)/g)).toHaveLength(3);
    expect(source).toMatch(/inactiveVideoRuntimeResponse\(\)/);
    expect(source).toMatch(/video-runtime-inactive-queue-retried/);
    expect(source).toMatch(/batch\.retryAll\(\)/);
    expect(source).toMatch(/video-runtime-inactive-scheduled-skipped/);
  });
});
