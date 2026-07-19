import { describe, expect, it } from "vitest";
import type { Env } from "../src/sources";
import {
  telemetrySampleReadbackStatement,
  telemetrySampleStatement,
} from "../src/telemetry_bucket";

function captureSql() {
  const prepared: string[] = [];
  const DB = {
    prepare(sql: string) {
      prepared.push(sql);
      return { bind: () => ({}) };
    },
  } as unknown as D1Database;
  return { env: { DB } as Env, prepared };
}

describe("telemetry retry write policy", () => {
  it("does not update an identical sequence solely to acknowledge a retry", () => {
    const { env, prepared } = captureSql();
    telemetrySampleStatement(env, "ci-device", {
      sequence: 7,
      observedAt: 1_800_000_000_000,
      co2: 700,
    });

    expect(prepared[0]).toContain("WHERE excluded.observed_at>environment_samples.observed_at");
    expect(prepared[0]).not.toContain("excluded.co2 IS environment_samples.co2");
  });

  it("reads conflicting sequences back in one bounded query", () => {
    const { env, prepared } = captureSql();
    telemetrySampleReadbackStatement(env, "ci-device", [7, 8, 9]);

    expect(prepared[0]).toContain("sequence IN (?2,?3,?4)");
    expect(prepared[0]?.match(/FROM environment_samples/g)).toHaveLength(1);
  });
});
