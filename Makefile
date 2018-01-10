CC=gcc
CFLAGS= -g -O0 -pthread
#-Wall -Werror
LFLAGS=-lssl -lcrypto
CACHE_PATH=cache
SSRCS=proxyserver.c
CACHE_FILES=$(CACHE_PATH)/ *
PROGRAMS=proxyserver

#Default make command builds executable file
all: $(PROGRAMS)

proxyserver: $(SSRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LFLAGS)

.PHONY: clean

clean:
	rm -rf $(PROGRAMS)
