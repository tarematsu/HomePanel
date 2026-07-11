import { applyD1Migrations, env } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { fetchOctopus } from "../src/octopus_source";
import { fetchStationhead } from "../src/spotify_source";
import type { Env } from "../src/sources";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

const baseEnv = {
  DB: env.DB,
  CITY_NAME: "Kawagoe",
  WEATHERNEWS_URL: "",
  STATIONHEAD_MONITOR_URL: "",
} satisfies Env;

function jsonResponse(value: unknown): Response {
  return new Response(JSON.stringify(value), {
    headers: { "Content-Type": "application/json" },
  });
}

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

describe("cloud sources", () => {
  it("stores stable Octopus readings while keeping the latest 48 hours live only", async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date("2026-07-10T18:00:00Z"));
    const requests: Request[] = [];
    const readingRanges: Array<{ accountNumber?: string; fromDatetime?: string; toDatetime?: string }> = [];
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const request = new Request(input, init);
      requests.push(request);
      const body = await request.clone().json() as {
        query?: string;
        variables?: { accountNumber?: string; fromDatetime?: string; toDatetime?: string };
      };
      if (body.query?.includes("obtainKrakenToken")) {
        return jsonResponse({
          data: { obtainKrakenToken: { token: "octopus-token", refreshToken: "refresh-token", refreshExpiresIn: 4_000_000_000 } },
        });
      }
      expect(request.headers.get("Authorization")).toBe("octopus-token");
      readingRanges.push(body.variables ?? {});
      const from = Date.parse(body.variables?.fromDatetime ?? "");
      return jsonResponse({
        data: {
          account: {
            properties: [{
              electricitySupplyPoints: [{
                spin: "spin-1",
                status: "Active",
                halfHourlyReadings: Number.isFinite(from)
                  ? [{ startAt: new Date(from + 30 * 60_000).toISOString(), value: "0.5" }]
                  : [],
              }],
            }],
          },
        },
      });
    }));

    const result = await fetchOctopus({
      ...baseEnv,
      OCTOPUS_EMAIL: "user@example.test",
      OCTOPUS_PASSWORD: "password",
      OCTOPUS_ACCOUNT: "A-123",
    } as Env & { OCTOPUS_ACCOUNT: string });

    expect(result.source).toBe("octopus");
    expect(requests.filter(request => request.headers.get("Authorization") === "octopus-token").length)
      .toBeGreaterThanOrEqual(24);
    expect(readingRanges).toEqual(expect.arrayContaining([
      expect.objectContaining({
        fromDatetime: "2025-07-06T15:00:00.000Z",
        toDatetime: "2025-07-08T15:00:00.000Z",
      }),
    ]));

    const payload = result.payload as {
      archive: { stableThrough: number; excludedRecentDays: number };
    };
    expect(payload.archive.excludedRecentDays).toBe(2);
    expect(new Date(payload.archive.stableThrough).toISOString()).toBe("2026-07-08T18:00:00.000Z");
    const stored = await env.DB.prepare(
      "SELECT COUNT(*) AS count,MAX(observed_at) AS latest FROM octopus_readings WHERE account_number=?1",
    ).bind("A-123").first<{ count: number; latest: number }>();
    expect(Number(stored?.count)).toBeGreaterThan(0);
    expect(Number(stored?.latest)).toBeLessThan(payload.archive.stableThrough);
  });

  it("preserves Stationhead monitor thumbnails as Spotify artwork", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => jsonResponse({
      ok: true,
      generated_at: Date.now(),
      playing: true,
      queue_status: { current_index: 0, progress_ms: 1000, anchor_at: Date.now() - 1000 },
      queue: [{
        position: 0,
        spotify_id: "track-1",
        title: "Track",
        artist: "Artist",
        thumbnail_url: "https://i.scdn.co/image/test",
        duration_ms: 180000,
        is_current: true,
      }],
    })));

    const result = await fetchStationhead({
      ...baseEnv,
      STATIONHEAD_MONITOR_URL: "https://monitor.example/api/dashboard",
    });
    const payload = result.payload as { item?: { artwork?: string } };

    expect(payload.item?.artwork).toBe("https://i.scdn.co/image/test");
  });
});
