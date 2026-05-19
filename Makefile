CC = gcc
CFLAGS = -Wall -Wextra -I md4c/src -I sds
LDFLAGS =

mdrender: mdrender.c utils_ut8.c md4c/src/md4c.c sds/sds.c
	$(CC) $(CFLAGS) -o mdrender mdrender.c utils_ut8.c md4c/src/md4c.c sds/sds.c $(LDFLAGS)

clean:
	rm -f mdrender

.PHONY: clean
