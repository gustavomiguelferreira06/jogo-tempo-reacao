CREATE TABLE IF NOT EXISTS partidas (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  jogador   TEXT NOT NULL,
  tempo_ms  INTEGER,
  modo      TEXT,
  queimou   INTEGER DEFAULT 0,
  data_hora TEXT DEFAULT (datetime('now','localtime'))
);

-- Ranking dos 10 melhores tempos
-- SELECT jogador, MIN(tempo_ms) AS Recorde_ms, ROUND(AVG(tempo_ms)) AS Media_ms, COUNT(*) AS Rodadas
-- FROM partidas WHERE queimou = 0 GROUP BY jogador ORDER BY Recorde_ms LIMIT 10;
