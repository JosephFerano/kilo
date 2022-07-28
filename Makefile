kilo: kilo.c
	$(CC) kilo.c -o kilo -Wall -Wextra -pedantic -std=c99

debug: kilo.c
	$(CC) kilo.c -o kilo -ggdb -Wall -Wextra -pedantic -std=c99

clean:
	rm ./kilo
