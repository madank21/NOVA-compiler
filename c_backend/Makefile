CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I.

# Windows (MinGW) needs ws2_32 for sockets; Linux/macOS do not.
ifeq ($(OS),Windows_NT)
  SERVER_LIBS := -lws2_32
else
  SERVER_LIBS :=
  UNAME_S := $(shell uname -s)
  ifeq ($(UNAME_S),Darwin)
    SERVER_LIBS :=
  else
    SERVER_LIBS += -lpthread
  endif
endif

COMMON_SRCS := lexer.c parser.c semantic.c tac.c optimizer.c bytecode.c vm.c json.c compile.c diag.c fmt.c
COMMON_OBJS := $(COMMON_SRCS:.c=.o)

all: nova_compiler nova_server

nova_compiler: main.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

nova_server: server.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(SERVER_LIBS) -lm

# Native test harness (its own main; COMMON_OBJS excludes main.o)
check: nova_native_test
	./nova_native_test

nova_native_test: native_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

test_native: check

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

asan:
	$(MAKE) CFLAGS="-std=c99 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" all nova_native_test

clean:
	rm -f *.o nova_compiler nova_server nova_native_test *.exe

.PHONY: all check test_native asan clean