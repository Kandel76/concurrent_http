EXEC = httpserver
SRC = $(wildcard *.c)
HEADERS = $(wildcard *.h)
OBJ = $(SRC:%.c=%.o)
LIBRARY = funcs.a
FORMATS = $(SOURCES:%.c=.format/%.c.fmt) $(HEADERS:%.h=.format/%.h.fmt)

CC = clang
FORMAT = clang-format
CFLAGS = -Wall -Wpedantic -Werror -Wextra -DDEBUG

.PHONY: all clean format

all: $(EXEC)
	@echo "Objects: $(OBJ)"
	@echo "Source files: $(SRC)"
	@echo "Headers: $(HEADERS)"
	@echo "Linked libraries: $(LDFLAGS)"

$(EXEC): $(OBJ) $(LIBRARY)
	$(CC) -o $@ $^

%.o: %.c %.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(EXEC) $(OBJ)

nuke: clean
	rm -rf .format

format: $(FORMATS)

.format/%.c.fmt: %.c
	mkdir -p .format
	$(FORMAT) -i $<
	touch $@

.format/%.h.fmt: %.h
	mkdir -p .format
	$(FORMAT) -i $<
	touch $@
