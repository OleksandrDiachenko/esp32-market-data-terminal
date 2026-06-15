#pragma once

#include "esp_err.h"

/**
 * Logs startup diagnostics and returns an error if required assumptions fail.
 */
esp_err_t startup_diagnostics(void);