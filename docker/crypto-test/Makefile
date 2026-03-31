HACL_DIR = ../../hacl-star/dist/gcc-compatible
KARAMEL_DIR = ../../hacl-star/dist/karamel
COMP_DIR = ../../components/DTUBridge

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -DCT_DEBUG \
         -I$(COMP_DIR) \
         -I$(HACL_DIR) \
         -I$(HACL_DIR)/internal \
         -I$(KARAMEL_DIR)/include \
         -I$(KARAMEL_DIR)/krmllib/dist/minimal

HACL_SRC = $(HACL_DIR)/Hacl_AEAD_Chacha20Poly1305.c \
           $(HACL_DIR)/Hacl_Chacha20.c \
           $(HACL_DIR)/Hacl_MAC_Poly1305.c

all: crypto_test

crypto_test: crypto_test.c $(HACL_SRC)
	$(CC) $(CFLAGS) -o $@ crypto_test.c $(HACL_SRC)

clean:
	rm -f crypto_test

.PHONY: all clean
