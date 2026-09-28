# CSE344 HW4 - Makefile
# Multi-Process & Multi-Threaded Concurrent Log Analysis Pipeline

CC = gcc
CFLAGS = -Wall -Wextra -pthread
LDFLAGS = -pthread
UNAME_S := $(shell uname -s)
ifneq ($(UNAME_S),Darwin)
    LDFLAGS += -lrt
endif

TARGET = analyzer

SRCS = main.c reader.c dispatcher.c analyzer.c aggregator.c shm.c watchdog.c
HDRS = reader.h dispatcher.h analyzer.h aggregator.h shm.h watchdog.h
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS) *.tmp
