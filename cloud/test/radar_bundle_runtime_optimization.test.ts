import { afterEach, describe, expect, it, vi } from "vitest";
import { cachedRadarBundleResponse } from "../src/radar_bundle_cache";
import { prewarmRadarBundle } from "../src/radar_bundle_prewarm";
import type { Env } from "../src/sources";

const baseTime = "20260723003000";

type PutOptions = {
  customMetadata: { baseTime: string; records: string };
};

function payload() {
  return {
    frames: [{
      baseTime,
      tiles: [
        { url: `/v1/radar/tile/jma/${baseTime}/${baseTime}/10/908/403.png` },
        { url: `/v1/radar/tile/jma/${baseTime}/20260723003500/10/909/403.png` },
      ],
    }],
  };
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("radar bundle runtime optimization", () => {
  it("prewarms the latest R2 bundle before radar state publication", async () => {
    const shard = new Uint8Array([10, 20, 30, 40]);
    const shardFetch = vi.fn(async (_input: RequestInfo | URL, _init?: RequestInit) => new Response(shard, {
      headers: {
        "Content-Length": String(shard.length),
        "X-HomePanel-Radar-Shard-Bytes": String(shard.length),
      },
    }));
    const namespace = {
      idFromName: vi.fn((name: string) => name),
      get: vi.fn(() => ({ fetch: shardFetch })),
    };
    const put = vi.fn(async (_key: string, _body: Uint8Array, _options: PutOptions) => undefined);
    const env = {
      DATA_BUCKET: { put },
      SCHEDULER_COORDINATOR: namespace,
    } as unknown as Env;

    await expect(prewarmRadarBundle(env, payload(), baseTime)).resolves.toBe(true);
    expect(shardFetch).toHaveBeenCalledTimes(1);
    expect(put).toHaveBeenCalledTimes(1);
    const [key, body, options] = put.mock.calls[0]!;
    expect(key).toBe("radar-bundles/v1/latest.hpb");
    expect(body).toBeInstanceOf(Uint8Array);
    const bytes = body as Uint8Array;
    expect(new TextDecoder().decode(bytes.slice(0, 8))).toBe("HPRB0001");
    expect(bytes[8]).toBe(2);
    expect(bytes.slice(12)).toEqual(shard);
    expect(options.customMetadata).toEqual({ baseTime, records: "2" });
  });

  it("serves a matching R2 bundle before the D1 fallback path", async () => {
    const cacheMatch = vi.fn(async (_request: Request) => undefined);
    const cachePut = vi.fn(async (_request: Request, _response: Response) => undefined);
    vi.stubGlobal("caches", { default: { match: cacheMatch, put: cachePut } });

    const bodyBytes = new Uint8Array([1, 2, 3, 4]);
    const get = vi.fn(async (_key: string) => ({
      body: new Response(bodyBytes).body!,
      size: bodyBytes.length,
      customMetadata: { baseTime, records: "2" },
    }));
    const pending: Promise<unknown>[] = [];
    const ctx = {
      waitUntil(promise: Promise<unknown>) {
        pending.push(promise);
      },
    } as ExecutionContext;
    const env = { DATA_BUCKET: { get } } as unknown as Env;

    const response = await cachedRadarBundleResponse(
      new Request(`https://example.test/v1/radar/bundle/${baseTime}.hpb`),
      env,
      ctx,
    );

    expect(response?.status).toBe(200);
    expect(response?.headers.get("X-HomePanel-Radar-Records")).toBe("2");
    expect(new Uint8Array(await response!.arrayBuffer())).toEqual(bodyBytes);
    await Promise.all(pending);
    expect(cacheMatch).toHaveBeenCalledTimes(1);
    expect(get).toHaveBeenCalledWith("radar-bundles/v1/latest.hpb");
    expect(cachePut).toHaveBeenCalledTimes(1);
  });
});
