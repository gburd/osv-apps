.PHONY: module
module: block-bench

CFLAGS = -std=gnu99 -O2 -Wall -Wextra -fpie

block-bench: block-bench.c
	$(CC) -pie -o $@ $(CFLAGS) $(LDFLAGS) block-bench.c -lpthread

clean:
	rm -f block-bench
