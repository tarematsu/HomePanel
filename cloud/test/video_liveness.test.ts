import { describe, expect, it } from "vitest";
import { runVideoLiveness } from "../src/video_liveness";

describe("video liveness scheduler bridge", () => {
  it("does not touch video tables while the unified runtime is inactive", async () => {
    const statements: string[] = [];
    const DB = {
      prepare(sql: string) {
        statements.push(sql);
        return {
          async first() {
            return { active: 0 };
          },
        };
      },
    };

    await runVideoLiveness({ DB } as never);

    expect(statements).toEqual([
      "SELECT active FROM video_runtime_state WHERE id = 1",
    ]);
  });
});
