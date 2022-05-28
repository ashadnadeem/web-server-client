// import libraries
#include <arpa/inet.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// define constants
// Listenq is the number of connections the server will be able to handle
#define LISTENQ  1024
// maxline is the maximum length of a request line
#define MAXLINE 1024
// riobufsize is the size of the read io buffer
#define RIO_BUFSIZE 1024

// rio_t is a structure
typedef struct {
    int rio_fd;                 /* descriptor for this buf */
    int rio_cnt;                /* unread byte in this buf */
    char *rio_bufptr;           /* next unread byte in this buf */
    char rio_buf[RIO_BUFSIZE];  /* internal buffer */
} rio_t;

// socket address
typedef struct sockaddr SA;

// http_request structure
typedef struct {
    char filename[512];
    off_t offset;              /* for support Range */
    size_t end;
} http_request;

// mime_map structure holds the mime type and the file extension
typedef struct {
    const char *extension;
    const char *mime_type;
} mime_map;

// array of mime types
mime_map meme_types [] = {
    {".css", "text/css"},
    {".gif", "image/gif"},
    {".htm", "text/html"},
    {".html", "text/html"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".ico", "image/x-icon"},
    {".js", "application/javascript"},
    {".pdf", "application/pdf"},
    {".mp4", "video/mp4"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".xml", "text/xml"},
    {NULL, NULL},
};

char *default_mime_type = "text/plain";

// inittialize the read io buffer
void rio_readinitb(rio_t *rp, int fd){
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

// write n bytes from buffer to descriptor
ssize_t writen(int fd, void *usrbuf, size_t n){
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;
    // loop until all bytes are written
    while (nleft > 0){
        if ((nwritten = write(fd, bufp, nleft)) <= 0){
            // interrupted by sig handler return
            if (errno == EINTR)
                // if error is EINTR, try again
                nwritten = 0;
            else
                // error return
                return -1;
        }
        // number of bytes left to write is decreased by number of bytes written
        nleft -= nwritten;
        // buffer pointer is increased by number of bytes written
        bufp += nwritten;
    }
    return n;
}


// transfers min(n, rio_cnt) bytes from an internal buffer to a user buffer
    // n is the number of bytes requested by the user
    // rio_cnt is the number of unread bytes in the internal buffer
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n){
    int cnt;
    while (rp->rio_cnt <= 0){
        // refill internal buffer
        // read(file descriptor, buffer, size of buffer)
        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0){
            // interrupted by sig handler return
            if (errno != EINTR)
                return -1;
        }
        else if (rp->rio_cnt == 0)
            // EOF return 0
            return 0;
        else
            // reset buffer pointer
            rp->rio_bufptr = rp->rio_buf;
    }

    // Copy min(n, rp->rio_cnt) bytes from internal buf to user buf
    cnt = n;
    if (rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

// read a line from a descriptor
// return the number of bytes read
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen){
    int n, rc;
    char c, *bufp = usrbuf;
    // loop until a newline is found or EOF
    for (n = 1; n < maxlen; n++){
        if ((rc = rio_read(rp, &c, 1)) == 1){
            // successfully read a byte
            // put the char-byte into the buffer
            *bufp++ = c;
            if (c == '\n')
                // newline found, break
                break;
        } else if (rc == 0){
            // Unsuccessful read of char-byte
            if (n == 1)
                // EOF No data found return 0
                return 0;
            else
                // EOF,some data was read break
                break;
        } else
            // error return -1
            return -1;
    }
    // buffer_pointer is 0 terminated
    *bufp = 0;
    // return the number of bytes read
    return n;
}

