# Run idf.py reconfigure first to download esp_tinyusb and its TinyUSB dependency.
TINYUSB = ../managed_components/espressif__tinyusb/src
CORE = ../../hid_bridge/src
CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
.PHONY: test clean
test: test_hid_commands test_cmd_split
	./test_hid_commands
	./test_cmd_split

test_hid_commands: test_hid_commands.c ../main/p4_hid_commands.c $(CORE)/hid/pace.c
	$(CC) $(CFLAGS) -Istubs -I../main -I$(CORE) -I$(TINYUSB) $^ -lm -o $@

# The id/command split at the head of the BLE->HID bridge: pure logic, so the real
# function compiles on the host with no stubs at all.
test_cmd_split: test_cmd_split.c ../main/p4_cmd_split.c ../main/p4_cmd_bridge.h
	$(CC) $(CFLAGS) -Istubs -I../main -o $@ test_cmd_split.c ../main/p4_cmd_split.c

clean:
	rm -f test_hid_commands test_cmd_split
