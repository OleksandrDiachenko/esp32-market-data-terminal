CC ?= gcc
SRC_DIR = ../src
BUILD_DIR = build
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I$(SRC_DIR) -I../../market_data_client/include

RETRY_POLICY_SOURCES = $(SRC_DIR)/app_state_retry_policy.c

TEST_NAMES = \
	test_app_state_retry_policy

TESTS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

.PHONY: test clean

test: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_app_state_retry_policy: test_app_state_retry_policy.c $(RETRY_POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_app_state_retry_policy.c $(RETRY_POLICY_SOURCES)

clean:
	rm -rf $(BUILD_DIR)
