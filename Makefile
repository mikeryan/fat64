OBJS = 64drive.o dir.o disk.o fat.o file.o fs.o

CFLAGS = -DLINUX -g -Wall -Werror
LDFLAGS = -lm

all: fs debug fuse64

fs: $(OBJS) main.o
	$(CC) -o fs $(OBJS) main.o $(LDFLAGS)

debug: $(OBJS) debug.o
	$(CC) -o debug $(OBJS) debug.o $(LDFLAGS)

fuse64: $(OBJS) fuse.o
	$(CC) -o fuse64 $(OBJS) fuse.o $(LDFLAGS) `pkg-config fuse --libs`

fuse.o: fuse.c
	$(CC) -c -o fuse.o fuse.c $(CFLAGS) `pkg-config fuse --cflags`

clean:
	rm -f fs fuse64 $(OBJS) main.o debug.o fuse.o
