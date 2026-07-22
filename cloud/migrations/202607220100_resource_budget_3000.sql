-- Keep the dashboard responsive while reducing Alarm invocations and D1 job
-- completion writes. Manual refresh and webhook paths still wake jobs early.
UPDATE jobs
   SET interval_seconds = CASE name
     WHEN 'switchbot' THEN 900
     WHEN 'stationhead' THEN 900
     WHEN 'stationhead_health' THEN 1800
     WHEN 'news' THEN 1800
     WHEN 'weather' THEN 3600
     WHEN 'octopus' THEN 21600
     WHEN 'video_liveness' THEN 720
     WHEN 'update_check' THEN 21600
     WHEN 'cleanup' THEN 86400
     ELSE interval_seconds
   END,
       next_run_at = CASE
         WHEN next_run_at = 0 THEN 0
         ELSE MIN(
           next_run_at,
           unixepoch() + CASE name
             WHEN 'switchbot' THEN 900
             WHEN 'stationhead' THEN 900
             WHEN 'stationhead_health' THEN 1800
             WHEN 'news' THEN 1800
             WHEN 'weather' THEN 3600
             WHEN 'octopus' THEN 21600
             WHEN 'video_liveness' THEN 720
             WHEN 'update_check' THEN 21600
             WHEN 'cleanup' THEN 86400
             ELSE interval_seconds
           END
         )
       END
 WHERE name IN (
   'switchbot','stationhead','stationhead_health','news','weather',
   'octopus','video_liveness','update_check','cleanup'
 );
