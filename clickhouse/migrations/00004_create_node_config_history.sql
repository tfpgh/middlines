-- +goose Up
CREATE TABLE middlines.node_config_history
(
    changed_at DateTime64(6, 'America/New_York') DEFAULT now64(6),
    node LowCardinality(String),
    token String,
    poll_interval_s UInt16,
    target_firmware_sha256 Nullable(FixedString(64)),
    restart_nonce Nullable(UUID)
)
ENGINE = MergeTree
PRIMARY KEY node
ORDER BY (node, changed_at);

-- +goose Down
DROP TABLE middlines.node_config_history;
