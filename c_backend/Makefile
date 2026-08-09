CC = gcc
CFLAGS = -Wall -Wextra -O2 -I.
SRCS = lexer.c parser.c semantic.c tac.c optimizer.c bytecode.c vm.c json.c main.c
SERVER_SRCS = lexer.c parser.c semantic.c tac.c optimizer.c bytecode.c vm.c json.c server.c
OBJS = $(SRCS:.c=.o)
TARGET = nova_compiler
SERVER_TARGET = nova_server

all: $(TARGET) $(SERVER_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(SERVER_TARGET): $(SERVER_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lws2_32

test_native: $(OBJS) native_test.o
	$(CC) $(CFLAGS) -o nova_native_test $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGET) $(SERVER_TARGET) $(TARGET).exe $(SERVER_TARGET).exe nova_native_test

.PHONY: all clean test_native
