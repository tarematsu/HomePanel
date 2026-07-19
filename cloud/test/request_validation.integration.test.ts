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

  it("rejects non-object refresh bodies", async () => {
    for (const body of [null, [], 123, "weather"]) {
      const response = await SELF.fetch("https://homepanel.test/v1/refresh", {
        method: "POST",
        headers: actionAuth,
        body: JSON.stringify(body),
      });
      expect(response.status).toBe(400);
      await expect(response.json()).resolves.toEqual({ error: "body must be an object" });
    }
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

  it("rejects non-string refresh sources instead of treating them as refresh all", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/refresh", {
      method: "POST",
      headers: actionAuth,
      body: JSON.stringify({ sources: [123] }),
    });
    expect(response.status).toBe(400);
    await expect(response.json()).resolves.toEqual({ error: "sources must contain only strings" });
  });

  it("rejects an empty refresh source list", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/refresh", {
      method: "POST",
      headers: actionAuth,
      body: JSON.stringify({ sources: [] }),
    });
    expect(response.status).toBe(400);
    await expect(response.json()).resolves.toEqual({ error: "sources must include a supported source" });
  });

  it("rejects unsupported refresh sources", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/refresh", {
      method: "POST",
      headers: actionAuth,
      body: JSON.stringify({ sources: ["unknown"] }),
    });
    expect(response.status).toBe(400);
    await expect(response.json()).resolves.toEqual({ error: "sources must include a supported source" });
  });
});
