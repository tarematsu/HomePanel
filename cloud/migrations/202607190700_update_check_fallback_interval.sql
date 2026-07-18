-- Native release builds trigger update rollout immediately after publishing assets.
-- Keep a 30-minute scheduled fallback for missing or failed rollout triggers.
UPDATE jobs
   SET interval_seconds = 1800
 WHERE name = 'update_check'
   AND interval_seconds <> 1800;
