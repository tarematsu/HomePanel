CREATE TABLE IF NOT EXISTS video_runtime_state (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  active INTEGER NOT NULL DEFAULT 0 CHECK (active IN (0, 1)),
  activated_at TEXT
);

INSERT OR IGNORE INTO video_runtime_state (id, active, activated_at)
VALUES (1, 0, NULL);
