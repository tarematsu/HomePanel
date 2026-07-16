from pathlib import Path

root = Path(__file__).resolve().parents[2]
panels = root / "native/src/renderer_panels"

part1 = (panels / "part1.inc").read_text(encoding="utf-8")
part2 = (panels / "part2.inc").read_text(encoding="utf-8")
part3 = (panels / "part3.inc").read_text(encoding="utf-8")
part4 = (panels / "part4.inc").read_text(encoding="utf-8")

windows_marker = "const std::array<Renderer::NativePanelSlot, 3>& Renderer::NativePanelSlots()"
clock_marker = "void Renderer::DrawClockSection"
music_marker = "void Renderer::DrawMusicSection"

windows_at = part2.index(windows_marker)
clock_at = part3.index(clock_marker)
music_at = part3.index(music_marker)

fragments = {
    "primitives.inc": part1 + part2[:windows_at],
    "windows.inc": part2[windows_at:] + part3[:clock_at],
    "environment_sections.inc": part3[clock_at:music_at],
    "media_section.inc": part3[music_at:],
    "data_sections.inc": part4,
}

original = part1 + part2 + part3 + part4
reorganized = "".join(fragments.values())
if reorganized != original:
    raise RuntimeError("renderer panel fragment reorganization changed source order")

for name, content in fragments.items():
    (panels / name).write_text(content, encoding="utf-8", newline="\n")

aggregator = root / "native/src/renderer_panels.cpp"
aggregator.write_text(
    "// Kept as one translation unit so cached GDI primitives remain shared.\n"
    '#include "renderer_panels/primitives.inc"\n'
    '#include "renderer_panels/windows.inc"\n'
    '#include "renderer_panels/environment_sections.inc"\n'
    '#include "renderer_panels/media_section.inc"\n'
    '#include "renderer_panels/data_sections.inc"\n',
    encoding="utf-8",
    newline="\n",
)

for name in ("part1.inc", "part2.inc", "part3.inc", "part4.inc"):
    (panels / name).unlink()
