CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
TARGET  = receptionist

# Sources for the receptionist standalone program
SRCS = reception.c \
       ../Agent_Init/network/network_agent.c \
       ../Agent_Init/network/ms_queue.c \
       ../Agent_Init/network/socket.c \
       ../Agent_Init/network/linked_list.c \
       ../Execution_Master/utils/net_utils.c


# Includes
INCLUDES = -I. \
           -I../Agent_Init \
           -I../Agent_Init/network \
           -I../parallax \
           -I../Execution_Master/utils

# OS detection
UNAME := $(shell uname)

ifeq ($(UNAME), Linux)
    CFLAGS += -DOS_LINUX
endif

ifeq ($(UNAME), Darwin)
    CFLAGS += -DOS_MACOS
endif

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(INCLUDES) -o $(TARGET)

clean:
	rm -f $(TARGET) .receptionist_uuid

.PHONY: all clean
