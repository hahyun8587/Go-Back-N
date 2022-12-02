#define _GNU_SOURCE

#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define DDEBUG 1
#define SERV_PORT_NUM 2
#define SERV_IP_ADDR 3
#define SLIDING_WIN_SIZE 4

#define SIGTO 34  
#define SIGCLS 35
#define SIGRCV 36
#define BILLION 1000000000
#define ALPHA 0.125
#define BETA 0.25
#define MAX_RTO 1 * BILLION
#define WAIT_SEC 10

#define INIT 0
#define ESTABLISHED 1
#define FIN 2

#define RECIEVE 0
#define TIMEOUT 1

#define DATA_SIZE 500
#define BUF_SIZE 300
#define MAX_PTR 1024
#define MAX_SEQN 15

#define INIT_RTO 1
int SWS;

typedef long long ll;

typedef struct {
    int seqn;
    int ackn;
    int fin;
    char data[DATA_SIZE + 1];
} packet;

typedef struct {
    packet **qa;
    int n;
    int head;
    int tail;
    char *name;
} Queue;

Queue *init_q(int size, char *name);
int empty(Queue *q);
int full(Queue *q);
int size(Queue *q);
int offset(Queue *q, int p); 
packet *push(Queue *q, packet *x);
packet *pop(Queue *q);
void free_q(Queue *q);
void qprint(Queue *q);

typedef struct {
    int sockfd;
    int flags;
    const struct sockaddr *dest_addr;
    socklen_t addrlen;
} sockinfo;

typedef struct {
    timer_t timerid;
    struct itimerspec rto;
    struct itimerspec srtt; 
    struct itimerspec rtt_var;
    int init;
} timerinfo;

typedef struct {
    int state;
    int sws;
    int base;
    int nextseqn;
    Queue *buf;
    Queue *window;
    sockinfo sinfo; 
    timerinfo tinfo;
} gbninfo;

typedef struct {
    packet *ptrs[MAX_PTR];
    int p;
} resource;

int DEBUG;

gbninfo info = { .state = INIT }; 
resource rsrc;

int set_rto(timerinfo *tinfo, int event);
void arm_timer(timerinfo *tinfo);
void send_pkts(gbninfo *info);
void timeout(int sig, siginfo_t *siginfo, void *ucontext);
void mv_pkts(gbninfo *info, int n);
void accept_pkt(gbninfo *info, packet *pkt);
void flush_buf(gbninfo *info);
void disarm_timer(timerinfo *tinfo);
void pkt_recv(int sig, siginfo_t *siginfo, void *ucontext);
void io_event(int sig, siginfo_t *siginfo, void *ucontext);
timer_t create_timer(int signum, void (*handler)(int, siginfo_t *, void *), 
                     int mask, void *val_ptr);
void install(int signum, void (*handler)(int, siginfo_t *, void *), int mask);
void initialize(gbninfo *info, int sockfd, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);
packet *make_pkt(gbninfo *info, const void *data, size_t len);
void refuse_pkt(gbninfo *info, packet *pkt);
int rdt_sendto(int sockfd, const void *buf, size_t len, int flags, 
               const struct sockaddr *dest_addr, socklen_t addrlen);
void disconnect(int sig, siginfo_t *siginfo, void *ucontext);
void free_rsrc();
int rdt_close(int sockfd); 

int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("usage: ./client <DEBUG_OPTION> <SERV_PORT_NUM> <SERV_IP_ADDR> <SLIDING_WIN_SIZE>\n");
        
        return 1;
    }
    DEBUG = atoi(argv[DDEBUG]);
    SWS = atoi(argv[SLIDING_WIN_SIZE]);

    struct sockaddr_in serv_addr;
    int sockfd;
    int fd;
    int len;
    char msg[DATA_SIZE + 1];
    char title[] = "test_file_short.txt"; 
    
    sockfd = socket(PF_INET, SOCK_DGRAM, 0);

    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(atoi(argv[SERV_PORT_NUM]));
    
    inet_aton(argv[SERV_IP_ADDR], &serv_addr.sin_addr);
    
    rdt_sendto(sockfd, title, sizeof(title), 
               0, (struct sockaddr *) &serv_addr, sizeof(serv_addr));    
    
    fd = open(title, O_RDONLY);

    while (len = read(fd, msg, DATA_SIZE)) {
        msg[len] = '\0';

        rdt_sendto(sockfd, msg, sizeof(msg), 
                   0, (struct sockaddr *) &serv_addr, sizeof(serv_addr));          
    }
    close(fd);
    rdt_close(sockfd);
    
    return 0;
}

