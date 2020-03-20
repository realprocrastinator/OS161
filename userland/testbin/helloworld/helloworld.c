// directly do syscall!

#define STDOUT_FILENO 1

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

void hello(void);

void hello() {
	puts("I am doing nothing!");
}

int main() {
	int a = 7;
	puts("");
	printf("main()    = %p\n", main);
	printf("hello()   = %p\n", hello);
	printf("puts()    = %p\n", puts);
	printf("stack var = %p\n", &a);
	printf("const str = %p\n", "Alpaca!");
	
	char* b = (char*)0x7ffe0000;
	int written = write(1,b,1);
	if(written < 0) {
		printf("Cannot write data: %d\n", errno);
	} else {
		printf("Written rogue %d bytes!\n", written);
	}
	
    return 0;
}
