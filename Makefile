P=kilo
OBJECTS=
CFLAGS=-g -Wall -Wextra -pedantic
LDLIBS=
CC=c99

$(P): $(OBJECTS)

clean:
	rm ./kilo
