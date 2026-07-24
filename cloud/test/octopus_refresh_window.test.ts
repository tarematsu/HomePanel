import { describe, expect, it } from "vitest";
import {
  OCTOPUS_COLLECTION_DAYS,
  octopusCollectionStart,
  octopusCompleteStableThroughJst,
  octopusStableCutoffJst,
} from "../src/octopus_history";

describe("Octopus refresh boundaries", () => {
  it("bootstraps seven complete JST days ending before the unstable window", () => {
    const now = Date.parse("2026-07-10T18:17:42Z");
    expect(OCTOPUS_COLLECTION_DAYS).toBe(7);
    expect(new Date(octopusCollectionStart(now)).toISOString()).toBe("2026-07-01T15:00:00.000Z");
    expect(new Date(octopusStableCutoffJst(now)).toISOString()).toBe("2026-07-08T18:00:00.000Z");
    expect(new Date(octopusCompleteStableThroughJst(now)).toISOString()).toBe("2026-07-08T15:00:00.000Z");
  });

  it("rejects an invalid synchronization clock", () => {
    expect(() => octopusCollectionStart(Number.NaN)).toThrow("finite");
  });
});
