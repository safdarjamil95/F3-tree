.PHONY: all clean
.DEFAULT_GOAL := all

LIBS=-lrt -lm -lpthread
INCLUDES=-I./include
CFLAGS=-O0 -std=c++11 -g 

output = fbtree_concurrent 
all: main

main: src/test_mix.cpp src/hash.h
	g++ $(CFLAGS) -o fbtree_concurrent src/test_mix.cpp $(LIBS) -DCONCURRENT 

clean: 
	rm $(output)
