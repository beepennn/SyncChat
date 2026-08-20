CC = gcc

CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L
INCLUDES = -Iinclude

BIN_DIR = bin

LOGGER_DAEMON = $(BIN_DIR)/logger_daemon
LOGGER_TEST = $(BIN_DIR)/logger_test
SERVER = $(BIN_DIR)/syncchat_server
CLIENT = $(BIN_DIR)/syncchat_client

LOGGER_SRC = src/logger/main.c
LOGGER_TEST_SRC = src/logger/test_client.c
LOGGER_CLIENT_SRC = src/logger/logger_client.c

NETWORK_IO_SRC = src/common/network_io.c
PROTOCOL_SRC = src/common/protocol.c
FILE_TRANSFER_SRC = src/common/file_transfer.c

CLIENT_MANAGER_SRC = src/server/client_manager.c
FILE_MANAGER_SRC = src/server/file_manager.c

UPLOAD_CLIENT_SRC = src/client/upload_client.c
DOWNLOAD_CLIENT_SRC = src/client/download_client.c


SERVER_SRC = \
	src/server/main.c \
	src/server/client_handler.c \
	$(CLIENT_MANAGER_SRC) \
	$(FILE_MANAGER_SRC) \
	$(LOGGER_CLIENT_SRC) \
	$(NETWORK_IO_SRC) \
	$(PROTOCOL_SRC) \
	$(FILE_TRANSFER_SRC)

CLIENT_SRC = \
	src/client/main.c \
	$(UPLOAD_CLIENT_SRC) \
	$(DOWNLOAD_CLIENT_SRC) \
	$(NETWORK_IO_SRC) \
	$(PROTOCOL_SRC) \
	$(FILE_TRANSFER_SRC)

.PHONY: all clean logger logger-test server client

all: logger logger-test server client

logger: $(LOGGER_DAEMON)

$(LOGGER_DAEMON): $(LOGGER_SRC) include/logger/logger_protocol.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(LOGGER_SRC) -o $(LOGGER_DAEMON)

logger-test: $(LOGGER_TEST)

$(LOGGER_TEST): $(LOGGER_TEST_SRC) include/logger/logger_protocol.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(LOGGER_TEST_SRC) -o $(LOGGER_TEST)

server: $(SERVER)

$(SERVER): $(SERVER_SRC) \
	include/common/network_io.h \
	include/common/protocol.h \
	include/common/file_transfer.h \
	include/logger/logger_protocol.h \
	include/logger/logger_client.h \
	include/server/server_config.h \
	include/server/client_handler.h \
	include/server/client_manager.h \
	include/server/file_manager.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(SERVER_SRC) -pthread -o $(SERVER)

client: $(CLIENT)

$(CLIENT): $(CLIENT_SRC) \
	include/common/network_io.h \
	include/common/protocol.h \
	include/common/file_transfer.h \
	include/client/upload_client.h \
	include/client/download_client.h \
	include/server/server_config.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(CLIENT_SRC) -pthread -o $(CLIENT)

clean:
	rm -f $(LOGGER_DAEMON)
	rm -f $(LOGGER_TEST)
	rm -f $(SERVER)
	rm -f $(CLIENT)
