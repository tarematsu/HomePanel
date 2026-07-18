import type { Env } from "./sources";

const RADAR_MANIFEST_KEY = "radar/manifest.json";
const RADAR_DISPATCH_MARKER_KEY = "radar/dispatch.json";
const RADAR_MANIFEST_MAX_AGE_MS = 4 * 60 * 1000;
const RADAR_DISPATCH_COOLDOWN_MS = 6 * 60 * 1000;
const GITHUB_REPOSITORY = "tarematsu/HP";
const GITHUB_WORKFLOW = "radar-frames.yml";
const GITHUB_REF = "main";

export type RadarDispatchResult =
  | { status: "fresh"; manifestAgeMs: number }
  | { status: "cooldown"; manifestAgeMs: number | null; cooldownRemainingMs: number }
  | { status: "dispatched"; manifestAgeMs: number | null; dispatchedAt: number };

interface RadarDispatchOptions {
  now?: () => number;
  fetcher?: typeof fetch;
}

function finiteTimestamp(value: unknown): number | null {
  if (typeof value === "number" && Number.isFinite(value) && value > 0) return value;
  if (typeof value === "string") {
    const parsed = Date.parse(value);
    if (Number.isFinite(parsed) && parsed > 0) return parsed;
  }
  return null;
}

async function readObjectJson(bucket: R2Bucket, key: string): Promise<Record<string, unknown> | null> {
  const object = await bucket.get(key);
  if (!object) return null;
  try {
    const parsed: unknown = JSON.parse(await object.text());
    return parsed && typeof parsed === "object" && !Array.isArray(parsed)
      ? parsed as Record<string, unknown>
      : null;
  } catch {
    return null;
  }
}

async function responseDetail(response: Response): Promise<string> {
  try {
    return (await response.text()).replace(/\s+/g, " ").trim().slice(0, 500);
  } catch {
    return "";
  }
}

export async function dispatchRadarBuildIfStale(
  env: Env,
  options: RadarDispatchOptions = {},
): Promise<RadarDispatchResult> {
  const bucket = env.UPDATE_BUCKET;
  if (!bucket) throw new Error("radar R2 bucket is not configured");
  const token = env.GITHUB_RADAR_DISPATCH_TOKEN?.trim();
  if (!token) throw new Error("GITHUB_RADAR_DISPATCH_TOKEN is not configured");

  const now = options.now?.() ?? Date.now();
  const manifest = await readObjectJson(bucket, RADAR_MANIFEST_KEY);
  const generatedAt = finiteTimestamp(manifest?.generatedAt);
  const manifestAgeMs = generatedAt === null ? null : Math.max(0, now - generatedAt);
  if (manifestAgeMs !== null && manifestAgeMs < RADAR_MANIFEST_MAX_AGE_MS) {
    return { status: "fresh", manifestAgeMs };
  }

  const marker = await readObjectJson(bucket, RADAR_DISPATCH_MARKER_KEY);
  const lastDispatchedAt = finiteTimestamp(marker?.dispatchedAt);
  if (lastDispatchedAt !== null) {
    const elapsed = Math.max(0, now - lastDispatchedAt);
    if (elapsed < RADAR_DISPATCH_COOLDOWN_MS) {
      return {
        status: "cooldown",
        manifestAgeMs,
        cooldownRemainingMs: RADAR_DISPATCH_COOLDOWN_MS - elapsed,
      };
    }
  }

  const fetcher = options.fetcher ?? fetch;
  const response = await fetcher(
    `https://api.github.com/repos/${GITHUB_REPOSITORY}/actions/workflows/${GITHUB_WORKFLOW}/dispatches`,
    {
      method: "POST",
      headers: {
        Accept: "application/vnd.github+json",
        Authorization: `Bearer ${token}`,
        "Content-Type": "application/json",
        "User-Agent": "HomePanel-Cloud/2.5",
        "X-GitHub-Api-Version": "2022-11-28",
      },
      body: JSON.stringify({ ref: GITHUB_REF }),
    },
  );
  if (response.status !== 204) {
    const detail = await responseDetail(response);
    throw new Error(`GitHub radar workflow dispatch failed: HTTP ${response.status}${detail ? ` ${detail}` : ""}`);
  }

  await bucket.put(RADAR_DISPATCH_MARKER_KEY, JSON.stringify({
    dispatchedAt: now,
    manifestGeneratedAt: generatedAt,
    manifestAgeMs,
    repository: GITHUB_REPOSITORY,
    workflow: GITHUB_WORKFLOW,
    ref: GITHUB_REF,
  }), {
    httpMetadata: {
      contentType: "application/json",
      cacheControl: "private, max-age=0, must-revalidate",
    },
  });
  console.info("Dispatched radar build to GitHub Actions", {
    manifestAgeMs,
    dispatchedAt: new Date(now).toISOString(),
  });
  return { status: "dispatched", manifestAgeMs, dispatchedAt: now };
}
