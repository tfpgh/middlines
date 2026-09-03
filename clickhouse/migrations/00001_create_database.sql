-- +goose Up
CREATE DATABASE IF NOT EXISTS middlines;

-- +goose Down
DROP DATABASE IF EXISTS middlines;
