CC ?= gcc
SRC_DIR = ../src
BUILD_DIR = build
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I$(SRC_DIR)

POLICY_SOURCES = $(SRC_DIR)/wifi_policy.c $(SRC_DIR)/wifi_profile_codec.c
CODEC_SOURCES = $(SRC_DIR)/wifi_profile_codec.c

TEST_NAMES = \
	test_wifi_policy_boot \
	test_wifi_policy_retry \
	test_wifi_policy_authblock \
	test_wifi_policy_cycle \
	test_wifi_policy_fallback \
	test_wifi_policy_scan \
	test_wifi_policy_switch_network \
	test_wifi_policy_pause_resume \
	test_wifi_profile_codec

TESTS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

.PHONY: test clean

test: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_wifi_policy_boot: test_wifi_policy_boot.c $(POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_policy_boot.c $(POLICY_SOURCES)

$(BUILD_DIR)/test_wifi_policy_retry: test_wifi_policy_retry.c $(POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_policy_retry.c $(POLICY_SOURCES)

$(BUILD_DIR)/test_wifi_policy_authblock: test_wifi_policy_authblock.c $(POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_policy_authblock.c $(POLICY_SOURCES)

$(BUILD_DIR)/test_wifi_policy_cycle: test_wifi_policy_cycle.c $(POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_policy_cycle.c $(POLICY_SOURCES)

$(BUILD_DIR)/test_wifi_policy_fallback: test_wifi_policy_fallback.c $(POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_policy_fallback.c $(POLICY_SOURCES)

$(BUILD_DIR)/test_wifi_policy_scan: test_wifi_policy_scan.c $(POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_policy_scan.c $(POLICY_SOURCES)

$(BUILD_DIR)/test_wifi_policy_switch_network: test_wifi_policy_switch_network.c $(POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_policy_switch_network.c $(POLICY_SOURCES)

$(BUILD_DIR)/test_wifi_policy_pause_resume: test_wifi_policy_pause_resume.c $(POLICY_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_policy_pause_resume.c $(POLICY_SOURCES)

$(BUILD_DIR)/test_wifi_profile_codec: test_wifi_profile_codec.c $(CODEC_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_wifi_profile_codec.c $(CODEC_SOURCES)

clean:
	rm -rf $(BUILD_DIR)
