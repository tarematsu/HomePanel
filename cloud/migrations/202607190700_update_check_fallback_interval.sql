UPDATE jobs
   SET interval_seconds = 1800
 WHERE name = 'update_check'
   AND interval_seconds <> 1800;
