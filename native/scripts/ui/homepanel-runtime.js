(() => {
  'use strict';

  const root = window.HomePanel;
  const { $, text, setHidden } = root.utils;
  const panels = root.panels || {};

  window.__homepanelRuntimeState = window.__homepanelRuntimeState || null;

  function renderCloudData() {
    const dashboard = root.state.dashboard || {};
    panels.weather?.renderWeather(dashboard.weather || {});
    panels.energy?.renderEnergy(dashboard.octopus || {});
    panels.switchbot?.renderSwitchBot(dashboard.switchbot || {});
    root.state.cloudStationhead = dashboard.stationhead || {};
  }

  function renderControls(state = {}) {
    const version = String(state.diagnostics?.appVersion || '').trim();
    text('#controls-status', version ? `v${version}` : '');
  }

  function airHistorySource() {
    const dashboard = root.state.dashboard || {};
    const runtime = root.state.runtime || {};
    const cloud = Array.isArray(dashboard.environment?.history) ? dashboard.environment.history : [];
    const local = Array.isArray(runtime.airHistory) ? runtime.airHistory : [];
    return [...cloud, ...local];
  }

  function revisionChanged(name, state) {
    const revision = state?.revisions?.[name];
    if (revision == null) return true;
    if (root.state.revisions[name] === revision) return false;
    root.state.revisions[name] = revision;
    return true;
  }

  function renderToast(runtime) {
    const toast = $('#toast');
    if (!toast) return;
    setHidden(toast, !runtime.toast);
    const nextToast = runtime.toast || '';
    if (toast.textContent !== nextToast) toast.textContent = nextToast;
  }

  function renderRuntime(state = {}) {
    const runtime = root.state.runtime = { ...root.state.runtime, ...(state || {}) };
    window.__homepanelRuntimeState = runtime;

    const dashboardChanged = revisionChanged('dashboard', runtime);
    const newsChanged = revisionChanged('news', runtime);
    if (dashboardChanged) {
      root.state.dashboard = runtime.dashboard || {};
      renderCloudData();
    }
    if (dashboardChanged || newsChanged) {
      panels.news?.renderNews(root.state.dashboard.news || {}, runtime.newsIndex || 0);
    }

    const diagnosticsChanged = revisionChanged('diagnostics', runtime);
    const sensorsChanged = revisionChanged('sensors', runtime);
    const historyChanged = revisionChanged('airHistory', runtime);

    if (diagnosticsChanged) {
      renderControls(runtime);
    }
    if (sensorsChanged) panels.air?.renderAir(runtime.sensors || {});
    if (dashboardChanged || historyChanged) {
      window.dispatchEvent(new CustomEvent('homepanel:air-history', { detail: airHistorySource() }));
    }
    if (diagnosticsChanged || sensorsChanged) {
      panels.air?.renderDiagnostics(runtime.diagnostics || {}, runtime.sensors || {});
    }

    setHidden($('#maintenance'), !runtime.maintenance);
    renderToast(runtime);
  }

  root.runtime = { renderRuntime };
})();
