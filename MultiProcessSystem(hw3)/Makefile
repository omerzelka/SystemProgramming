CC = cc
CFLAGS = -Wall -Wextra -pedantic -std=c11
LDFLAGS = -pthread

TARGET = hw3
SRC = hw3.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
