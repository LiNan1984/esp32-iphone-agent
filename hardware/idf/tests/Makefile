# Run idf.py reconfigure first to download esp_tinyusb and its TinyUSB dependency.
TINYUSB = ../managed_components/espressif__tinyusb/src
CORE = ../../hid_bridge/src
CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
.PHONY: test clean
test: test_hid_commands
	./test_hid_commands

test_hid_commands: test_hid_commands.c ../main/p4_hid_commands.c $(CORE)/hid/pace.c
	$(CC) $(CFLAGS) -Istubs -I../main -I$(CORE) -I$(TINYUSB) $^ -lm -o $@

clean:
	rm -f test_hid_commands
