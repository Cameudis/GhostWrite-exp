all:
	gcc -march="rv64gczve64x" -no-pie -o main main.c
