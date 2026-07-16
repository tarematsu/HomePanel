from pathlib import Path

path = Path('native/src/sh_shared.h')
text = path.read_text(encoding='utf-8')
old = '''              const bool armedNow = context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_STYLESHEET &&
                  armed.load(std::memory_order_relaxed);
              if ((blockImages && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE) ||
                  (blockFonts && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT) ||
                  (context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_STYLESHEET && armedNow) ||
                  context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_TEXT_TRACK ||
'''
new = '''              // Stationhead loads route and player styles lazily. Blocking every
              // stylesheet after audio starts can leave only the already-rendered
              // header visible while the station body stays black.
              (void)armed;
              if ((blockImages && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE) ||
                  (blockFonts && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT) ||
                  context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_TEXT_TRACK ||
'''
count = text.count(old)
if count != 1:
    raise SystemExit(f'expected one stylesheet blocking block, found {count}')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
