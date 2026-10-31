CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Werror -Isrc $(EXTRA)
BIN     := build/quarry
SRCS    := src/main.c src/pager.c src/btree.c src/record.c src/sql.c src/db.c
CORE    := src/pager.c src/btree.c src/record.c src/sql.c src/db.c

all: $(BIN)

$(BIN): $(SRCS) src/quarry.h src/sql.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) -o $@

check: $(BIN)
	./tests/run-tests.sh

clean:
	rm -rf build

.PHONY: all check clean
