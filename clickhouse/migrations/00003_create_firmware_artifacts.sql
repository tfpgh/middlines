-- +goose Up
CREATE TABLE middlines.firmware_artifacts
(
    sha256 FixedString(64),
    version String,
    filename String,
    original_filename String,
    size_bytes UInt64,
    uploaded_at DateTime64(3, 'America/New_York') DEFAULT now64(3)
)
ENGINE = MergeTree
PRIMARY KEY sha256
ORDER BY sha256;

-- +goose Down
DROP TABLE middlines.firmware_artifacts;
