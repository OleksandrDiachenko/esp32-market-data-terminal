CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I.. -I.
BUILD_DIR = build
TARGET = $(BUILD_DIR)/test_display_ui_logic

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): test_display_ui_logic.c test_common.h ../display_ui_logic.c ../display_ui_logic.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_display_ui_logic.c ../display_ui_logic.c

clean:
	rm -rf $(BUILD_DIR)
