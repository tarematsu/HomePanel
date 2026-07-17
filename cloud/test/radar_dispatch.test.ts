import { env } from "cloudflare:test";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { dispatchRadarBuildIfStale } from "../src/radar_dispatch";
import type { Env } from "../src/sources";

const MANIFEST_KEY = "radar/manifest.json";
const MARKER_KEY = "radar/dispatch.json";
const NOW = Date.UTC(2026, 6, 18, 1, 30, 0);

function testEnv(): Env {
  return {
    ...(env as unknown as Env),
    GITHUB_RADAR_DISPATCH_TOKEN: "test-github-token",
  };
}

async function clearRadarObjects(): Promise<void> {
  const bucket = (env as unknown as Env).UPDATE_BUCKET!;
  let cursor: string | undefined;
  do {
    const listed = cursor
      ? await bucket.list({ prefix: "radar/", cursor })
      : await bucket.list({ prefix: "radar/" });
    const keys = listed.objects.map(object => object.key);
    if (keys.length) await bucket.delete(keys);
    cursor = listed.truncated ? listed.cursor : undefined;
  } while (cursor);
}

beforeEach(clearRadarObjects);

describe("Cloudflare-triggered radar build dispatch", () => {
  it("does not dispatch while the R2 manifest is fresh", async () => {
    const bucket = (env as unknown as Env).UPDATE_BUCKET!;
    await bucket.put(MANIFEST_KEY, JSON.stringify({ generatedAt: new Date(NOW - 2 * 60_000).toISOString() }));
    const fetcher = vi.fn<typeof fetch>();

    const result = await dispatchRadarBuildIfStale(testEnv(), {
      now: () => NOW,
      fetcher,
    });

    expect(result).toEqual({ status: "fresh", manifestAgeMs: 2 * 60_000 });
    expect(fetcher).not.toHaveBeenCalled();
    expect(await bucket.get(MARKER_KEY)).toBeNull();
  });

  it("dispatches the reusable GitHub workflow when the manifest is stale", async () => {
    const bucket = (env as unknown as Env).UPDATE_BUCKET!;
    await bucket.put(MANIFEST_KEY, JSON.stringify({ generatedAt: new Date(NOW - 9 * 60_000).toISOString() }));
    const fetcher = vi.fn<typeof fetch>(async () => new Response(null, { status: 204 }));

    const result = await dispatchRadarBuildIfStale(testEnv(), {
      now: () => NOW,
      fetcher,
    });

    expect(result).toEqual({ status: "dispatched", manifestAgeMs: 9 * 60_000, dispatchedAt: NOW });
    expect(fetcher).toHaveBeenCalledOnce();
    const [url, init] = fetcher.mock.calls[0]!;
    expect(url).toBe("https://api.github.com/repos/tarematsu/HP/actions/workflows/radar-frames.yml/dispatches");
    expect(init?.method).toBe("POST");
    expect(new Headers(init?.headers).get("Authorization")).toBe("Bearer test-github-token");
    expect(JSON.parse(String(init?.body))).toEqual({ ref: "main" });

    const markerObject = await bucket.get(MARKER_KEY);
    expect(markerObject).not.toBeNull();
    const marker = JSON.parse(await markerObject!.text()) as Record<string, unknown>;
    expect(marker).toMatchObject({
      dispatchedAt: NOW,
      manifestAgeMs: 9 * 60_000,
      repository: "tarematsu/HP",
      workflow: "radar-frames.yml",
      ref: "main",
    });
  });

  it("uses the R2 marker to suppress duplicate dispatches during cooldown", async () => {
    const bucket = (env as unknown as Env).UPDATE_BUCKET!;
    await bucket.put(MANIFEST_KEY, JSON.stringify({ generatedAt: new Date(NOW - 20 * 60_000).toISOString() }));
    await bucket.put(MARKER_KEY, JSON.stringify({ dispatchedAt: NOW - 3 * 60_000 }));
    const fetcher = vi.fn<typeof fetch>();

    const result = await dispatchRadarBuildIfStale(testEnv(), {
      now: () => NOW,
      fetcher,
    });

    expect(result).toEqual({
      status: "cooldown",
      manifestAgeMs: 20 * 60_000,
      cooldownRemainingMs: 7 * 60_000,
    });
    expect(fetcher).not.toHaveBeenCalled();
  });

  it("does not write a cooldown marker when GitHub rejects the dispatch", async () => {
    const bucket = (env as unknown as Env).UPDATE_BUCKET!;
    const fetcher = vi.fn<typeof fetch>(async () => Response.json(
      { message: "Bad credentials" },
      { status: 401 },
    ));

    await expect(dispatchRadarBuildIfStale(testEnv(), {
      now: () => NOW,
      fetcher,
    })).rejects.toThrow("HTTP 401");

    expect(await bucket.get(MARKER_KEY)).toBeNull();
  });
});
