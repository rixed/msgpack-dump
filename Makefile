CFLAGS = -W -Wall -std=c99 -O3
#CFLAGS = -W -Wall -std=c99 -O0 -ggdb

all: msgpack-dump

.PHONY: clean distclean

clean:
	$(RM) *.o *.s

distclean: clean
	$(RM) msgpack-dump
