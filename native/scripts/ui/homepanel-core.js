(() => {
  'use strict';

  const root = window.HomePanel = window.HomePanel || {};
  const textUrl = 'texts.json';

  const fallbackWeekdays = ['日', '月', '火', '水', '木', '金', '土'];
  const fallbackWeekdaysShort = ['日', '月', '火', '水', '木', '金', '土'];
  const fallbackCloudUpdated = '未更新';

  root.state = root.state || {
    texts: {},
    dashboard: {},
    runtime: {},
    cloudStationhead: {},
    newsItems: [],
    newsIndex: 0,
    revisions: Object.create(null),
    textsPromise: null,
  };

  const nodeCache = new Map();
  const $ = selector => {
    const cached = nodeCache.get(selector);
    if (cached?.isConnected) return cached;
    const node = document.querySelector(selector);
    if (node) nodeCache.set(selector, node);
    return node;
  };
  const setText = (selector, value) => {
    const node = $(selector);
    const next = value ?? '';
    if (node && node.textContent !== next) node.textContent = next;
  };
  const setButtonText = (selector, value) => {
    const node = $(`${selector} span:last-child`);
    const next = value ?? '';
    if (node && node.textContent !== next) node.textContent = next;
  };
  const setHidden = (node, hidden) => {
    if (node && node.hidden !== hidden) node.hidden = hidden;
  };
  const finite = value => Number.isFinite(Number(value));
  const number = (value, digits = 1) => finite(value) ? Number(value).toFixed(digits) : '--';
  const mb = value => finite(value) ? `${Math.round(Number(value) / 1048576)} MB` : '--';
  const escapeHtml = value => String(value ?? '').replace(/[&<>'"]/g, char => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;',
  }[char]));
  const translate = path => path.split('.').reduce((acc, key) => acc && acc[key], root.state.texts) ?? '';
  const format = (template, values = {}) => String(template || '').replace(/\{(\w+)\}/g, (_, key) => values[key] ?? '');
  const icon = id => `<svg class="mini-icon"><use href="#${id}"></use></svg>`;

  function postAction(action, value) {
    window.chrome?.webview?.postMessage({ type: 'action', action, value });
  }

  function statusLabel(section) {
    const state = section?.__status;
    if (state === 'stale') return translate('status.stale');
    if (state === 'error') return translate('status.error');
    if (state === 'waiting') return translate('status.waiting');
    return '';
  }

  function setStaticLabels() {
    setText('#news-title', translate('titles.news'));
    setText('#weather-title', translate('titles.weather'));
    setText('#switchbot-title', translate('titles.switchbot'));
    setText('#energy-title', translate('titles.octopus'));
    setText('#radar-title', translate('titles.radar'));
    setText('#controls-title', translate('titles.controls'));
    setText('#maintenance-title', translate('titles.maintenance'));
    setText('#temperature-label', translate('air.temperature'));
    setText('#humidity-label', translate('air.humidity'));
    setButtonText('#maintenance-close', translate('controls.close'));
    setButtonText('#maintenance-reconnect', translate('controls.reconnect'));
    setButtonText('#maintenance-clear', translate('controls.clearCache'));
    setButtonText('#maintenance-log', translate('controls.showLog'));
    setButtonText('#app-update', translate('controls.appUpdate'));
    setButtonText('#restart', translate('controls.restart'));
    setButtonText('#maintenance-button', translate('controls.maintenance'));
    setButtonText('#exit', translate('controls.exit'));
  }

  async function loadTexts() {
    if (root.state.textsPromise) return root.state.textsPromise;
    root.state.textsPromise = fetch(textUrl, { cache: 'no-store' })
      .then(response => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.json();
      })
      .then(json => {
        root.state.texts = json || {};
        setStaticLabels();
        return root.state.texts;
      })
      .catch(() => {
        root.state.texts = {};
        setStaticLabels();
        return root.state.texts;
      });
    return root.state.textsPromise;
  }

  document.addEventListener('click', event => {
    const button = event.target.closest('[data-action]');
    if (button && !button.disabled) postAction(button.dataset.action);
  });

  root.utils = {
    $, text: setText, buttonText: setButtonText, setHidden,
    finite, number, mb, escapeHtml, T: translate, format, icon,
    postAction, statusLabel, setStaticLabels, loadTexts,
    fallbackWeekdays, fallbackWeekdaysShort, fallbackCloudUpdated,
    degree: '℃', middleDot: '・', hourSuffix: '時',
  };
})();