int set_rto(timerinfo *tinfo, int event) {

    if (event != RECIEVE && event != TIMEOUT) {
        return -1;
    }

    if (DEBUG) {
        printf("setting rto\n", 12);
        printf("previous rto: [%ds %ldns]\n", 
               tinfo->rto.it_value.tv_sec, tinfo->rto.it_value.tv_nsec);
    }

    ll curr_rto;

    if (event == RECIEVE) { 
        struct itimerspec its;
        ll prev_rto;
        ll prev_srtt;
        ll curr_srtt;
        ll prev_rtt_var;
        ll curr_rtt_var;
        ll mrtt;

        prev_rto = tinfo->rto.it_value.tv_sec * BILLION 
                    + tinfo->rto.it_value.tv_nsec;
        prev_srtt = tinfo->srtt.it_value.tv_sec * BILLION 
                    + tinfo->srtt.it_value.tv_nsec;
        prev_rtt_var = tinfo->rtt_var.it_value.tv_sec * BILLION 
                       + tinfo->rtt_var.it_value.tv_nsec;
        
        timer_gettime(tinfo->timerid, &its);

        mrtt = prev_rto 
               - (its.it_value.tv_sec * BILLION + its.it_value.tv_nsec);
  
        curr_srtt = (ll) (tinfo->init ? mrtt 
                                      : (1 - ALPHA) * prev_srtt 
                                        + ALPHA * mrtt);
        curr_rtt_var = (ll) (tinfo->init ? mrtt / 2 
                                         : (1 - BETA) * prev_rtt_var + 
                                           BETA * abs(prev_srtt - mrtt));
        curr_rto = curr_srtt + 4 * curr_rtt_var;

        tinfo->rto.it_value.tv_sec = curr_srtt / BILLION;
        tinfo->rto.it_value.tv_nsec = curr_srtt % BILLION;
        tinfo->rtt_var.it_value.tv_sec = curr_rtt_var / BILLION;
        tinfo->rtt_var.it_value.tv_nsec = curr_rtt_var % BILLION;
    } else {
        curr_rto = 2 * (tinfo->rto.it_value.tv_sec * BILLION 
                        + tinfo->rto.it_value.tv_nsec);
    }

    if (curr_rto > MAX_RTO) {
        curr_rto = BILLION; 
    }
    tinfo->rto.it_value.tv_sec = curr_rto / BILLION;
    tinfo->rto.it_value.tv_nsec = curr_rto % BILLION; 

    if (tinfo->init) {
        tinfo->init = 0;
    }

    if (DEBUG) {
        printf("current rto: [%ds %ldns]\n", 
               tinfo->rto.it_value.tv_sec, tinfo->rto.it_value.tv_nsec);
    }
    
    return 0;
}

void arm_timer(timerinfo *tinfo) {
    
    if (DEBUG) {
        printf("arming the timer\n");
    }

    timer_settime(tinfo->timerid, 0, &tinfo->rto, NULL);
}

void send_pkts(gbninfo *info) {
    
    if (DEBUG) {
        printf("sending unacked packets...\n");
    }

    int p = info->window->head;

    while (!empty(info->window)) {
        sendto(info->sinfo.sockfd, pop(info->window), sizeof(packet), 
               0, info->sinfo.dest_addr, info->sinfo.addrlen); 
    }
    offset(info->window, p);
}

void timeout(int sig, siginfo_t *siginfo, void *ucontext) {

    if (DEBUG) {
        printf("\n----timeout----\n\n");
    }

    gbninfo *info = (gbninfo *) siginfo->si_value.sival_ptr;

    set_rto(&info->tinfo, TIMEOUT);
    arm_timer(&info->tinfo);
    send_pkts(info);
}

void mv_pkts(gbninfo *info, int n) {
    int i;

    for (i = 1; i <= n; i++) {
        rsrc.ptrs[rsrc.p++] = pop(info->window);
    }
}

void accept_pkt(gbninfo *info, packet *pkt) {

    if (DEBUG) {
        printf("accepting packet\n");
    }

    push(info->window, pkt);
    sendto(info->sinfo.sockfd, pkt, sizeof(packet), 
           info->sinfo.flags, info->sinfo.dest_addr, info->sinfo.addrlen);
    
    info->nextseqn = (info->nextseqn + 1) % (MAX_SEQN + 1);
}

