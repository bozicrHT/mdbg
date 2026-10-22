CC		= clang
CFLAGS	= -Wall -std=gnu11 -Wextra
OUT		= mdbg
IN		= mdbg.c dwarf/section-line.c

$(OUT): $(IN)
	$(CC) $(CFLAGS) $^ -o $@

.PHONY: test test-next

test: test.c
	$(CC) -g -no-pie $< -o $@

test-next: test-next.c
	$(CC) -g -gdwarf-5 -no-pie $< -o $@
