interface VideoScheduledController {
  cron: string;
}

interface VideoExecutionContext {
  waitUntil(promise: Promise<unknown>): void;
}

interface VideoWorkerHandler {
  scheduled(
    controller: VideoScheduledController,
    env: unknown,
    ctx: VideoExecutionContext,
  ): Promise<void>;
}

declare const videoWorker: VideoWorkerHandler;
export default videoWorker;