// format the size of a file
void format_size(char* buf, struct stat *stat){
    if(S_ISDIR(stat->st_mode)){
        // directory 
        sprintf(buf, "%s", "[DIR]");
    } else {
        // file
        off_t size = stat->st_size;
        if(size < 1024){
            // file size in bytes
            sprintf(buf, "%lu", size);
        } else if (size < 1024 * 1024){
            // file size in kilobytes
            sprintf(buf, "%.1fK", (double)size / 1024);
        } else if (size < 1024 * 1024 * 1024){
            // file size in megabytes
            sprintf(buf, "%.1fM", (double)size / 1024 / 1024);
        } else {
            // file size in gigabytes
            sprintf(buf, "%.1fG", (double)size / 1024 / 1024 / 1024);
        }
    }
}
// directory listing
void handle_directory_request(int out_fd, int dir_fd, char *filename){
    char buf[MAXLINE], m_time[32], size[16];
    struct stat statbuf;
    // show the response header
    sprintf(buf, "HTTP/1.1 200 OK\r\n%s%s%s%s%s",
            "Content-Type: text/html\r\n\r\n",
            "<html><head><style>",
            "body{font-family: monospace; font-size: 13px;}",
            "td {padding: 1.5px 6px;}",
            "</style></head><body><table>\n");

    writen(out_fd, buf, strlen(buf));
    // open the directory
    DIR *d = fdopendir(dir_fd);
    struct dirent *dp;
    int ffd;
    // loop through the directory
    while ((dp = readdir(d)) != NULL){
        if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")){
            // skip the current and parent directory
            continue;
        }
        if ((ffd = openat(dir_fd, dp->d_name, O_RDONLY)) == -1){
            // openening the directory failed
            perror(dp->d_name);
            continue;
        }
        // get the file statistic
        // fstat(file descriptor, stat buffer)
        fstat(ffd, &statbuf);
        // format the time of last modification
        strftime(m_time, sizeof(m_time), "%Y-%m-%d %H:%M", localtime(&statbuf.st_mtime));
        // format the file size with our own function
        format_size(size, &statbuf);
        if(S_ISREG(statbuf.st_mode) || S_ISDIR(statbuf.st_mode)){
            char *d = S_ISDIR(statbuf.st_mode) ? "/" : "";
            sprintf(buf, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
                    dp->d_name, d, dp->d_name, d, m_time, size);
            writen(out_fd, buf, strlen(buf));
        }
        close(ffd);
    }
    sprintf(buf, "</table></body></html>");
    writen(out_fd, buf, strlen(buf));
    closedir(d);
}

static const char* get_mime_type(char *filename){
    char *dot = strrchr(filename, '.');
    if(dot){
         // strrchar Locate last occurrence of character in string
        mime_map *map = meme_types;
        // loop through the mime_types array
        while(map->extension){
            // compare the extension with the filename
            if(strcmp(map->extension, dot) == 0){
                // return the mime type
                return map->mime_type;
            }
            map++;
        }
    }
    // default mime type
    return default_mime_type;
}

// open_listenfd - open and return a listening socket on port
int open_listenfd(int port){
    int listenfd, optval=1;
    struct sockaddr_in serveraddr;

    // Create a socket descriptor
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    // Eliminates "Address already in use" error from bind.
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int)) < 0)
        return -1;

    // 6 is TCP's protocol number
    // enable this, much faster : 4000 req/s -> 17000 req/s
    if (setsockopt(listenfd, 6, TCP_CORK, (const void *)&optval , sizeof(int)) < 0)
        return -1;

    // Listenfd will be an endpoint for all requests to port on any IP address for this host
    // server address configuration
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)port);

    // bind the socket to the address
    if (bind(listenfd, (SA *)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    // Make it a listening socket ready to accept connection requests
    if (listen(listenfd, LISTENQ) < 0)
        return -1;
    return listenfd;
}

// url_decode - decode the url
void url_decode(char* src, char* dest, int max) {
    char *p = src;
    char code[3] = { 0 };
    while(*p && --max) {
        if(*p == '%') {
            memcpy(code, ++p, 2);
            *dest++ = (char)strtoul(code, NULL, 16);
            p += 2;
        } else {
            *dest++ = *p++;
        }
    }
    *dest = '\0';
}

void parse_request(int fd, http_request *req){
    rio_t rio;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE];
    req->offset = 0;
    req->end = 0; // end of the request

    // initialize the read input buffer
    rio_readinitb(&rio, fd);
    // read the request line
    rio_readlineb(&rio, buf, MAXLINE);
    sscanf(buf, "%s %s", method, uri);
    /* read all */
    while(buf[0] != '\n' && buf[1] != '\n') { /* \n || \r\n */
        rio_readlineb(&rio, buf, MAXLINE);
        if(buf[0] == 'R' && buf[1] == 'a' && buf[2] == 'n'){
            sscanf(buf, "Range: bytes=%lu-%lu", &req->offset, &req->end);
            // Range: [start, end]
            if( req->end != 0) req->end ++;
        }
    }
    char* filename = uri;
    if(uri[0] == '/'){
        filename = uri + 1;
        int length = strlen(filename);
        if (length == 0){
            filename = ".";
        } else {
            for (int i = 0; i < length; ++ i) {
                if (filename[i] == '?') {
                    filename[i] = '\0';
                    break;
                }
            }
        }
    }
    url_decode(filename, req->filename, MAXLINE);
}

