CC = gcc
CFLAGS = -Wall -Wextra -std=c11

COMMON = tftp.o udp.o file.o

all: client server

client: client.o $(COMMON)
	$(CC) $(CFLAGS) -o client client.o $(COMMON)

server: server.o $(COMMON)
	$(CC) $(CFLAGS) -o server server.o $(COMMON)

client.o: client.c tftp.h udp.h file.h
	$(CC) $(CFLAGS) -c client.c

server.o: server.c tftp.h udp.h file.h
	$(CC) $(CFLAGS) -c server.c

tftp.o: tftp.c tftp.h file.h
	$(CC) $(CFLAGS) -c tftp.c

udp.o: udp.c udp.h
	$(CC) $(CFLAGS) -c udp.c

file.o: file.c file.h tftp.h
	$(CC) $(CFLAGS) -c file.c

clean:
	rm -f client server *.o
