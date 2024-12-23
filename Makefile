CC = clang

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Darwin)
    INCLUDES = -I$(shell pg_config --includedir) -I/usr/local/include
    LDFLAGS = -L$(shell pg_config --libdir) -L/usr/local/lib
    LDLIBS = -lpq -lssl -lcrypto
else ifeq ($(UNAME_S), Linux)
    DEPLOYMENT_TARGET =
    INCLUDES = -I/usr/include/postgresql -I/usr/include/openssl
    LDFLAGS = -L/usr/lib -L/usr/lib/x86_64-linux-gnu
    LDLIBS = -lpq -lssl -lcrypto
endif

CFLAGS_RELEASE = -Wall -Wextra -pedantic -std=c99 -O2 -DNDEBUG \
                 -march=native -mtune=native -fomit-frame-pointer \
                 -ffast-math -flto -fvisibility=hidden -fstrict-aliasing \
                 -fno-plt -fstack-protector-strong $(INCLUDES)

ifeq ($(UNAME_S), Linux)
    CFLAGS_RELEASE += -D_POSIX_C_SOURCE=199309L
endif

CFLAGS_DEBUG = -Wall -Wextra -pedantic -std=c99 -g -O0 -DDEBUG -fno-omit-frame-pointer $(INCLUDES)

SOURCES = src/main.c src/db.c src/hmac.c src/base64.c src/log.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = batcher

all: release

release: CFLAGS = $(CFLAGS_RELEASE)
release: $(TARGET)
	strip $(TARGET)

debug: CFLAGS = $(CFLAGS_DEBUG)
debug: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

dep:
	$(CC) -MM $(CFLAGS_DEBUG) $(SOURCES) > dependencies.mk

-include dependencies.mk

clean:
	rm -f $(TARGET) $(OBJECTS) dependencies.mk
