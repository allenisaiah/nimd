CC := gcc
CFLAGS := -std=c99 -g -Wall -Wextra
LDFLAGS := -fsanitize=address,undefined

all: nimd

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

nimd: mysh.o arraylist.o
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $^

clean:
	rm -f nimd *.o
