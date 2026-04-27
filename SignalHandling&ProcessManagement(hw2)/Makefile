CC = gcc
CFLAGS = -Wall -g

TARGET = procSearch

all: $(TARGET)

$(TARGET): try.c
	$(CC) $(CFLAGS) try.c -o $(TARGET)

clean:
	rm -f $(TARGET)
	@echo "The $(TARGET) file deleted."