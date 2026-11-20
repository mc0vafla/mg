CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread
PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 gstreamer-1.0)
PKG_LIBS   := $(shell pkg-config --libs gtk+-3.0 gstreamer-1.0) -lm

TARGET := mg
SRC    := main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $< -o $@ $(PKG_LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
