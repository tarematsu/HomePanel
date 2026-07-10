import { describe, expect, it } from "vitest";
import { signedRadarTilePath, verifyRadarTileRequest } from "../src/radar_tile";
import type { Env } from "../src/sources";

const testEnv = { UPDATE_SIGNING_SECRET: "radar-test-secret" } as Env;
const pathname = "/v1/radar/tile/jma/20260711000000/20260711000500/10/900/400.png";

describe("radar tile signatures", () => {
  it("accepts an unmodified signed tile URL", async () => {
    const now = 1_783_700_000;
    const signed = await signedRadarTilePath(testEnv, pathname, now + 600);
    const request = new Request(`https://homepanel.test${signed}`);
    await expect(verifyRadarTileRequest(request, testEnv, now)).resolves.toBe(true);
  });

  it("rejects unsigned, tampered, and expired tile URLs", async () => {
    const now = 1_783_700_000;
    const signed = await signedRadarTilePath(testEnv, pathname, now + 600);
    await expect(verifyRadarTileRequest(
      new Request(`https://homepanel.test${pathname}`), testEnv, now,
    )).resolves.toBe(false);
    await expect(verifyRadarTileRequest(
      new Request(`https://homepanel.test${signed.replace("/900/", "/901/")}`), testEnv, now,
    )).resolves.toBe(false);
    const expired = await signedRadarTilePath(testEnv, pathname, now - 1);
    await expect(verifyRadarTileRequest(
      new Request(`https://homepanel.test${expired}`), testEnv, now,
    )).resolves.toBe(false);
  });
});
