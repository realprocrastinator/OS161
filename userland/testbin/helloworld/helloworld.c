// directly do syscall!

#define STDOUT_FILENO 1

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

void hello(void);
void print_memories(void);
void basic_file_test(void);
void lseek_test(void);

void hello() {
	puts("I am doing nothing!");
}

void print_memories() {
	int a = 7;
	puts("");
	write(1,"test long text!\n", 16);
	printf("main()    = %p\n", print_memories);
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
}

void basic_file_test() {
	char buff[64];
	int rd;
	int fds[] = {open("/hello.txt", O_RDONLY), open("/hello.txt", O_RDONLY)};
	const int fd_count = sizeof(fds)/sizeof(int);
	for(int i=0; i<fd_count; ++i) {
		int fd = fds[i];
	
		if(fd < 0) {
			printf("Failed to open file: %d\n", errno);
			continue;
		}
		do {
			rd = read(fd, buff, sizeof(buff));
			if(rd < 0) {
				putchar('\n');
				printf("Failed to read file: %d\n", errno);
				continue;
				//goto finish;
			}
			for(int i=0; i<rd; ++i)
				putchar(buff[i]);
			if((size_t)rd < sizeof(buff))
				break;
		} while(1);
		putchar('\n');
	}

	for(int i=0; i<fd_count; ++i) 
		close(fds[i]);
	return;
}

void lseek_test(void) {
	const char* ref_fn = "/testfiles/append.dat";
	const char* tgt_fn = "/testfiles/largefile.dat";
	printf("sizeof off_t = %d\n", sizeof(off_t));
	
	char buff[64], buff2[64];
	int bufflen, bufflen2;
	
	// open the requisite files
	int ref_fd = open(ref_fn, O_RDONLY);
	int tgt_fd = open(tgt_fn, O_RDONLY);
	
	// read the reference to buff
	bufflen = read(ref_fd, buff, sizeof(buff));
	
	// seek into the target file
	off_t seekresult = lseek(tgt_fd, bufflen*-1, SEEK_END);
	size_t *seekresult_split = &seekresult;
	printf("Return lseek = %x and %x\n",seekresult_split[0], seekresult_split[1]);
	printf("New offset position: %lld\n", seekresult);
	if(seekresult < 0) {
		printf("Error seeking file: %d\n", errno);
		return;
	}
	//printf("New offset position: %ld\n", seekresult);
	
	// read target file
	bufflen2 = read(tgt_fd, buff2, sizeof(buff2));
	if(bufflen2 != bufflen) {
		printf("Wrong file length. Expected %d, got %d.\n", bufflen, bufflen2);
		return;
	} else {
		printf("File length match!\n");
	}
	
	// compare
	write(1, "Expected: ", 10);
	write(1, buff, bufflen);
	write(1, "\n", 1);
	write(1, "Got     : ", 10);
	write(1, buff2, bufflen2);
	write(1, "\n", 1);
	
	int match = 1;
	for(int i=0; i<bufflen; ++i) {
		if(buff[i]!=buff2[i]) {
			match = 0;
			break;
		}
	}
	
	printf("%s\n", match ? "Match!" : "Mismatch!");
}

int main() {
	lseek_test();
}
