import { describe, expect, it } from "vitest";
import { radarBundlePaths } from "../src/radar_bundle";
import { radarTileTargetForPath } from "../src/radar_tile";

const baseTime = "20260720070000";

function tile(validTime: string, x: number): string {
  return `/v1/radar/tile/jma/${baseTime}/${validTime}/10/${x}/403.png?expires=9999999999&signature=test`;
}

describe("radar bundle path selection", () => {
  it("collects unique JMA tile paths for the requested base time", () => {
    const first = tile(baseTime, 908);
    const second = tile("20260720070500", 909);
    expect(radarBundlePaths({
      frames: [
        { baseTime, tiles: [{ url: first }, { url: second }] },
        { baseTime, tiles: [{ url: first }] },
        { baseTime: "20260720065500", tiles: [{ url: "/v1/radar/tile/jma/20260720065500/20260720065500/10/908/403.png" }] },
      ],
    }, baseTime)).toEqual([
      new URL(first, "https://test.invalid").pathname,
      new URL(second, "https://test.invalid").pathname,
    ]);
  });

  it("rejects unsupported, absolute, and malformed tile URLs", () => {
    expect(radarBundlePaths({
      frames: [{
        baseTime,
        tiles: [
          { url: "https://www.jma.go.jp/tile.png" },
          { url: "/v1/radar/tile/gsi/10/908/403.png" },
          { url: "/v1/radar/tile/jma/not-a-time/20260720070000/10/908/403.png" },
        ],
      }],
    }, baseTime)).toEqual([]);
  });

  it("maps valid JMA paths to their public upstream target", () => {
    expect(radarTileTargetForPath(
      `/v1/radar/tile/jma/${baseTime}/${baseTime}/10/908/403.png`,
    )).toEqual({
      upstream: `https://www.jma.go.jp/bosai/jmatile/data/nowc/${baseTime}/none/${baseTime}/surf/hrpns/10/908/403.png`,
      ttl: 300,
    });
  });
});
