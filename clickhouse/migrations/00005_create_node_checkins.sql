-- +goose Up
CREATE TABLE middlines.node_checkins
(
    seen_at DateTime64(6, 'America/New_York') DEFAULT now64(6),
    node LowCardinality(String),
    firmware_version LowCardinality(String),
    client_ip String
)
ENGINE = MergeTree
PRIMARY KEY node
ORDER BY (node, seen_at);

-- +goose Down
DROP TABLE middlines.node_checkins;
