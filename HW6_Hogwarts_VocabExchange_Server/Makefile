CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11
TARGETS = hogwarts wizard professor

.PHONY: all clean

all: $(TARGETS)

hogwarts: hogwarts.c
	$(CC) $(CFLAGS) -o hogwarts hogwarts.c

wizard: wizard.c
	$(CC) $(CFLAGS) -o wizard wizard.c

professor: professor.c
	$(CC) $(CFLAGS) -o professor professor.c

clean:
	rm -f $(TARGETS) *.o