int min(int a, int b) {
    return a <= b ? a : b;
}

void flush_buf(gbninfo *info) {
       
    if (DEBUG) {
        printf("flushing the buffer...\n");
    }
    
    int n;
    int i;

    n = min(SWS - size(info->window), size(info->buf));

    for (i = 1; i <= n; i++) {
        accept_pkt(info, pop(info->buf));
    }

    if (DEBUG) {
        printf("[base: %d nextseqn: %d]\n", info->base, info->nextseqn);
    }
}

void disarm_timer(timerinfo *tinfo) {
    
    if (DEBUG) {
        printf("disarming the timer\n");
    }

    struct itimerspec disarm = { { 0, 0L }, { 0, 0L } };

    timer_settime(tinfo->timerid, 0, &disarm, NULL);
}

void pkt_recv(int sig, siginfo_t *siginfo, void *ucontext) {

    if (DEBUG) {
        printf("\n----pkt_recv----\n\n"); 
    }

    gbninfo *info;
    packet pkt;
    int accept;
    int n;
    
    info = (gbninfo *) siginfo->si_value.sival_ptr;
    
    set_rto(&info->tinfo, RECIEVE);
    recvfrom(info->sinfo.sockfd, &pkt, sizeof(pkt), 0, NULL, NULL);
    
    if (DEBUG) {
        printf("[base: %d received ack: %d]\n", info->base, pkt.ackn);
    }

    accept = info->base + info->sws - 1 <= MAX_SEQN 
            ? pkt.ackn >= info->base && pkt.ackn <= info->base + info->sws - 1
            : pkt.ackn <= (info->base + info->sws - 1) % (MAX_SEQN + 1) 
              || pkt.ackn >= info->base;

    if (accept) {
        n = pkt.ackn >= info->base ? pkt.ackn - info->base + 1
                                   : MAX_SEQN + 1 - info->base + pkt.ackn + 1;
        mv_pkts(info, n);

        info->base = (pkt.ackn + 1) % (MAX_SEQN + 1);

        flush_buf(info);

        if (info->nextseqn == info->base) {
            disarm_timer(&info->tinfo);
        } else {
            arm_timer(&info->tinfo);
        }
    }
    
    if (DEBUG) {
        qprint(info->buf);
        qprint(info->window);
    }
}

void io_event(int sig, siginfo_t *siginfo, void *ucontext) {
    union sigval sv;

    sv.sival_ptr = &info;

    sigqueue(getpid(), SIGRCV, sv);
}

timer_t create_timer(int signum, void (*handler)(int, siginfo_t *, void *), 
                     int mask, void *val_ptr) {
    timer_t timerid;
    struct sigevent sev;

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = signum;
    sev.sigev_value.sival_ptr = val_ptr;

    install(signum, handler, mask);
    timer_create(CLOCK_REALTIME, &sev, &timerid);

    return timerid;
}

void install(int signum, void (*handler)(int, siginfo_t *, void *), 
                 int mask) {
    struct sigaction act = { .sa_sigaction = handler, .sa_flags = SA_SIGINFO };
    
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, signum);
    
    if (mask) {
        sigaddset(&act.sa_mask, mask);
    }
    sigaction(signum, &act, NULL);
}

void initialize(gbninfo *info, int sockfd, int flags, 
                const struct sockaddr *dest_addr, socklen_t addrlen) {

    if (DEBUG) {
        printf("initializing\n");
    }

    memset(info, 0, sizeof(gbninfo));
 
    info->sws = SWS;
    info->window = init_q(info->sws, "window");
    info->buf = init_q(BUF_SIZE, "buffer");
    info->sinfo.sockfd = sockfd;
    info->sinfo.flags = flags;
    info->sinfo.dest_addr = dest_addr;
    info->sinfo.addrlen = addrlen;
    info->tinfo.timerid = create_timer(SIGTO, timeout, SIGRCV, info);
    info->tinfo.rto.it_value.tv_sec = INIT_RTO;
    info->tinfo.init = 1;
    rsrc.p = 0;

    install(SIGRCV, pkt_recv, SIGTO);
    install(SIGCLS, disconnect, 0);
    install(SIGIO, io_event, 0);
    
    fcntl(info->sinfo.sockfd, F_SETOWN, getpid());
    fcntl(info->sinfo.sockfd, F_SETSIG, SIGIO);
    fcntl(info->sinfo.sockfd, F_SETFL, 
          fcntl(info->sinfo.sockfd, F_GETFL) | O_ASYNC);

    info->state = ESTABLISHED;
}

