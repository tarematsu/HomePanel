import { describe, expect, it } from "vitest";
import { updateFileResponse, updateManifestResponse } from "../src/update_proxy";
import type { Env } from "../src/sources";

type StoredObject = {
  body: Uint8Array;
  contentType?: string;
  size?: number;
};

function text(value: string): Uint8Array {
  return new TextEncoder().encode(value);
}

function bucket(objects: Record<string, StoredObject>): R2Bucket {
  return {
    async get(key: string) {
      const object = objects[key];
      if (!object) return null;
      return {
        body: new ReadableStream({
          start(controller) {
            controller.enqueue(object.body);
            controller.close();
          },
        }),
        size: object.size,
        httpEtag: `"${key}"`,
        httpMetadata: object.contentType ? { contentType: object.contentType } : undefined,
        async text() {
          return new TextDecoder().decode(object.body);
        },
      } as unknown as R2ObjectBody;
    },
  } as unknown as R2Bucket;
}

function testEnv(objects: Record<string, StoredObject>, overrides: Partial<Env> = {}): Env {
  return {
    DB: {} as D1Database,
    UPDATE_BUCKET: bucket(objects),
    UPDATE_SIGNING_SECRET: "test-signing-secret",
    UPDATE_BUCKET_PREFIX: "updates",
    ...overrides,
  };
}

describe("update proxy", () => {
  it("returns signed manifest URLs backed by R2", async () => {
    const manifest = JSON.stringify({
      version: "2607071200",
      signed: false,
      files: [
        { name: "HomePanel.exe", sha256: "a", size: 10, requireAuthenticode: false },
        { name: "HomePanelUpdater.exe", sha256: "b", size: 11, requireAuthenticode: false },
        { name: "WebView2Loader.dll", sha256: "c", size: 12, requireAuthenticode: true },
      ],
    });
    const env = testEnv({
      "updates/latest/update-manifest.json": { body: text(manifest), contentType: "application/json" },
    });
    const response = await updateManifestResponse(new Request("https://homepanel.test/v1/update/manifest"), env);
    expect(response.status).toBe(200);
    const payload = await response.json() as {
      version: string;
      files: Array<{ name: string; url: string }>;
    };
    expect(payload.version).toBe("2607071200");
    expect(payload.files).toHaveLength(3);
    expect(payload.files[0]?.url).toContain("/v1/update/file/HomePanel.exe?version=2607071200");
  });

  it("streams an R2 object when the signature is valid", async () => {
    const manifest = JSON.stringify({
      version: "2607071200",
      signed: false,
      files: [
        { name: "HomePanel.exe", sha256: "a", size: 10, requireAuthenticode: false },
        { name: "HomePanelUpdater.exe", sha256: "b", size: 11, requireAuthenticode: false },
        { name: "WebView2Loader.dll", sha256: "c", size: 12, requireAuthenticode: true },
      ],
    });
    const env = testEnv({
      "updates/latest/update-manifest.json": { body: text(manifest), contentType: "application/json" },
      "updates/releases/2607071200/HomePanel.exe": { body: text("binary-homepanel"), contentType: "application/octet-stream" },
    });

    const manifestResponse = await updateManifestResponse(new Request("https://homepanel.test/v1/update/manifest"), env);
    const payload = await manifestResponse.json() as { files: Array<{ name: string; url: string }> };
    const fileUrl = payload.files.find(file => file.name === "HomePanel.exe")?.url;
    expect(fileUrl).toBeTruthy();

    const response = await updateFileResponse(new Request(fileUrl!), env, "HomePanel.exe");
    expect(response.status).toBe(200);
    expect(response.headers.get("content-type")).toBe("application/octet-stream");
    expect(response.headers.get("content-length")).toBeNull();
    const body = new TextDecoder().decode(await response.arrayBuffer());
    expect(body).toBe("binary-homepanel");
  });
});
