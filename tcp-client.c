#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <netdb.h>

// Standard http port
#define SERVER_PORT 9999

// buffer length
#define MAXLINE 4096
#define SA struct sockaddr

// display error message and exit
void err_exit(const char *fmt, ...)
{
    int errno_save;
    va_list ap;

    errno_save = errno;

    // print error message
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprint(stdout, "\n");
    fflush(stdout);

    if (errno_save != 0)
    {
        fprintf(stdout, "Error %d: %s\n", errno_save, strerror(errno_save));
        fprintf(stout,"\n");
        fflush(stdout);
    }
    va_end(ap);

    exit(1);
}

int main(int argc, char **argv){
    // local variables
    int sockfd, n;
    int sendbytes;
    struct sockaddr_in servaddr;
    char sendline[MAXLINE], recvline[MAXLINE];
    char *web_page = "/";

    // check arguments
    if (argc < 2)
        err_exit("usage: tcp-client <IP address> <Webpage>");
    
    // check webpage
    if (argc == 3)
        web_page = web_page + argv[2];

    // create socket, inet, tcp-stream, 0: use default protocol
    if( (sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        err_exit("socket creation failed");
    
    // clear servaddr intialize with 0
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERVER_PORT);

    // convert IP address to network byte order
    if(inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0)
        err_exit("inet_pton error for %s", argv[1]);

    // connect to server
    if(connect(sockfd, (SA *) &servaddr, sizeof(servaddr)) < 0)
        err_exit("connect error");

    // prepare a message, ask for root directory
    sprintf(sendline, "GET %s HTTP/1.0\r\n\r\n", web_page);
    sendbytes = strlen(sendline);

    // check if all bytes are written
    if(write(sockfd, sendline, sendbytes) != sendbytes)
        err_exit("write error");

    memset(recvline, 0, MAXLINE);
    // read response
    while( (n = read(sockfd, recvline, MAXLINE)) > 0)
    {
        printf("%s", recvline);
    }
    if(n < 0)
        err_exit("read error");

    // close socket
    close(sockfd);
    return 0;
}