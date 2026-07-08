(() => {
  'use strict';

  const root = window.HomePanel;
  const { $, text } = root.utils;

  const RADAR_WIDTH = 800;
  const RADAR_HEIGHT = 520;
  const RADAR_DATA_URL = 'https://data.homepanel/radar.json';
  const RADAR_BASE_SATELLITE_URL = 'radar-satellite.png';
  const RADAR_BASE_MAP_URL = 'radar-map.png';

  let radarRefreshPromise = null;
  let radarSignatureValue = '';
  let radarFrame = null;
  let baseLayersPromise = null;

  function buildFallbackFrame(baseLayers) {
    const frame = document.createElement('canvas');
    frame.width = RADAR_WIDTH;
    frame.height = RADAR_HEIGHT;
    const context = frame.getContext('2d', { alpha: false });
    if (!context) return null;
    context.setTransform(1, 0, 0, 1, 0, 0);
    context.clearRect(0, 0, RADAR_WIDTH, RADAR_HEIGHT);
    context.globalCompositeOperation = 'source-over';
    context.globalAlpha = 1;
    context.imageSmoothingEnabled = true;
    context.imageSmoothingQuality = 'high';
    context.drawImage(baseLayers.satellite, 0, 0, RADAR_WIDTH, RADAR_HEIGHT);
    context.drawImage(baseLayers.map, 0, 0, RADAR_WIDTH, RADAR_HEIGHT);
    return frame;
  }

  function loadRadarImage(url, cacheKey = '') {
    return new Promise(resolve => {
      const image = new Image();
      image.onload = () => resolve(image);
      image.onerror = () => resolve(null);
      const separator = String(url).includes('?') ? '&' : '?';
      image.src = cacheKey ? `${url}${separator}v=${encodeURIComponent(cacheKey)}` : url;
    });
  }

  function radarDateFromMillis(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed > 0 ? new Date(parsed) : null;
  }

  function loadBaseLayers() {
    if (!baseLayersPromise) {
      baseLayersPromise = Promise.all([
        loadRadarImage(RADAR_BASE_SATELLITE_URL, 'bundled-satellite'),
        loadRadarImage(RADAR_BASE_MAP_URL, 'bundled-map'),
      ]).then(([satellite, map]) => {
        if (!satellite || !map) throw new Error('bundled radar base layers unavailable');
        return { satellite, map };
      }).catch(error => {
        baseLayersPromise = null;
        throw error;
      });
    }
    return baseLayersPromise;
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
    const baseLayers = await loadBaseLayers();
    let radar = null;
    try {
      const response = await fetch(`${RADAR_DATA_URL}?_=${Date.now()}`, { cache: 'no-store' });
      if (response.ok) radar = await response.json();
    } catch (_) {
      radar = null;
    }
    const baseTiles = Array.isArray(radar?.baseTiles) ? radar.baseTiles : [];
    const frames = Array.isArray(radar?.frames) ? radar.frames : [];
    const frameData = frames[0] || null;
    const overlayTiles = Array.isArray(frameData?.tiles) ? frameData.tiles : [];

    const signature = JSON.stringify([
      frameData?.baseTime || '',
      frameData?.validTime || '',
      overlayTiles.length,
      baseTiles.length || 'bundled',
      ...overlayTiles.map(tile => String(tile?.url || '')),
    ]);
    if (signature === radarSignatureValue && radarFrame) {
      presentRadar();
      return;
    }

    const width = Math.max(1, Number(radar?.width) || 400);
    const height = Math.max(1, Number(radar?.height) || 260);
    const scaleX = RADAR_WIDTH / width;
    const scaleY = RADAR_HEIGHT / height;

    const overlayCacheKey = `${frameData?.baseTime || ''}-${frameData?.validTime || ''}`;
    const overlayImages = await Promise.all(
      overlayTiles.map(tile => loadRadarImage(tile.url, `${overlayCacheKey}-${tile?.x || 0}-${tile?.y || 0}`)),
    );

    const frame = buildFallbackFrame(baseLayers);
    if (!frame) return;
    const context = frame.getContext('2d', { alpha: false });
    if (!context) return;

    context.imageSmoothingEnabled = false;
    let loadedOverlayCount = 0;
    overlayTiles.forEach((tile, index) => {
      const image = overlayImages[index];
      if (!image) return;
      loadedOverlayCount += 1;
      context.drawImage(
        image,
        Math.round(Number(tile.destX || 0) * scaleX),
        Math.round(Number(tile.destY || 0) * scaleY),
        Math.ceil(256 * scaleX),
        Math.ceil(256 * scaleY),
      );
    });
    context.imageSmoothingEnabled = true;
    context.imageSmoothingQuality = 'high';
    if (overlayTiles.length) context.drawImage(baseLayers.map, 0, 0, RADAR_WIDTH, RADAR_HEIGHT);

    const when = radarDateFromMillis(frameData?.validAt);
    if (when) {
      text('#radar-time', when.toLocaleTimeString('ja-JP', {
        hour: '2-digit', minute: '2-digit', hour12: false, timeZone: 'Asia/Tokyo',
      }));
    } else {
      text('#radar-time', overlayTiles.length ? '--:--' : '待機中');
    }

    if (overlayTiles.length && loadedOverlayCount === 0) {
      radarSignatureValue = '';
      text('#radar-time', '--:--');
      return;
    }

    radarSignatureValue = signature;
    radarFrame = frame;
    presentRadar();
  }

  function refreshRadar() {
    if (radarRefreshPromise) return radarRefreshPromise;
    radarRefreshPromise = buildRadarFrame()
      .catch(() => {
        if (!radarFrame) {
          loadBaseLayers()
            .then(baseLayers => {
              radarFrame = buildFallbackFrame(baseLayers);
              if (radarFrame) {
                text('#radar-time', '待機中');
                presentRadar();
              }
            })
            .catch(() => {
              text('#radar-time', '--:--');
            });
        }
      })
      .finally(() => {
        radarRefreshPromise = null;
      });
    return radarRefreshPromise;
  }

  root.panels = root.panels || {};
  root.panels.radar = { refreshRadar, presentRadar };
})();
