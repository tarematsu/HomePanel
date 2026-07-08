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
    let secondPulseTimer = 0;

    if (window.chrome?.webview) {
      window.chrome.webview.addEventListener('message', event => {
        if (!initialized) {
          queuedState = event.data;
          return;
        }
        if (event.data?.type === 'radar-updated') panels.radar?.refreshRadar();
        else renderRuntime(event.data);
      });
    }

    await loadTexts();
    setStaticLabels();
    panels.radar?.refreshRadar();
    panels.energy?.drawEnergyChart();

    initialized = true;
    if (queuedState?.type === 'radar-updated') panels.radar?.refreshRadar();
    else if (queuedState) renderRuntime(queuedState);
    window.chrome?.webview?.postMessage({ type: 'ready' });

    const dispatchSecondPulse = () => {
      if (document.hidden) return;
      window.dispatchEvent(new CustomEvent('homepanel-second', {
        detail: { now: Date.now() },
      }));
    };

    const scheduleSecondPulse = () => {
      if (secondPulseTimer) clearTimeout(secondPulseTimer);
      const delay = 1000 - (Date.now() % 1000) || 1000;
      secondPulseTimer = setTimeout(() => {
        secondPulseTimer = 0;
        dispatchSecondPulse();
        scheduleSecondPulse();
      }, delay);
    };

    dispatchSecondPulse();
    scheduleSecondPulse();
    window.addEventListener('online', () => panels.radar?.refreshRadar());
    window.addEventListener('resize', () => {
      panels.energy?.drawEnergyChart();
      panels.radar?.presentRadar();
    });
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) {
        panels.energy?.drawEnergyChart();
        panels.radar?.presentRadar();
      }
    });
  }

  initialize();
})();
