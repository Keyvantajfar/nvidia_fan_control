CC=gcc
CFLAGS=-fPIC -O2 -Wall -Wextra
LDFLAGS=-shared

all: libnvidia-ml.so

libnvidia-ml.so: libnvidia-ml.c nvml.h
	$(CC) $(CFLAGS) -o $@ libnvidia-ml.c $(LDFLAGS)

clean:
	rm -f libnvidia-ml.so
