#pragma once

// Orchestrates app_state's REST bootstrap/resync: watches wifi_manager's
// event queue and time_sync_is_synced(), and calls
// market_data_client_fetch_klines_24h_5m_batch() for whichever watchlist
// symbols are due (never-synced, or a recoverable failure whose backoff
// delay has elapsed) - see app_state_retry_policy.h for what "due" means.
// A Wi-Fi reconnect after a gap of at least one candle interval (5 min)
// forces every symbol to be re-fetched, since the existing history may
// have a hole in it that a plain retry wouldn't fill.
//
// REST here is a bootstrap/resync mechanism, not continuous polling - once
// a symbol is SYNCED it is left alone until Phase 9's WebSocket stream (or
// the next resync trigger) updates it further.

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Creates the sync task. Call once, after app_state_init(),
    // wifi_manager_init()+_start(), and time_sync_start() - the task itself
    // tolerates all three being not-yet-connected/synced and just waits.
    esp_err_t app_state_sync_task_start(void);

    // Forces every non-ERROR watchlist symbol to be re-fetched on the sync
    // task's next 2s cycle - the same path a post-reconnect gap resync uses
    // (see RESYNC_GAP_MS in app_state_sync_task.c). Called by display_ui.c
    // after an API region switch, since the underlying host changed and any
    // existing klines history came from the old one - see
    // docs/decisions/0009-regional-server-auto-selection.md.
    void app_state_sync_task_force_resync(void);

#ifdef __cplusplus
}
#endif
