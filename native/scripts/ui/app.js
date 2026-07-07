(() => {
  'use strict';

  const root = window.HomePanel;
  const { loadTexts, setStaticLabels } = root.utils;
  const panels = root.panels || {};
  const runtime = root.runtime || {};

  function renderRuntime(state) {
    runtime.renderRuntime?.(state || {});
  }

  async function initialize() {
    let initialized = false;
    let queuedState = null;

    if (window.chrome?.webview) {
      window.chrome.webview.addEventListener('message', event => {
        if (!initialized) {
          queuedState = event.data;
          return;
        }
        renderRuntime(event.data);
      });
    }

    await loadTexts();
    setStaticLabels();
    panels.clock?.updateClock();
    panels.radar?.refreshRadar();
    panels.energy?.drawEnergyChart();

    initialized = true;
    if (queuedState) renderRuntime(queuedState);
    window.chrome?.webview?.postMessage({ type: 'ready' });

    setInterval(() => {
      if (!document.hidden) panels.clock?.updateClock();
    }, 1000);
    setInterval(() => panels.radar?.refreshRadar(), panels.radar?.refreshMs || 5 * 60 * 1000);

    window.addEventListener('online', () => panels.radar?.refreshRadar());
    window.addEventListener('resize', () => {
      panels.energy?.drawEnergyChart();
      panels.radar?.presentRadar();
    });
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) {
        panels.clock?.updateClock();
        panels.energy?.drawEnergyChart();
        panels.radar?.refreshRadar();
      }
    });
  }

  initialize();
})();
