CC ?= gcc
SRC_DIR = ../src
BUILD_DIR = build
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I$(SRC_DIR) -I../include

TEST_NAMES = \
	test_ota_client_json

TESTS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

.PHONY: test clean

test: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_ota_client_json: test_ota_client_json.c $(SRC_DIR)/ota_client_json.c test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_ota_client_json.c $(SRC_DIR)/ota_client_json.c

clean:
	rm -rf $(BUILD_DIR)
