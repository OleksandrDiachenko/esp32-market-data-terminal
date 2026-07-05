# Architecture

## Current State
The project currently contains a minimal ESP-IDF application.

`components/market_data_client` is a Binance public REST API client
(exchangeInfo symbol validation, klines candles). It has no `_init()`/
`_start()` and runs no background task - every call is blocking, invoked
from the caller's own task, gated on `time_sync_is_synced()`. JSON
responses are parsed with a custom incremental (streaming) parser rather
than a DOM library, to avoid buffering a full response body or building a
parse tree on a device sharing RAM with the display - see
`docs/decisions/0002-market-data-client.md`.

## Planned Modules
- OTA service
- MQTT service
- Wi-Fi service
- configuration storage
- market_data_client (Phase 7, REST fetch/parse/validate — done, see
  `docs/roadmap.md`)
- market data polling/orchestration service (Phase 8 — periodic polling and
  runtime state built on top of market_data_client)
- market data WebSocket streaming (Phase 9 - live kline updates via
  wss://stream.binance.{com,us}, layered on Phase 8's runtime state — done,
  see `docs/roadmap.md` and `docs/decisions/0004-websocket-streaming.md`)
- display UI LVGL layer
