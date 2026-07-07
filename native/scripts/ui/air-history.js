(() => {
  'use strict';

  const historyWindowMs = 24 * 60 * 60 * 1000;
  const sampleBucketMs = 5 * 60 * 1000;
  const maxSamples = Math.ceil(historyWindowMs / sampleBucketMs) + 1;
  const metrics = [
    { key: 'co2', rangeId: 'co2-trend-range', decimals: 0, suffix: ' ppm', color: '#39d353', width: 2.5 },
    { key: 'temperature', rangeId: 'temperature-trend-range', decimals: 1, suffix: '\u2103', color: 'rgba(255,184,48,.38)', width: 1 },
    { key: 'humidity', rangeId: 'humidity-trend-range', decimals: 0, suffix: '%', color: 'rgba(74,179,244,.38)', width: 1 },
  ];

  let history = [];
  let historyVersion = 0;
  let lastDrawKey = '';
  const finite = value => Number.isFinite(Number(value));

  function validSample(value) {
    return value && finite(value.t) && finite(value.co2) && finite(value.temperature) && finite(value.humidity) &&
      Number(value.co2) >= 250 && Number(value.co2) <= 10000 &&
      Number(value.temperature) >= -40 && Number(value.temperature) <= 85 &&
      Number(value.humidity) >= 0 && Number(value.humidity) <= 100;
  }

  function normalizeSample(value) {
    return {
      t: Math.floor(Number(value.t) / sampleBucketMs) * sampleBucketMs,
      co2: Number(value.co2),
      temperature: Number(value.temperature),
      humidity: Number(value.humidity),
    };
  }

  function trimHistory(now = Date.now()) {
    const cutoff = now - historyWindowMs;
    const byBucket = new Map();
    history.forEach(value => {
      if (!validSample(value)) return;
      const sample = normalizeSample(value);
      if (sample.t >= cutoff && sample.t <= now + sampleBucketMs) byBucket.set(sample.t, sample);
    });
    history = [...byBucket.values()].sort((a, b) => a.t - b.t).slice(-maxSamples);
  }

  function surface() {
    const canvas = document.getElementById('air-history-chart');
    if (!(canvas instanceof HTMLCanvasElement)) return null;
    const rect = canvas.getBoundingClientRect();
    const ratio = Math.min(1.25, Math.max(1, window.devicePixelRatio || 1));
    const width = Math.max(1, Math.round(rect.width * ratio));
    const height = Math.max(1, Math.round(rect.height * ratio));
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const context = canvas.getContext('2d', { alpha: true });
    if (!context) return null;
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    return { context, width: rect.width, height: rect.height };
  }

  function metricBounds(key, values) {
    if (!values.length) return { min: 0, max: 1 };
    const observedMin = Math.min(...values);
    const observedMax = Math.max(...values);
    const padding = key === 'co2' ? 80 : key === 'temperature' ? 0.5 : 2;
    const minimumSpan = key === 'co2' ? 300 : key === 'temperature' ? 2 : 10;
    const center = (observedMin + observedMax) / 2;
    const span = Math.max(minimumSpan, observedMax - observedMin + padding * 2);
    return { min: center - span / 2, max: center + span / 2 };
  }

  function updateRanges(valuesByMetric) {
    metrics.forEach(metric => {
      const node = document.getElementById(metric.rangeId);
      const values = valuesByMetric[metric.key];
      if (!node) return;
      const next = values.length
        ? `${Math.min(...values).toFixed(metric.decimals)}\u2013${Math.max(...values).toFixed(metric.decimals)}${metric.suffix}`
        : '--';
      if (node.textContent !== next) node.textContent = next;
    });
  }

  function drawLine(context, samples, key, bounds, cutoff, plot, color, width) {
    if (!samples.length) return;
    context.beginPath();
    samples.forEach((sample, index) => {
      const x = plot.left + Math.max(0, Math.min(1, (sample.t - cutoff) / historyWindowMs)) * plot.width;
      const yRatio = (Number(sample[key]) - bounds.min) / Math.max(.0001, bounds.max - bounds.min);
      const y = plot.bottom - Math.max(0, Math.min(1, yRatio)) * plot.height;
      if (index === 0) context.moveTo(x, y);
      else context.lineTo(x, y);
    });
    context.strokeStyle = color;
    context.lineWidth = width;
    context.lineJoin = 'round';
    context.lineCap = 'round';
    context.stroke();
  }

  function formatDateTime(date) {
    return `${date.getMonth() + 1}/${date.getDate()} ${String(date.getHours()).padStart(2, '0')}:${String(date.getMinutes()).padStart(2, '0')}`;
  }

  function renderHistory(now = Date.now()) {
    trimHistory(now);
    const canvas = surface();
    if (!canvas) return;
    const drawKey = `${historyVersion}:${Math.round(canvas.width)}:${Math.round(canvas.height)}:${Math.floor(now / sampleBucketMs)}`;
    if (drawKey === lastDrawKey) return;
    lastDrawKey = drawKey;

    const { context, width, height } = canvas;
    context.clearRect(0, 0, width, height);
    const plot = { left: 31, right: Math.max(32, width - 31), top: 3, bottom: Math.max(4, height - 3) };
    plot.width = Math.max(1, plot.right - plot.left);
    plot.height = Math.max(1, plot.bottom - plot.top);

    context.strokeStyle = 'rgba(255,255,255,.06)';
    context.lineWidth = 1;
    for (let index = 0; index < 4; index += 1) {
      const y = plot.top + plot.height * index / 3 + .5;
      context.beginPath();
      context.moveTo(plot.left, y);
      context.lineTo(plot.right, y);
      context.stroke();
    }

    const valuesByMetric = Object.fromEntries(metrics.map(metric => [
      metric.key,
      history.map(sample => Number(sample[metric.key])),
    ]));
    const bounds = Object.fromEntries(metrics.map(metric => [
      metric.key,
      metricBounds(metric.key, valuesByMetric[metric.key]),
    ]));
    const cutoff = now - historyWindowMs;

    drawLine(context, history, 'temperature', bounds.temperature, cutoff, plot, metrics[1].color, metrics[1].width);
    drawLine(context, history, 'humidity', bounds.humidity, cutoff, plot, metrics[2].color, metrics[2].width);
    drawLine(context, history, 'co2', bounds.co2, cutoff, plot, metrics[0].color, metrics[0].width);

    context.font = '8px sans-serif';
    context.textBaseline = 'middle';
    for (let index = 0; index < 4; index += 1) {
      const ratio = index / 3;
      const y = plot.bottom - plot.height * ratio;
      const co2 = bounds.co2.min + (bounds.co2.max - bounds.co2.min) * ratio;
      const temperature = bounds.temperature.min + (bounds.temperature.max - bounds.temperature.min) * ratio;
      context.textAlign = 'right';
      context.fillStyle = 'rgba(57,211,83,.75)';
      context.fillText(`${Math.round(co2)}`, plot.left - 3, y);
      context.textAlign = 'left';
      context.fillStyle = 'rgba(255,184,48,.6)';
      context.fillText(`${temperature.toFixed(1)}\u00b0`, plot.right + 3, y);
    }

    updateRanges(valuesByMetric);
    const start = document.getElementById('air-history-start');
    const end = document.getElementById('air-history-end');
    const startText = formatDateTime(new Date(now - historyWindowMs));
    const endText = formatDateTime(new Date(now));
    if (start && start.textContent !== startText) start.textContent = startText;
    if (end && end.textContent !== endText) end.textContent = endText;
  }

  function applyHistory(samples) {
    history = (Array.isArray(samples) ? samples : []).filter(validSample).map(normalizeSample);
    trimHistory();
    historyVersion += 1;
    lastDrawKey = '';
    renderHistory();
  }

  window.addEventListener('homepanel:air-history', event => applyHistory(event.detail || []));
  let resizeQueued = false;
  window.addEventListener('resize', () => {
    if (resizeQueued) return;
    resizeQueued = true;
    requestAnimationFrame(() => {
      resizeQueued = false;
      lastDrawKey = '';
      renderHistory();
    });
  });
  renderHistory();
})();

(() => {
  'use strict';

  function addLegend() {
    const stage = document.querySelector('.radar-stage');
    if (!stage || stage.querySelector('.radar-legend')) return;
    const legend = document.createElement('div');
    legend.className = 'radar-legend';
    legend.setAttribute('aria-label', '降水量の凡例');
    const title = document.createElement('span');
    title.className = 'radar-legend-title';
    title.textContent = 'mm/h';
    legend.appendChild(title);
    [
      ['#b40068', '80'], ['#ff2800', '50'], ['#ff9900', '30'],
      ['#faf500', '20'], ['#00a0f0', '10'], ['#0041ff', '5'], ['#2424aa', '1'],
    ].forEach(([color, value]) => {
      const row = document.createElement('span');
      const swatch = document.createElement('i');
      swatch.style.setProperty('--rain', color);
      row.append(swatch, document.createTextNode(value));
      legend.appendChild(row);
    });
    stage.appendChild(legend);
  }

  addLegend();
})();
