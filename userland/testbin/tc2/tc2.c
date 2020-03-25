#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

const char* buff = "The quick brown fox jumps over the lazy dog. ";
const char* filename = "test.txt";
const int buff_len = 45;

int main() {
	int fd01,fd02,fd03,fd04,result;
	char lbuff[128];

    // test 0 - check if truncate works
    puts("Ultra basic test! Check if truncate works (below should result in 0).");
    fd01 = open(filename, O_CREAT|O_TRUNC|O_RDWR, 0755);
    if(fd01 < 0)
		goto error;
    printf("Got #1 FD %d\n", fd01);
    result = read(fd01, lbuff, sizeof(lbuff));
    printf("Got %d bytes\n", result);
    close(fd01);
    puts("");
	
	// test 1 - basic file write
	puts("Basic test!");
	puts("Open file for writing");
	fd01 = open(filename, O_CREAT|O_TRUNC|O_WRONLY, 0755);
	if(fd01 < 0)
		goto error;
	printf("Got #1 FD %d\n", fd01);

    // test 1.1 - try reading on WRONLY
    puts("Try reading ...");
    result = read(fd01, lbuff, 1);
    if(result >= 0) {
        puts("Doesn't expect read to success on WRONLY file.");
        return 0;
    }
    printf("Success! Read failed with error: %d\n", errno);
	
	printf("Writing %d bytes\n", buff_len);
	result = write(fd01, buff, buff_len);
	if(result < 0)
		goto error;
	printf("Writing %d bytes again\n", buff_len);
	result = write(fd01, buff, buff_len);
	if(result < 0)
		goto error;
	
	// test 2 - basic file read
	puts("Open file for reading");
	fd02 = open(filename, O_RDONLY);
	if(fd02 < 0)
		goto error;
	printf("Got #2 FD %d\n", fd02);
	
	// 2.1 - try read closed file
	printf("Closing #1 FD: %d\n", fd01);
	close(fd01);
	printf("Reading #1 FD %d with %d bytes\n", fd01, sizeof(lbuff));
	result = read(fd01, lbuff, sizeof(lbuff));
	if(result >= 0) {
		puts("Error! Read file success.");
		return 0;
	}
	puts("Success! Read file failed.");
	
	// 2.2 - try read all
	printf("Reading FD %d with %d bytes\n", fd02, sizeof(lbuff));
	result = read(fd02, lbuff, sizeof(lbuff));
	if(result < 0)
		goto error;
    printf("Got %d bytes ... ", result);
    if(result != buff_len * 2) {
        puts("File size mismatch");
        return 0;
    }
    puts("File size OK");
    printf("Re-reading FD again %d with %d bytes\n", fd02, sizeof(lbuff));
	result = read(fd02, lbuff, sizeof(lbuff));
	if(result < 0)
		goto error;
    printf("Got %d bytes ... ", result);
    if(result) {
        puts("File size mismatch");
        return 0;
    }
    puts("File size OK");
    // check retreived data
    if(!(memcmp(buff, lbuff, buff_len) == 0 && memcmp(buff, lbuff + buff_len, buff_len) == 0)) {
        puts("File corrupted!");
        return 0;
    }
    puts("Read buffer is OK");

    // 2.3 try write
    puts("Try writing ...");
    result = write(fd02, buff, buff_len);
    if(result >= 0) {
        puts("Doesn't expect write to success on RDONLY file.");
        return 0;
    }
    printf("Success! Write failed with error: %d\n", errno);
    puts("");

#ifdef APPEND_TEST
    // 3 - append test
    puts("Open file for append write");
	fd01 = open(filename, O_APPEND | O_WRONLY);
	if(fd01 < 0)
		goto error;
	printf("Got #1 FD %d\n", fd01);
    
    printf("Appending %d bytes\n", buff_len);
	result = write(fd01, buff, buff_len);
	if(result < 0)
		goto error;
    
    // 3.1 - read appended file
    puts("Trying re-read appended data from the existing FD");
	memset(lbuff, 0, sizeof(lbuff));
    result = read(fd02, lbuff, sizeof(lbuff));
    if(result < 0)
		goto error;
    printf("Got %d bytes ... ", result);
    if(result != buff_len) {
        puts("File size mismatch");
        return 0;
    }
    if(memcmp(lbuff, buff, buff_len) != 0) {
        puts("File corrupted!");
        return 0;
    }
    puts("Read buffer is OK");
    puts("");
    printf("Closing append FD: %d\n", fd01);
	close(fd01);
#endif
    
    // 4 - dup2 test (FD02 still open!)
    puts("Opening R/W file truncate for dup2 test");
    fd03 = open(filename, O_CREAT|O_TRUNC|O_RDWR, 0755);
    if(fd03 < 0)
        goto error;
    printf("Got #3 FD %d. This FD will be dup2-ed 3 times.\n", fd03);
    // 4.0 - test read first, ensure we got 0 bytes but not error
    printf("Testing read from FD %d\n", fd02);
    result = read(fd02, lbuff, sizeof(lbuff));
    if(result < 0)
        goto error;
    if(result) {
        printf("Read failed. Expecting none, got %d bytes instead.\n", result);
        return 0;
    }
    puts("Success! Got 0 bytes.");
    printf("Testing read from FD %d\n", fd03);
    result = read(fd02, lbuff, sizeof(lbuff));
    if(result < 0)
        goto error;
    if(result) {
        printf("Read failed. Expecting none, got %d bytes instead.\n", result);
        return 0;
    }
    puts("Success! Got 0 bytes.");
    // write to fd03 first
    puts("Trying write again");
    result = write(fd03, lbuff, buff_len);
    if(result < 0)
        goto error;
    // 4.1 the real dup2 test!
    puts("dup2 to #2 FD (which is still open)");
    result = dup2(fd03, fd02);
    if(result < 0)
        goto error;
    puts("Success!");
    fd01 = 2;
    puts("dup2 to stderr");
    result = dup2(fd03, fd01);
    if(result < 0)
        goto error;
    puts("Success!");
    puts("dup2 to some larger FD");
    fd04 = 78;
    result = dup2(fd03, fd04);
    if(result < 0)
        goto error;
    puts("Success!");
    puts("dup2 to myself");
    fd04 = 78;
    result = dup2(fd03, fd03);
    if(result < 0)
        goto error;
    puts("Success!");
    puts("dup2 to negative");
    fd04 = 78;
    result = dup2(fd03, -9);
    if(result >= 0) {
        puts("Success? Perhaps not!");
        return 0;
    }
    printf("Success! Got error %d\n", errno);
    
    // seek fd01, close it, close originating (fd03), and read from fd04
    printf("Try lseek to start from FD %d\n", fd01);
    result = lseek(fd01, 0, SEEK_SET);
    if(result < 0)
        goto error;
    puts("Seek OK");
    puts("Close (formerly) stderr's FD");
    close(fd01);
    // read
    printf("Try read from FD %d\n", fd04);
    result = read(fd04, lbuff, sizeof(lbuff));
    if(result < 0)
        goto error;
    if(result != buff_len) {
        printf("Read failed. Expecting none, got %d bytes instead.\n", result);
        return 0;
    }
    if(memcmp(lbuff, buff, buff_len) != 0) {
        puts("Data corrupted");
        return 0;
    }
    puts("Data seems OK");
    
    // seek one last time
    printf("Try seek again on FD %d\n", fd02);
    result = lseek(fd02, -10, SEEK_END);
    if(result < 0)
        goto error;
    printf("Reading from FD %d. Expected output: 'lazy dog' (8 bytes).\n", fd04);
    result = read(fd04, lbuff, 8);
    if(result < 0)
        goto error;
    lbuff[8] = 0;
    printf("Got %d bytes which read: '%s'\n", result, lbuff);
    
    // close all
    puts("Closing all FDs");
    close(fd01);
    close(fd02);
    close(fd03);
    close(fd04);
    
    // end of test
    puts("End of test! Fingers crossed!");
    return 0;
    
    // TODO: 4 - dup2 test
	
		
error:
	printf("Error: %d\n", errno);
	return 0;
}