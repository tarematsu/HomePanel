(() => {
  'use strict';

  const root = window.HomePanel;
  const { $, text } = root.utils;

  const RADAR_WIDTH = 800;
  const RADAR_HEIGHT = 520;
  const RADAR_REFRESH_MS = 5 * 60 * 1000;
  const RADAR_DATA_URL = 'https://data.homepanel/radar.json';

  let radarRefreshPromise = null;
  let radarSignatureValue = '';
  let radarFrame = null;

  function loadRadarImage(url) {
    return new Promise(resolve => {
      const image = new Image();
      image.onload = () => resolve(image);
      image.onerror = () => resolve(null);
      image.src = url;
    });
  }

  function radarDateFromMillis(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed > 0 ? new Date(parsed) : null;
  }

  function presentRadar() {
    if (!radarFrame) return;
    const canvas = $('#radar-canvas');
    const context = canvas?.getContext('2d', { alpha: false, desynchronized: true });
    if (!canvas || !context) return;
    if (canvas.width !== RADAR_WIDTH) canvas.width = RADAR_WIDTH;
    if (canvas.height !== RADAR_HEIGHT) canvas.height = RADAR_HEIGHT;
    context.setTransform(1, 0, 0, 1, 0, 0);
    context.globalCompositeOperation = 'copy';
    context.globalAlpha = 1;
    context.drawImage(radarFrame, 0, 0);
    context.globalCompositeOperation = 'source-over';
  }

  async function buildRadarFrame() {
    const response = await fetch(`${RADAR_DATA_URL}?_=${Date.now()}`, { cache: 'no-store' });
    if (!response.ok) throw new Error(`radar HTTP ${response.status}`);
    const radar = await response.json();
    const baseTiles = Array.isArray(radar?.baseTiles) ? radar.baseTiles : [];
    const frames = Array.isArray(radar?.frames) ? radar.frames : [];
    const frameData = frames[0] || null;
    const overlayTiles = Array.isArray(frameData?.tiles) ? frameData.tiles : [];
    if (!baseTiles.length) throw new Error('radar base tiles unavailable');

    const signature = JSON.stringify([
      frameData?.baseTime || '',
      frameData?.validTime || '',
      overlayTiles.length,
      baseTiles.length,
    ]);
    if (signature === radarSignatureValue && radarFrame) {
      presentRadar();
      return;
    }

    const width = Math.max(1, Number(radar?.width) || 400);
    const height = Math.max(1, Number(radar?.height) || 260);
    const scaleX = RADAR_WIDTH / width;
    const scaleY = RADAR_HEIGHT / height;

    const [baseImages, overlayImages] = await Promise.all([
      Promise.all(baseTiles.map(tile => loadRadarImage(tile.url))),
      Promise.all(overlayTiles.map(tile => loadRadarImage(tile.url))),
    ]);

    const frame = document.createElement('canvas');
    frame.width = RADAR_WIDTH;
    frame.height = RADAR_HEIGHT;
    const context = frame.getContext('2d', { alpha: false });
    if (!context) return;

    context.setTransform(1, 0, 0, 1, 0, 0);
    context.clearRect(0, 0, RADAR_WIDTH, RADAR_HEIGHT);
    context.globalCompositeOperation = 'source-over';
    context.globalAlpha = 1;
    context.imageSmoothingEnabled = true;
    context.imageSmoothingQuality = 'high';

    baseTiles.forEach((tile, index) => {
      const image = baseImages[index];
      if (!image) return;
      context.drawImage(
        image,
        Math.round(Number(tile.destX || 0) * scaleX),
        Math.round(Number(tile.destY || 0) * scaleY),
        Math.ceil(256 * scaleX),
        Math.ceil(256 * scaleY),
      );
    });

    context.imageSmoothingEnabled = false;
    overlayTiles.forEach((tile, index) => {
      const image = overlayImages[index];
      if (!image) return;
      context.drawImage(
        image,
        Math.round(Number(tile.destX || 0) * scaleX),
        Math.round(Number(tile.destY || 0) * scaleY),
        Math.ceil(256 * scaleX),
        Math.ceil(256 * scaleY),
      );
    });

    const when = radarDateFromMillis(frameData?.validAt);
    if (when) {
      text('#radar-time', when.toLocaleTimeString('ja-JP', {
        hour: '2-digit', minute: '2-digit', hour12: false, timeZone: 'Asia/Tokyo',
      }));
    } else {
      text('#radar-time', '--:--');
    }

    radarSignatureValue = signature;
    radarFrame = frame;
    presentRadar();
  }

  function refreshRadar() {
    if (radarRefreshPromise) return radarRefreshPromise;
    radarRefreshPromise = buildRadarFrame()
      .catch(() => {
        if (!radarFrame) text('#radar-time', '--:--');
      })
      .finally(() => {
        radarRefreshPromise = null;
      });
    return radarRefreshPromise;
  }

  root.panels = root.panels || {};
  root.panels.radar = { refreshRadar, presentRadar, refreshMs: RADAR_REFRESH_MS };
})();