// log_access - log the access print ip, port, status, file
void log_access(int status, struct sockaddr_in *c_addr, http_request *req){
    printf("%s:%d %d - %s\n", inet_ntoa(c_addr->sin_addr), ntohs(c_addr->sin_port), status, req->filename);
}

// client_error - returns an error message to the client
void client_error(int fd, int status, char *msg, char *longmsg){
    char buf[MAXLINE];
    // construct the header and message
    sprintf(buf, "HTTP/1.1 %d %s\r\n", status, msg);
    sprintf(buf + strlen(buf), "Content-length: %lu\r\n\r\n", strlen(longmsg));
    sprintf(buf + strlen(buf), "%s", longmsg);
    // send the header and message
    writen(fd, buf, strlen(buf));
}

// serve_static - copy the file to the client
void serve_static(int out_fd, int in_fd, http_request *req, size_t total_size){
    char buf[256];
    if (req->offset > 0){
        // full file not sent
        sprintf(buf, "HTTP/1.1 206 Partial\r\n");
        sprintf(buf + strlen(buf), "Content-Range: bytes %lu-%lu/%lu\r\n", req->offset, req->end, total_size);
    } else {
        // full file sent
        sprintf(buf, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n");
    }
    // print buf length to header
    sprintf(buf + strlen(buf), "Cache-Control: no-cache\r\n");
    // print the content length to header
    sprintf(buf + strlen(buf), "Content-length: %lu\r\n",req->end - req->offset);
    // print the content type to header
    sprintf(buf + strlen(buf), "Content-type: %s\r\n\r\n",get_mime_type(req->filename));
    // send the header to client
    writen(out_fd, buf, strlen(buf));
    off_t offset = req->offset; /* copy */
    while(offset < req->end){
        if(sendfile(out_fd, in_fd, &offset, req->end - req->offset) <= 0) {
            break;
        }
        printf("offset: %d \n\n", offset);
        close(out_fd);
        break;
    }
}

void process(int fd, struct sockaddr_in *clientaddr){
    printf("accept request, fd is %d, pid is %d\n", fd, getpid());
    http_request req;
    parse_request(fd, &req);

    struct stat sbuf;
    int status = 200, ffd = open(req.filename, O_RDONLY, 0);
    if(ffd <= 0){
        status = 404;
        char *msg = "File not found";
        client_error(fd, status, "Not found", msg);
    } else {
        fstat(ffd, &sbuf);
        if(S_ISREG(sbuf.st_mode)){
            if (req.end == 0){
                req.end = sbuf.st_size;
            }
            if (req.offset > 0){
                status = 206;
            }
            serve_static(fd, ffd, &req, sbuf.st_size);
        } else if(S_ISDIR(sbuf.st_mode)){
            status = 200;
            handle_directory_request(fd, ffd, req.filename);
        } else {
            status = 400;
            char *msg = "Unknow Error";
            client_error(fd, status, "Error", msg);
        }
        close(ffd);
    }
    log_access(status, clientaddr, &req);
}

int main(int argc, char** argv){
    struct sockaddr_in clientaddr;
    int default_port = 9999,
        listenfd,
        connfd;
    char buf[256];
    char *path = getcwd(buf, 256);
    socklen_t clientlen = sizeof clientaddr;
    if(argc == 2) {
        if(argv[1][0] >= '0' && argv[1][0] <= '9') {
            default_port = atoi(argv[1]);
        } else {
            path = argv[1];
            if(chdir(argv[1]) != 0) {
                perror(argv[1]);
                exit(1);
            }
        }
    } else if (argc == 3) {
        default_port = atoi(argv[2]);
        path = argv[1];
        if(chdir(argv[1]) != 0) {
            perror(argv[1]);
            exit(1);
        }
    }

    listenfd = open_listenfd(default_port);
    if (listenfd > 0) {
        printf("listen on port %d, fd is %d\n", default_port, listenfd);
    } else {
        perror("ERROR");
        exit(listenfd);
    }
    // Ignore SIGPIPE signal, so if browser cancels the request, it
    // won't kill the whole process.
    signal(SIGPIPE, SIG_IGN);

    for(int i = 0; i < 10; i++) {
        int pid = fork();
        if (pid == 0) {         //  child
            while(1){
                connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
                process(connfd, &clientaddr);
                close(connfd);
            }
        } else if (pid > 0) {   //  parent
            printf("child pid is %d\n", pid);
        } else {
            perror("fork");
        }
    }

    while(1){
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        process(connfd, &clientaddr);
        close(connfd);
    }

    return 0;
}


