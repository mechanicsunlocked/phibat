# phibat build
#   Linux (cross):  make              needs mingw-w64
#   Windows MSYS2:  make CROSS=       in a UCRT64/MINGW64 shell

VERSION := 0.1.0
CROSS   ?= x86_64-w64-mingw32-
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE
LDFLAGS := -static -s -mwindows -municode
LIBS    := -luser32 -lgdi32 -lshell32 -lsetupapi -ladvapi32

all: build/phibat.exe

build:
	mkdir -p build

build/phibat.o: src/phibat.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/phibat.res.o: res/phibat.rc res/phibat.manifest | build
	$(WINDRES) -I res res/phibat.rc -O coff -o $@

build/phibat.exe: build/phibat.o build/phibat.res.o
	$(CC) $^ -o $@ $(LDFLAGS) $(LIBS)

dist: build/phibat.exe
	rm -f build/phibat-$(VERSION)-x64.zip
	cd build && cp ../README.md ../LICENSE . && zip -9 phibat-$(VERSION)-x64.zip phibat.exe README.md LICENSE

clean:
	rm -rf build

.PHONY: all dist clean