packet *make_pkt(gbninfo *info, const void *data, size_t len) {
    packet *pkt;
    
    pkt = (packet *) malloc(sizeof(packet));
    
    memset(pkt, 0, sizeof(packet));
    
    pkt->seqn = (info->nextseqn + size(info->buf)) % (MAX_SEQN + 1);

    if (info->state == FIN) {
        pkt->fin = 1;
    }

    if (data) {
        memcpy(pkt->data, data, len); 
    }
    return pkt;
}

void refuse_pkt(gbninfo *info, packet *pkt) {
    
    if (DEBUG) {
        printf("refusing packet\n");
    }
    
    push(info->buf, pkt);
}

int rdt_sendto(int sockfd, const void *buf, size_t len, int flags, 
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    if (DEBUG) {
        printf("\n----rdt_sendto----\n\n");
    }

    if (info.state == INIT) {
        initialize(&info, sockfd, flags, dest_addr, addrlen);
    } 
    
    if (info.nextseqn == info.base) {
        arm_timer(&info.tinfo);
    }

    flush_buf(&info);
    
    if (info.nextseqn == (info.base + info.sws) % (MAX_SEQN + 1)) {
        refuse_pkt(&info, make_pkt(&info, buf, len));
    }
    else {
        accept_pkt(&info, make_pkt(&info, buf, len));
    }

    if (DEBUG) {
        qprint(info.buf);
        qprint(info.window);
    }

    return min(len, DATA_SIZE);
}

void free_rsrc() {
    int i;

    for (i = 0; i < rsrc.p; i++) {
        free(rsrc.ptrs[i]);
    }
    free_q(info.window);
    free_q(info.buf);
}

void disconnect(int sig, siginfo_t *siginfo, void *ucontext) {
    
    if (DEBUG) {
        printf("\n----disconnect----\n");
    }
    
    gbninfo *info = (gbninfo *) siginfo->si_value.sival_ptr;

    info->state = FIN;

    rdt_sendto(info->sinfo.sockfd, NULL, 0, info->sinfo.flags, 
               info->sinfo.dest_addr, info->sinfo.addrlen);
    
    while (!empty(info->buf) || info->base != info->nextseqn);

    if (DEBUG) {
        printf("\n----disconnect ends----\n");
    }
}

int rdt_close(int sockfd) {
    union sigval sival;

    sival.sival_ptr = &info;

    sigqueue(getpid(), SIGCLS, sival);
    free_rsrc();
    close(sockfd);

    return 0;
}

Queue *init_q(int size, char *name) {
    Queue *q;
   
    q = (Queue *) malloc(sizeof(Queue));
    q->qa = (packet **) malloc(sizeof(packet *) * (size + 1));
    q->n = size + 1;
    q->head = 0;
    q->tail = 0;
    q->name = name;

    return q;
}

int empty(Queue *q) {
    return q->head == q->tail ? 1 : 0;
}

int full(Queue *q) {
    return (q->tail + 1) % q->n == q->head ? 1 : 0;
}

int size(Queue *q) {
    return q->head <= q->tail ? q->tail - q->head : q->n + q->tail - q->head;  
}

int offset(Queue *q, int p) {
    q->head = p;

    return p;
}

packet *push(Queue *q, packet *x) {
    if (full(q)) {
        return NULL;
    }
    q->qa[q->tail++] = x;
    q->tail %= q->n;

    return x;
}

packet *pop(Queue *q) {
    if (empty(q)) {
        return NULL;
    }
    packet *pkt = q->qa[q->head++];

    q->head %= q->n;
    
    return pkt;
}

void free_q(Queue *q) {
    free(q->qa);
    free(q);
}

void qprint(Queue *q) {
    sigset_t set;
    int p = q->head;

    sigemptyset(&set);
    sigaddset(&set, SIGTO);
    sigaddset(&set, SIGRCV);
    sigprocmask(SIG_BLOCK, &set, NULL);
    
    printf("%s: [ ", q->name);
    
    while (!empty(q)) {
        printf("%d ", pop(q)->seqn);
    }
    printf("]\n");
    offset(q, p);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}
