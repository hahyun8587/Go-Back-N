#define _GNU_SOURCE

#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>

#define DDEBUG 1
#define SERV_PORT_NUM 2

#define WAIT_SEC 10

#define INIT 0
#define ESTABLISHED 1
#define CLOSE_WAIT 2

#define FAIL 0
#define SUCCESS 1

#define DATA_SIZE 500
#define MAX_SEQN 15

typedef struct {
    int seqn;
    int ackn;
    int fin;
    char data[DATA_SIZE + 1];
} packet;

typedef struct {
    int sockfd;
} socketinfo;

typedef struct {
    int state;
    int expectedseqn;
    socketinfo sinfo;
} gbninfo;

gbninfo info = { .state = INIT };

int DEBUG;

void initialize(gbninfo *info, int sockfd);
packet *make_pkt(gbninfo *info, int event);
void send_ack(gbninfo *info, packet *pkt, struct sockaddr *src_addr, socklen_t addrlen);
int rdt_recvfrom(int sockfd, void *buf, size_t len, 
                 int flags, struct sockaddr *dest_addr, socklen_t *addrlen);
void handler(int sig, siginfo_t *siginfo, void *ucontext);

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: ./server <DEBUG_OPTION> <SERV_PORT_NUM>\n");

        return 1;
    }

    DEBUG = atoi(argv[DDEBUG]);

    struct sockaddr_in serv_addr;
    struct sockaddr clnt_addr;
    int sockfd;
    socklen_t addrlen = sizeof(clnt_addr);

    sockfd = socket(PF_INET, SOCK_DGRAM, 0);

    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = PF_INET;
    serv_addr.sin_port = htons(atoi(argv[SERV_PORT_NUM]));
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) == -1) {
        fprintf(stderr, "bind error\n");
    }

    int fd;
    int len;
    char msg[DATA_SIZE + 1];
    
    rdt_recvfrom(sockfd, msg, sizeof(msg), 0, &clnt_addr, &addrlen);

    fd = open(msg, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR);

    while (rdt_recvfrom(sockfd, msg, sizeof(msg), 0, &clnt_addr, &addrlen)) {
        len = strlen(msg);
        
        write(fd, msg, len);
    }
    close(sockfd);
    close(fd);

    return 0;
}

void install(int signum, void (*handler)(int, siginfo_t *, void *), 
                 int mask) {
    struct sigaction act = { .sa_sigaction = handler, .sa_flags = SA_SIGINFO };
    
    sigemptyset(&act.sa_mask);
    
    if (mask) {
        sigaddset(&act.sa_mask, mask);
    }
    sigaction(signum, &act, NULL);
}

void handler(int sig, siginfo_t *siginfo, void *ucontext) {}

void initialize(gbninfo *info, int sockfd) {
    
    if (DEBUG) {
        printf("initializing\n");
    }

    info->expectedseqn = 0;
    info->sinfo.sockfd = sockfd;

    install(SIGIO, handler, 0);
    fcntl(info->sinfo.sockfd, F_SETFL, O_ASYNC);
    fcntl(info->sinfo.sockfd, F_SETSIG, SIGIO);
    fcntl(info->sinfo.sockfd, F_SETOWN, getpid());
    
    info->state = ESTABLISHED;
}

packet *make_pkt(gbninfo *info, int event) {
    packet *pkt;

    pkt = (packet *) malloc(sizeof(packet));

    memset(pkt, 0, sizeof(packet));

    pkt->ackn = event ? info->expectedseqn : (info->expectedseqn - 1 + MAX_SEQN + 1) % (MAX_SEQN + 1);
    
    return pkt;
}

void send_ack(gbninfo *info, packet *pkt, struct sockaddr *dest_addr, socklen_t addrlen) {
    
    if (DEBUG) {
        printf("sending ack\n");
    }

    sendto(info->sinfo.sockfd, pkt, sizeof(packet), 0, dest_addr, addrlen);
   
    if (pkt->ackn == info->expectedseqn) {    
        info->expectedseqn = (info->expectedseqn + 1) % (MAX_SEQN + 1);
    }
}

int rdt_recvfrom(int sockfd, void *buf, size_t len, int flags, 
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    struct sockaddr tmp_addr;
    socklen_t tmp_addrlen;
    packet pkt;
    int bytes;

    if (info.state == INIT) {
        initialize(&info, sockfd);
    }

    while (1) {
        bytes = recvfrom(sockfd, &pkt, sizeof(packet), 0, &tmp_addr, &tmp_addrlen);
        
        if (DEBUG) {
            printf("expectedseqn: %d\n", info.expectedseqn);
            printf("recieved seqn: %d\n", pkt.seqn); 
        }

        if (pkt.seqn != info.expectedseqn) {
            send_ack(&info, make_pkt(&info, FAIL), &tmp_addr, tmp_addrlen);
        } else {
            break;
        }
    }

    if (DEBUG) {
        printf("recieved data: %s\n", pkt.data);  
    } 

    send_ack(&info, make_pkt(&info, SUCCESS), &tmp_addr, tmp_addrlen);
    
    if (pkt.fin) {
        struct timespec ts = { WAIT_SEC, 0L };
        sigset_t set;

        info.state = CLOSE_WAIT;

        sigemptyset(&set);
        sigaddset(&set, SIGIO);

        while (sigtimedwait(&set, NULL, &ts) != -1) {
            send_ack(&info, make_pkt(&info, FAIL), &tmp_addr, tmp_addrlen);    
        }
    } else {
        memcpy(buf, &pkt.data, len);

        if (src_addr) {
            memcpy(src_addr, &tmp_addr, sizeof(struct sockaddr));
        }

        if (addrlen) {
            *addrlen = tmp_addrlen;
        }
    }
    return info.state == CLOSE_WAIT ? 0 : bytes;
}