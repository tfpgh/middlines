-- +goose Up
CREATE TABLE IF NOT EXISTS middlines.observations_raw
(
    observed_at DateTime64(3, 'America/New_York')
        CODEC(DoubleDelta, LZ4),
    node LowCardinality(String),
    mac UInt64
        CODEC(T64, LZ4),
    rssi Int8,
    ingested_at DateTime64(3, 'America/New_York')
        DEFAULT now64(3)
        CODEC(DoubleDelta, LZ4)
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(observed_at)
PRIMARY KEY observed_at
ORDER BY (observed_at, node);

-- +goose Down
DROP TABLE IF EXISTS middlines.observations_raw;
