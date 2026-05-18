CC=clang
CFLAGS=-Wall -Wextra -Werror -pedantic -O3
DEBUG=$(CFLAGS) -g
CLIBS=-lm -lcurl -lpthread
SRCS=fast.c
TARGET=fast

OS=$(shell uname -s)

all:
	$(CC) $(CFLAGS) $(SRCS) $(CLIBS) -o $(TARGET)

test:
	$(CC) $(DEBUG) $(SRCS) $(CLIBS) -o $(TARGET)
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all ./$(TARGET)

install: all
ifeq ($(OS),Linux)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
else ifeq ($(OS),Darwin)
	install -m755 $(TARGET) /usr/local/bin/$(TARGET)
else
	@echo "Unsupported OS: $(OS)"
endif

uninstall:
	rm -f /usr/local/bin/$(TARGET)

clean:
	$(RM) $(TARGET)
