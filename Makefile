CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra
fileward: fileward.c
	$(CC) $(CFLAGS) -o $@ $<
test: fileward
	sh tests/test.sh
clean:
	rm -f fileward
.PHONY: test clean
