CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
TARGET  = parallax_agent

# Sources communes
SRCS = main.c \
       Agent_Init/init.c \
       Agent_Init/monitoring/Monitoring.c \
	   Agent_Init/network/network_agent.c \
	   Controller/state_receiver/state_receiver.c \
	   Controller/state_receiver/persistence.c \
	   Controller/state_receiver/node_table.c \

# Détection OS
UNAME := $(shell uname)

ifeq ($(UNAME), Linux)
    CFLAGS += -DOS_LINUX
endif

ifeq ($(UNAME), Darwin)
    CFLAGS += -DOS_MACOS
endif

# Windows (MinGW)
ifeq ($(OS), Windows_NT)
    CFLAGS  += -DOS_WINDOWS
    LDFLAGS += -lpsapi -liphlpapi -lpdh
endif

# Includes
INCLUDES = -I./Agent_Init \
			-I./Agent_Init/network \
			-I./Agent_Init/monitoring \
		   	-I./Controller/state_receiver \
			-I./parallax

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean