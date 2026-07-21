import { runLivenessMonitor } from "../../video/src/liveness-monitor.js";
import type { Env } from "./sources";
import { videoRuntimeActive } from "./video_runtime_activation.js";

export async function runVideoLiveness(env: Env): Promise<void> {
  if (!await videoRuntimeActive(env)) {
    console.log("video-liveness-skipped-inactive-runtime");
    return;
  }
  await runLivenessMonitor(env);
}
