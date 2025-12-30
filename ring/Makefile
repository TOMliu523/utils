TARGET=test

all:
	gcc -Ofast -Wall -Werror -o $(TARGET) main.c ring.c -lpthread
	#gcc -g -O0 -Wall -Werror -fno-omit-frame-pointer -fsanitize=address -pthread -o test main.c ring.c

clean:
	rm -rf $(TARGET)

run:
	./$(TARGET)
