CC       = gcc
CFLAGS   = -Wall -Wextra -Werror -std=c11 -g -O0
CFLAGS  += -I../components/include

RING_SRCS   = test_ring.c ../src/vdtu_ring.c
CONFIG_SRCS = test_vdtu_config.c ../src/vdtu_ring.c ../src/vdtu_channels.c

TARGETS  = test_ring test_vdtu_config

.PHONY: all clean test

all: $(TARGETS)

test_ring: $(RING_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_vdtu_config: $(CONFIG_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGETS)
	./test_ring
	./test_vdtu_config

clean:
	rm -f $(TARGETS)
