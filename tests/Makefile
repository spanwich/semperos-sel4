CC       = gcc
CFLAGS   = -Wall -Wextra -Werror -std=c11 -g -O0
CFLAGS  += -I../components/include

SRCS     = test_ring.c ../src/vdtu_ring.c
TARGET   = test_ring

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
