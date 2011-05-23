OBJS = 64drive.o dir.o disk.o fat.o file.o fs.o

CFLAGS = -DLINUX -g -Wall
LDFLAGS = -lm

all: fs debug dragon_debug

fs: $(OBJS) main.o
	$(CC) -o fs $(OBJS) main.o $(LDFLAGS)

debug: $(OBJS) debug.o
	$(CC) -o debug $(OBJS) debug.o $(LDFLAGS)

dragon_debug: $(OBJS) libdragon.o
	$(CC) -o dragon_debug $(OBJS) libdragon.o $(LDFLAGS)

clean:
	rm -f fs $(OBJS) main.o debug.o
