export const LIVENESS_JOB_NAME = 'video_liveness';
export const LIVENESS_INTERVAL_SECONDS = 60 * 60;
export const LIVENESS_SCHEDULE = `homepanel-alarm:${LIVENESS_INTERVAL_SECONDS}s`;

// Compatibility value for imported VideoScraper code and historical tests.
// No Cloudflare Cron Trigger is configured after the HomePanel alarm migration.
export const LIVENESS_CRON = '0 * * * *';
