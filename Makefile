CC      = gcc
NASM    = nasm
WINDRES = windres

CFLAGS  = -O2 -Wall -Wextra -std=c11 -DUNICODE -D_UNICODE
LDFLAGS = -mwindows -municode
LIBS    = -lz -lcomctl32 -lcomdlg32 -lshell32 -lole32

TARGET  = snowzip.exe
OBJECTS = snowzip.o snowzip_asm.o snowzip_res.o

.PHONY: all clean

all: $(TARGET)

snowzip.o: snowzip.c snowzip.h
	$(CC) $(CFLAGS) -c snowzip.c -o snowzip.o

snowzip_asm.o: snowzip.asm
	$(NASM) -f win64 snowzip.asm -o snowzip_asm.o

snowzip_res.o: snowzip.rc snowzip.ico
	$(WINDRES) snowzip.rc -O coff -o snowzip_res.o

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $(TARGET) $(LIBS)

clean:
	rm -f $(OBJECTS) $(TARGET)
