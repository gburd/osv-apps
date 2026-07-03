.PHONY: module
module: io-uring-bench

CFLAGS = -std=gnu99 -O2 -Wall -Wextra -fpie

io-uring-bench: io-uring-bench.c
	$(CC) -pie -o $@ $(CFLAGS) $(LDFLAGS) io-uring-bench.c

clean:
	rm -f io-uring-bench
