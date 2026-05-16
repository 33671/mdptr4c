CC = gcc
CFLAGS = -Wall -Wextra -I md4c/src
LDFLAGS =

mdrender: mdrender.c utils_ut8.c md4c/src/md4c.c
	$(CC) $(CFLAGS) -o mdrender mdrender.c utils_ut8.c md4c/src/md4c.c $(LDFLAGS)

clean:
	rm -f mdrender

.PHONY: clean
