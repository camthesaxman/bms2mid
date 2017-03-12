CC:=gcc
CFLAGS:=-std=c99 -Wall -Wextra -Wpedantic -Wno-sign-compare -O0 -g -DDEBUG

bms2mid: bms2mid.c
	$(CC) $(CFLAGS) $^ -o $@
 
clean:
	$(RM) bms2mid
