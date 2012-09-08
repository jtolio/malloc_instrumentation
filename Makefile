malloc_instrument.so:
	gcc -shared -fPIC -o malloc_instrument.so malloc_instrument.c \
			-lpthread -ldl

clean:
	rm -f *.so

.PHONY: clean
