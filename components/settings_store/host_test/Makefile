CC ?= gcc
SRC_DIR = ../src
INCLUDE_DIR = ../include
BUILD_DIR = build
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I$(SRC_DIR) -I$(INCLUDE_DIR)

CODEC_SOURCES = $(SRC_DIR)/settings_codec.c

TEST_NAMES = \
	test_settings_codec_display \
	test_settings_codec_symbols \
	test_settings_codec_locale

TESTS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

.PHONY: test clean

test: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_settings_codec_display: test_settings_codec_display.c $(CODEC_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_settings_codec_display.c $(CODEC_SOURCES)

$(BUILD_DIR)/test_settings_codec_symbols: test_settings_codec_symbols.c $(CODEC_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_settings_codec_symbols.c $(CODEC_SOURCES)

$(BUILD_DIR)/test_settings_codec_locale: test_settings_codec_locale.c $(CODEC_SOURCES) test_common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_settings_codec_locale.c $(CODEC_SOURCES)

clean:
	rm -rf $(BUILD_DIR)
