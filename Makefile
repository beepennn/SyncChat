CC = gcc

CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L

INCLUDES = -Iinclude

BIN_DIR = bin

LOGGER_DAEMON = $(BIN_DIR)/logger_daemon
LOGGER_TEST = $(BIN_DIR)/logger_test

LOGGER_SRC = src/logger/main.c
LOGGER_TEST_SRC = src/logger/test_client.c

.PHONY: all clean logger logger-test

all: logger logger-test

logger: $(LOGGER_DAEMON)

$(LOGGER_DAEMON): $(LOGGER_SRC) include/logger/logger_protocol.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(LOGGER_SRC) -o $(LOGGER_DAEMON)

logger-test: $(LOGGER_TEST)

$(LOGGER_TEST): $(LOGGER_TEST_SRC) include/logger/logger_protocol.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(LOGGER_TEST_SRC) -o $(LOGGER_TEST)

clean:
	rm -f $(LOGGER_DAEMON) $(LOGGER_TEST)
