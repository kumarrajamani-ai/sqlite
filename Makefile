CC ?= cc
UNAME_S := $(shell uname -s)

CFLAGS  ?= -O2 -fPIC -Wall -Wextra -I vendor
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null || curl-config --cflags)
CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null || curl-config --libs)

ifeq ($(UNAME_S),Darwin)
  TARGET := http.dylib
  SHARED_FLAGS := -bundle -undefined dynamic_lookup
else
  TARGET := http.so
  SHARED_FLAGS := -shared
endif

.PHONY: all clean test macos-universal

all: $(TARGET)

$(TARGET): http.c
	$(CC) $(CFLAGS) $(CURL_CFLAGS) $(SHARED_FLAGS) -o $@ http.c $(CURL_LIBS)

# Build an arm64 + x86_64 universal .dylib on macOS via lipo.
macos-universal: http.c
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -target arm64-apple-macos11 -bundle -undefined dynamic_lookup \
		-o http.arm64.dylib http.c $(CURL_LIBS)
	$(CC) $(CFLAGS) $(CURL_CFLAGS) -target x86_64-apple-macos11 -bundle -undefined dynamic_lookup \
		-o http.x86_64.dylib http.c $(CURL_LIBS)
	lipo -create -output http.dylib http.arm64.dylib http.x86_64.dylib
	rm -f http.arm64.dylib http.x86_64.dylib
	lipo -info http.dylib

test: $(TARGET)
	./tests/run_tests.sh

clean:
	rm -f http.so http.dylib http.arm64.dylib http.x86_64.dylib
