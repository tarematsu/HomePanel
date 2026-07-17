// Kept as one translation unit so cached GDI primitives remain shared.
// Fragment boundaries follow complete responsibilities and never split functions.
#include "renderer_panels/primitives.inc"
#include "renderer_panels/layout_overrides.inc"

#define SplitSidebarSections SplitRearrangedSidebarSections
#define SplitMainSections SplitRearrangedMainSections
#define ControlsFromContent RearrangedControlsFromContent
#define DrawClockSection HP_DRAW_CLOCK_WITH_CONTROLS
#define DrawControlsSection DrawAirSection
#include "renderer_panels/windows.inc"
#undef DrawControlsSection
#undef DrawClockSection
#undef ControlsFromContent
#undef SplitMainSections
#undef SplitSidebarSections

#include "renderer_panels/environment_sections.inc"
#include "renderer_panels/media_section.inc"
#include "renderer_panels/data_sections.inc"