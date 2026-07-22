#include "web_renderer.h"

namespace hp {

bool Renderer::LoadDashboard(const fs::path& jsonPath, bool* changed) {
  if (changed) *changed = false;
  try {
    std::error_code error;
    const fs::path normalizedPath = jsonPath.lexically_normal();
    const std::uintmax_t sourceSize = fs::file_size(normalizedPath, error);
    if (error || sourceSize == 0 ||
        sourceSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
      return false;
    }
    const fs::file_time_type modifiedAt = fs::last_write_time(normalizedPath, error);
    if (error) return false;

    if (dashboardSourceStamp_.valid &&
        dashboardSourceStamp_.path == normalizedPath &&
        dashboardSourceStamp_.size == sourceSize &&
        dashboardSourceStamp_.modifiedAt == modifiedAt) {
      return true;
    }

    std::ifstream input(normalizedPath, std::ios::binary);
    if (!input) return false;
    std::string text(static_cast<size_t>(sourceSize), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (input.gcount() != static_cast<std::streamsize>(text.size()) ||
        input.peek() != std::char_traits<char>::eof()) {
      return false;
    }

    const DashboardSourceStamp nextStamp{
        normalizedPath, sourceSize, modifiedAt, true};
    // dashboardUtf8_ is retained as a compatibility field name, but now stores
    // only a small content signature instead of a second persistent copy of
    // dashboard.json. Size is included so differently sized input cannot be
    // treated as equal solely because of the 64-bit hash.
    const std::string contentSignature =
        std::to_string(sourceSize) + ":" + std::to_string(Fnv1a64(text));
    if (dashboardSourceStamp_.valid && dashboardUtf8_ == contentSignature) {
      dashboardSourceStamp_ = nextStamp;
      return true;
    }

    DashboardSnapshot snapshot;
    if (!ParseDashboardSnapshot(text, snapshot)) return false;
    const bool firstSnapshot = !nativeDashboard_.loaded;
    const bool weatherChanged = firstSnapshot ||
        snapshot.revisions.weather != nativeDashboard_.revisions.weather;
    const bool energyChanged = firstSnapshot ||
        snapshot.revisions.energy != nativeDashboard_.revisions.energy;
    const bool contentChanged = weatherChanged || energyChanged;

    // News is no longer rendered natively, so keep the legacy rotation count at
    // zero. This prevents the App's 30-second News timer/state publication path
    // from arming without changing its compatibility surface in this PR.
    newsCount_ = 0;
    nativeDashboard_ = std::move(snapshot);
    dashboardUtf8_ = contentSignature;
    dashboardSourceStamp_ = nextStamp;
    if (weatherChanged) ++dashboardRevisions_.weather;
    if (energyChanged) ++dashboardRevisions_.energy;
    if (changed) *changed = contentChanged;
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace hp
