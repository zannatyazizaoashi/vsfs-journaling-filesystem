CC = gcc
CFLAGS = -std=gnu11 -Wall -Wextra

all: mkfs journal validator

mkfs: mkfs.c
	$(CC) $(CFLAGS) -o $@ $<

journal: journal.c
	$(CC) $(CFLAGS) -o $@ $<

validator: validator.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: all
