-- +goose Up
CREATE TABLE middlines.device_counts_1m
(
    node LowCardinality(String),
    bucket DateTime('UTC'),
    devices AggregateFunction(uniqExact, UInt64),
    last_observed_at SimpleAggregateFunction(max, DateTime64(3, 'America/New_York'))
)
ENGINE = AggregatingMergeTree
PARTITION BY toYYYYMM(bucket)
ORDER BY (node, bucket);

CREATE MATERIALIZED VIEW middlines.device_counts_1m_mv
TO middlines.device_counts_1m
AS
SELECT
    node,
    toDateTime(toStartOfMinute(observed_at), 'UTC') AS bucket,
    uniqExactState(mac) AS devices,
    max(observed_at) AS last_observed_at
FROM middlines.observations_raw
WHERE rssi >= -120
GROUP BY node, bucket;

-- +goose Down
DROP VIEW middlines.device_counts_1m_mv;
DROP TABLE middlines.device_counts_1m;
