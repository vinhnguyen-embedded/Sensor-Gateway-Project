CC = gcc

CFLAGS = -Wall -Wextra -pthread -Iinclude
DEBUG_FLAGS = -g -DDEBUG

HTTP = 8080
PORT = 1234

SRC = src/main.c \
      src/connection_manager.c \
      src/data_manager.c \
      src/logger.c \
      src/protocol_adapter.c \
      src/sbuffer.c \
      src/storage_manager.c

TARGET = sensor_gateway

LDLIBS = -lsqlite3


# ============================================================
# Build bình thường
# ============================================================

all: $(TARGET)


$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDLIBS)


# ============================================================
# Build debug
# ============================================================

debug: $(SRC)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(SRC) -o $(TARGET) $(LDLIBS)


# ============================================================
# Chạy gateway
# ============================================================

run: $(TARGET)
	./$(TARGET) $(PORT) $(HTTP)


# ============================================================
# Build sensor node
# ============================================================

sensor_node:
	$(CC) $(CFLAGS) test/sensor_node.c -o sensor_node


# ============================================================
# Valgrind - kiểm tra memory leak
# ============================================================

valgrind: debug
	valgrind \
		--leak-check=full \
		--show-leak-kinds=all \
		--track-origins=yes \
		./$(TARGET) $(PORT) $(HTTP)


# ============================================================
# Helgrind - kiểm tra data race/thread
# ============================================================

helgrind: debug
	valgrind \
		--tool=helgrind \
		./$(TARGET) $(PORT) $(HTTP)


# ============================================================
# Kiểm tra process gateway còn sót không
# ============================================================

check-process:
	ps aux | grep $(TARGET)


# ============================================================
# Kiểm tra FIFO còn tồn tại không
# ============================================================

check-fifo:
	ls -l logFifo


# ============================================================
# Cleanup
# ============================================================

clean:
	rm -f $(TARGET)
	rm -f sensor_node
	rm -f logFifo


.PHONY: all \
	debug \
	run \
	sensor_node \
	valgrind \
	helgrind \
	check-process \
	check-fifo \
	clean