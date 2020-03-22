// directly do syscall!

#define STDOUT_FILENO 1

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

void hello(void);
void print_memories(void);
void basic_file_test(void);
void lseek_test(void);
void dup2_test(void);

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
	
	int match = 1;
	for(int i=0; i<bufflen; ++i) {
		if(buff[i]!=buff2[i]) {
			match = 0;
			break;
		}
	}
	
	printf("%s\n", match ? "Match!" : "Mismatch!");
}

void dup2_test() {
	int result;
	int fd1 = open("/testfiles/append.dat", O_RDONLY);
	if(fd1 < 0) {
		printf("Error opening file 1: %d\n", errno);
		return;
	}
	
	int fd2 = open("/testfiles/append.dat", O_RDONLY);
	if(fd2 < 0) {
		printf("Error opening file 2: %d\n", errno);
		return;
	}
	
	printf("Got FD1 = %d and FD2 = %d.\n", fd1, fd2);
	
	// try duplicate to an invalid file pointer
	result = dup2(fd1, 7);
	if(result < 0) {
		printf("Failed! dup2 on invalid FD failed with error %d!\n", errno);
		return;
	} else {
		printf("Success!! dup2 on invalid FD succeeded.\n");
		close(7);
	}
	
	// dup2 fd1 to fd2
	result = dup2(fd1, fd2);
	if(result < 0) {
		printf("dup2 failed with error %d!\n", errno);
		return;
	}
	
	// close fd2
	//close(fd2);
	close(fd1);
	
	// can we still read fd1?
	int buff;
	result = read(fd2, &buff, sizeof(buff));
	if(result < 0) {
		// closing duplicated FD should not close the other and vice versa
		printf("Failed! Because read failed with error %d\n", result);
	}
	else {
		printf("Success! Because read succeeded.\n");
	}
}

int main() {
	dup2_test();
}
