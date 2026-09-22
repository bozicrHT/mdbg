CC		= clang
CFLAGS	= -Wall -std=gnu11 -Wextra
OUT		= mdbg
IN		= mdbg.c

$(OUT): $(IN)
	$(CC) $(CFLAGS) $< -o $@

.PHONY: test

test: test.c
	$(CC) -g $< -o $@
