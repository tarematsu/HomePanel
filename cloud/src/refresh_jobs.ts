export const REFRESHABLE_JOB_NAMES = [
  "weather",
  "news",
  "switchbot",
  "octopus",
  "stationhead",
  "stationhead_health",
  "radar",
  "update_check",
] as const;

const REFRESHABLE_JOB_SET = new Set<string>(REFRESHABLE_JOB_NAMES);

export function normalizeRefreshJobNames(names?: readonly string[]): string[] | null {
  if (names === undefined) return [...REFRESHABLE_JOB_NAMES];
  const selected = [...new Set(names.filter(name => REFRESHABLE_JOB_SET.has(name)))];
  return selected.length ? selected : null;
}
