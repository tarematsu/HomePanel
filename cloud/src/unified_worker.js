import homePanelWorker from './worker_entry.ts';
import videoWorker from '../../video/src/entry.js';

export { SchedulerCoordinator } from './worker_entry.ts';

const HOMEPANEL_EXACT_PATHS = new Set(['/admin', '/v1']);

export function requestFamily(pathname) {
  return HOMEPANEL_EXACT_PATHS.has(pathname) || pathname.startsWith('/v1/')
    ? 'homepanel'
    : 'video';
}

export default {
  async fetch(request, env, ctx) {
    const pathname = new URL(request.url).pathname;
    if (requestFamily(pathname) === 'homepanel') {
      return homePanelWorker.fetch(request, env, ctx);
    }
    return videoWorker.fetch(request, env, ctx);
  },

  async queue(batch, env, ctx) {
    if (typeof videoWorker.queue !== 'function') return undefined;
    return videoWorker.queue(batch, env, ctx);
  },

  async scheduled(controller, env, ctx) {
    if (typeof videoWorker.scheduled !== 'function') return undefined;
    return videoWorker.scheduled(controller, env, ctx);
  }
};
