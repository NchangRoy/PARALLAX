CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g -O2 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread

SRCS = fault_tolerance.c fault_master.c fault_worker_secondary.c
TEST = test_fault_tolerance.c
BIN  = ft_test

all: $(BIN)

$(BIN): $(SRCS) $(TEST) fault_tolerance.h
	$(CC) $(CFLAGS) $(SRCS) $(TEST) -o $(BIN) $(LDFLAGS)

clean:
	rm -f $(BIN) *.o

.PHONY: all clean
