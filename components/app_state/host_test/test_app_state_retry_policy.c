#include "test_common.h"
#include "app_state_retry_policy.h"

static void test_recoverable_classification(void)
{
    CHECK(app_state_retry_is_recoverable(MARKET_DATA_ERR_NETWORK));
    CHECK(app_state_retry_is_recoverable(MARKET_DATA_ERR_TIMEOUT));
    CHECK(app_state_retry_is_recoverable(MARKET_DATA_ERR_HTTP_STATUS));
    CHECK(app_state_retry_is_recoverable(MARKET_DATA_ERR_RATE_LIMITED));
    CHECK(app_state_retry_is_recoverable(MARKET_DATA_ERR_NOT_SYNCED));
    CHECK(app_state_retry_is_recoverable(MARKET_DATA_ERR_PARSE));
    CHECK(app_state_retry_is_recoverable(MARKET_DATA_ERR_NO_MEM));
}

static void test_unrecoverable_classification(void)
{
    CHECK(!app_state_retry_is_recoverable(MARKET_DATA_OK));
    CHECK(!app_state_retry_is_recoverable(MARKET_DATA_ERR_INVALID_ARG));
    CHECK(!app_state_retry_is_recoverable(MARKET_DATA_ERR_SYMBOL_NOT_FOUND));
}

static void test_backoff_formula(void)
{
    CHECK(app_state_retry_backoff_delay_ms(1000, 8000, 0) == 1000);
    CHECK(app_state_retry_backoff_delay_ms(1000, 8000, 1) == 2000);
    CHECK(app_state_retry_backoff_delay_ms(1000, 8000, 2) == 4000);
    CHECK(app_state_retry_backoff_delay_ms(1000, 8000, 3) == 8000);
    CHECK(app_state_retry_backoff_delay_ms(1000, 8000, 10) == 8000);
}

static void test_backoff_resets_conceptually(void)
{
    // The policy itself is stateless (attempt is caller-tracked) - a
    // "reset after success" claim just means the caller passes attempt=0
    // again, which must yield base_ms.
    CHECK(app_state_retry_backoff_delay_ms(500, 4000, 0) == 500);
}

static void test_gap_detection_threshold(void)
{
    CHECK(!app_state_retry_needs_resync(0, 300000));
    CHECK(!app_state_retry_needs_resync(299999, 300000));
    CHECK(app_state_retry_needs_resync(300000, 300000));
    CHECK(app_state_retry_needs_resync(600000, 300000));
}

int main(void)
{
    test_recoverable_classification();
    test_unrecoverable_classification();
    test_backoff_formula();
    test_backoff_resets_conceptually();
    test_gap_detection_threshold();
    return test_summary("app_state_retry_policy");
}
