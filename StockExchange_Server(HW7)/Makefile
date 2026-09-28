CC = gcc
CFLAGS = -Wall -Wextra -std=c11
TARGETS = ServerTrd trader analyst ticker

all: $(TARGETS)

ServerTrd: ServerTrd.c
	$(CC) $(CFLAGS) -o $@ $<

trader: trader.c
	$(CC) $(CFLAGS) -o $@ $<

analyst: analyst.c
	$(CC) $(CFLAGS) -o $@ $<

ticker: ticker.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean
