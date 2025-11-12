# Makefile

CC = gcc
CFLAGS = -std=gnu11 -O2 -pthread -Wall -Wextra -D_GNU_SOURCE
TARGET = syswatch

all: $(TARGET)

$(TARGET): syswatch.c
	$(CC) $(CFLAGS) -o $(TARGET) syswatch.c

install: $(TARGET)
	install -d $(DESTDIR)/usr/local/bin
	install -m 0755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean install