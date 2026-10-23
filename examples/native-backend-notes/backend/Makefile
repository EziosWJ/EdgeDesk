CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Werror
LDFLAGS ?=
TARGET := notes-backend

.PHONY: all clean

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CPPFLAGS) -std=c11 $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
