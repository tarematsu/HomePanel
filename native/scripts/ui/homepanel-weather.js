(() => {
  'use strict';

  const root = window.HomePanel;
  const { $, text, finite, number, escapeHtml, statusLabel, hourSuffix, degree } = root.utils;
  const WX_ICON_BASE = 'vendor/wx-icons';
  const LOCAL_ICON_CODES = new Set([100, 101, 200, 206, 212, 300, 313, 400]);

  function wxYahooCode(iconCode) {
    const c = parseInt(iconCode, 10);
    if (!c) return null;
    if (c >= 500) return 400;
    if (c >= 400) return 206;
    if (c >= 300) return 300;
    if (c >= 230) return 212;
    if (c >= 210) return 206;
    if (c >= 200) return 200;
    if (c >= 130) return 101;
    if (c >= 100) return 100;
    return null;
  }

  function localWxCode(iconCode) {
    const yahoo = wxYahooCode(iconCode);
    if (!yahoo) return null;
    if (LOCAL_ICON_CODES.has(yahoo)) return yahoo;
    if (yahoo >= 400) return 400;
    if (yahoo >= 300) return 313;
    return 200;
  }

  function wxIconImg(iconCode) {
    const localCode = localWxCode(iconCode);
    if (!localCode) return '<span class="wx-emoji-fallback">--</span>';
    return `<img class="wx-icon-img" src="${WX_ICON_BASE}/${localCode}.svg" alt="">`;
  }

  function weatherHours(weather) {
    const hourly = weather.hourly || {};
    return Object.keys(hourly)
      .map(Number)
      .filter(Number.isFinite)
      .sort((a, b) => a - b)
      .slice(0, 5)
      .map(hour => ({ hour, ...(hourly[String(hour)] || {}) }));
  }

  function renderWeather(weather = {}) {
    const container = $('#weather-hours');
    text('#weather-status', statusLabel(weather));
    if (!container) return;

    const hours = weatherHours(weather);
    if (!hours.length) {
      container.innerHTML = `<div class="empty">${escapeHtml(root.utils.T('empty.weather'))}</div>`;
      return;
    }

    const pops = hours.map(h => finite(h.pop) ? Number(h.pop) : 0);
    const maxPop = Math.max(...pops);
    const popClass = maxPop >= 70 ? 'pop-high' : maxPop >= 40 ? 'pop-mid' : 'pop-low';
    const forecastDate = weather.forecastDate || '';

    const leftHtml = `
      <div class="wx-left">
        ${forecastDate ? `<div class="wx-forecast-date">${escapeHtml(forecastDate)}</div>` : ''}
        <div class="wx-pop-label">降水確率</div>
        <div class="wx-pop-value ${popClass}">${Math.round(maxPop)}<span class="wx-pop-unit">%</span></div>
      </div>`;

    const rightHtml = `
      <div class="wx-right">${hours.map(h => {
        const rainMm = finite(h.rainMm) ? Number(h.rainMm) : null;
        const rainLabel = rainMm !== null ? `${Math.round(rainMm)}mm` : '--';
        return `<div class="wx-hour-card">
          <time>${h.hour}${hourSuffix}</time>
          ${wxIconImg(h.icon)}
          <b>${finite(h.temp) ? number(h.temp, 0) : '--'}${degree}</b>
          <span class="wx-rain">${escapeHtml(rainLabel)}</span>
        </div>`;
      }).join('')}</div>`;

    container.innerHTML = leftHtml + rightHtml;
  }

  root.panels = root.panels || {};
  root.panels.weather = { renderWeather };
})();
