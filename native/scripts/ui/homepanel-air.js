(() => {
  'use strict';

  const root = window.HomePanel;
  const { $, text, number, mb, escapeHtml, T, degree, middleDot, fallbackCloudUpdated } = root.utils;

  function renderAir(value = {}) {
    const co2 = value.connected ? Number(value.co2) : 0;
    const quality = !value.connected
      ? T('air.quality.disconnected')
      : co2 > 1000
        ? T('air.quality.high')
        : co2 > 800
          ? T('air.quality.medium')
          : T('air.quality.good');

    const airPanel = $('.panel.air');
    if (airPanel) {
      airPanel.classList.remove('co2-good', 'co2-warn', 'co2-bad');
      if (value.connected) {
        if (co2 > 1000) airPanel.classList.add('co2-bad');
        else if (co2 > 800) airPanel.classList.add('co2-warn');
        else airPanel.classList.add('co2-good');
      }
    }

    text('#co2-value', value.connected ? value.co2 : '---');
    text('#air-label', quality);
    text('#temperature', value.connected ? `${number(value.temperature, 1)}${degree}` : `--.-${degree}`);
    text('#humidity', value.connected ? `${number(value.humidity, 0)}%` : '--%');
    text('#presence', value.presence === 'home' ? T('air.presence.home') : value.presence === 'away' ? T('air.presence.away') : T('air.presence.unknown'));
    text('#room-state', `${value.light ? T('air.room.lightOn') : T('air.room.lightOff')}${middleDot}${value.motion ? T('air.room.motionOn') : T('air.room.motionOff')}`);
  }

  root.panels = root.panels || {};
  root.panels.air = { renderAir };
})();
