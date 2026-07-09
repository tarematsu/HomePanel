# Runtime native assets

Dashboard rendering is native. The runtime asset installer only copies files
that native code still loads from disk.

Current assets:

- `native/scripts/ui/radar-satellite.png`: 1920x1280 GSI
  `seamlessphoto` tile composite centered on Kawagoe Suna-shinden 1-chome.
- `native/scripts/ui/radar-map.png`: 1920x1280 transparent municipality
  boundary overlay generated from GSI `experimental_bvmap` vector tiles.

When adding a runtime asset:

1. Add the source file under an appropriate native asset folder.
2. Add an `RCDATA` entry in `native/resources/HomePanel.rc.in`.
3. Add the resource id and output filename to `kRuntimeAssets` in
   `native/src/embedded_ui.cpp`.
4. Load it from native code; do not add Dashboard browser HTML, CSS, or JS.
