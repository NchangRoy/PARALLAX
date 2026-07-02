CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
TARGET  = parallax_agent

# Sources communes
SRCS = main.c \
       Agent_Init/init.c \
       Agent_Init/monitoring/Monitoring.c \
	   Agent_Init/heart_beat/heartbeat.c \
	   Agent_Init/network/*.c \
	   Controller/state_receiver/*.c \
	   Execution_Worker/worker_exec.c \
	   $(filter-out Execution_Master/utils/linked_list.c,\
             $(wildcard Execution_Master/utils/*.c))

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
			-I./Agent_Init/heart_beat \
		   	-I./Controller/state_receiver \
			-I./Execution_Worker \
			-I./parallax \
			-I./Execution_Master/utils


# Parser (Parser/build/mytool) — separate CMake/C++ project used by the
# master agent to decompose @parallax-annotated submissions. Not part of
# $(SRCS) above (it's C++/Clang tooling, not plain C), but wired in here so
# `make` keeps it in sync instead of it silently rotting against whatever
# LLVM version happened to be installed when someone last built it by hand.
PARSER_DIR = Parser
PARSER_BUILD_DIR = $(PARSER_DIR)/build
PARSER_BIN = $(PARSER_BUILD_DIR)/mytool

all: $(TARGET) $(PARSER_BIN)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS)  $(SRCS) $(INCLUDES) -o $(TARGET) $(LDFLAGS)

$(PARSER_BIN): $(PARSER_DIR)/parser.cpp $(PARSER_DIR)/CMakeLists.txt
	mkdir -p $(PARSER_BUILD_DIR)
	cd $(PARSER_BUILD_DIR) && cmake .. -DCMAKE_BUILD_TYPE=Release
	$(MAKE) -C $(PARSER_BUILD_DIR)

clean:
	rm -f $(TARGET)

# Separate from `clean`: Parser/build is a git submodule reference with no
# .gitmodules entry, so its contents are NOT recoverable via git if deleted
# (learned this the hard way — see CLUSTER_INTEGRATION.md). Only run this
# if you specifically want to force a from-scratch Parser rebuild.
parser-clean:
	rm -rf $(PARSER_BUILD_DIR)

.PHONY: all clean parser-clean