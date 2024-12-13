CC = clang

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Darwin)
    INCLUDES = -I$(shell pg_config --includedir)
    LDFLAGS = -L$(shell pg_config --libdir)
else ifeq ($(UNAME_S), Linux)
    DEPLOYMENT_TARGET =
    INCLUDES = -I/usr/include/postgresql
    LDFLAGS = -L/usr/lib
endif

LDLIBS = -lpq

CFLAGS_RELEASE = -Wall -Wextra -pedantic -std=c11 -O3 -DNDEBUG -fomit-frame-pointer -ffast-math $(INCLUDES)

CFLAGS_DEBUG = -Wall -Wextra -pedantic -std=c11 -g -O0 -DDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer $(INCLUDES)

all: release

# Release build
release: CFLAGS = $(CFLAGS_RELEASE)
release: token_harvester

# Debug build
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: token_harvester

token_harvester: token_harvester.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ token_harvester.o $(LDLIBS)

token_harvester.o: token_harvester.c
	$(CC) $(CFLAGS) -c token_harvester.c

dep:
	$(CC) -MM $(CFLAGS_DEBUG) *.c > dependencies.mk

-include dependencies.mk

clean:
	rm -f token_harvester *.o dependencies.mk
