# Telemetry protocol

Nodes send BLE observation batches to:

```text
POST /api/node/{node}/observations
Content-Type: application/octet-stream
Authorization: Bearer <node token>
```

Requests use HTTPS. The firmware sends at most 64 observations in each request.
The API submits each request to ClickHouse as an asynchronous insert without
waiting for the ClickHouse buffer to flush. A failed request is counted and its
batch is dropped; the firmware does not retain a retry queue.

The payload is binary and uses network byte order:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | Magic bytes `ML` |
| 2 | 1 | Protocol version (`1`) |
| 3 | 2 | Number of records (`uint16`) |
| 5 | 15 × count | Observation records |

Each observation record contains:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | Unix timestamp in milliseconds (`uint64`) |
| 8 | 6 | Full BLE MAC address |
| 14 | 1 | RSSI in dBm (`int8`) |

A 64-record payload is 965 bytes. The URL carries the node identity, so it is
not repeated in each record. Unknown versions and payloads whose size does not
exactly match their record count are discarded.
