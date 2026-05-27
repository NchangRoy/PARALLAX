all: bin/parallax

bin/parallax: parallax_team.c utils/barrier.c utils/linked_list.c | bin
	gcc parallax_team.c utils/barrier.c utils/linked_list.c -o bin/parallax -Iutils

bin:
	mkdir -p bin

run: all
	./bin/parallax

clean:
	rm -rf bin