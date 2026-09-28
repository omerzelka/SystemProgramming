CC = gcc
CFLAGS = -Wall -g

TARGET = myFind

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c -o $(TARGET)

clean:
	rm -f $(TARGET)
	@echo "The $(TARGET) file deleted."