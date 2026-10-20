CC		= clang
CFLAGS	= -Wall -std=gnu11 -Wextra
OUT		= mdbg
IN		= mdbg.c

$(OUT): $(IN)
	$(CC) $(CFLAGS) $< -o $@

.PHONY: test test-bt

test: test.c
	$(CC) -g -no-pie $< -o $@

test-bt: test-bt.c
	$(CC) -no-pie -g -O0 -fno-omit-frame-pointer $< -o $@
