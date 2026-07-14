import { SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";

const actionAuth: HeadersInit = {
  Authorization: "Bearer test-action",
  "content-type": "application/json",
};

describe("Worker request validation", () => {
  it("rejects malformed update file path encoding without returning 500", async () => {
    const response = await SELF.fetch(
      "https://homepanel.test/v1/update/file/%E0%A4%A?expires=1&signature=invalid",
    );
    expect(response.status).toBe(400);
    await expect(response.json()).resolves.toEqual({ error: "invalid update file path" });
  });

  it("rejects malformed refresh JSON instead of refreshing every source", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/refresh", {
      method: "POST",
      headers: actionAuth,
      body: "{",
    });
    expect(response.status).toBe(400);
    await expect(response.json()).resolves.toEqual({ error: "invalid json" });
  });

  it("rejects a non-array refresh source list", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/refresh", {
      method: "POST",
      headers: actionAuth,
      body: JSON.stringify({ sources: "weather" }),
    });
    expect(response.status).toBe(400);
    await expect(response.json()).resolves.toEqual({ error: "sources must be an array" });
  });
});
