#include "web_renderer.h"

namespace hp {

bool Renderer::LoadDashboard(const fs::path& jsonPath, bool* changed) {
  if (changed) *changed = false;
  try {
    std::error_code error;
    const fs::path normalizedPath = jsonPath.lexically_normal();
    const std::uintmax_t sourceSize = fs::file_size(normalizedPath, error);
    if (error) return false;
    const fs::file_time_type modifiedAt = fs::last_write_time(normalizedPath, error);
    if (error) return false;

    const DashboardSourceStamp nextStamp{
        normalizedPath, sourceSize, modifiedAt, true};
    if (dashboardSourceStamp_.valid &&
        dashboardSourceStamp_.path == nextStamp.path &&
        dashboardSourceStamp_.size == nextStamp.size &&
        dashboardSourceStamp_.modifiedAt == nextStamp.modifiedAt) {
      return true;
    }

    std::ifstream input(normalizedPath, std::ios::binary);
    if (!input) return false;
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return false;
    if (text == dashboardUtf8_) {
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
    const bool newsChanged = firstSnapshot ||
        snapshot.revisions.news != nativeDashboard_.revisions.news;
    const bool contentChanged = weatherChanged || energyChanged || newsChanged;

    newsCount_ = snapshot.newsItemCount;
    nativeDashboard_ = std::move(snapshot);
    dashboardUtf8_ = std::move(text);
    dashboardSourceStamp_ = nextStamp;
    if (weatherChanged) ++dashboardRevisions_.weather;
    if (energyChanged) ++dashboardRevisions_.energy;
    if (newsChanged) ++dashboardRevisions_.news;
    if (changed) *changed = contentChanged;
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace hp
