#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "packet.h"
#include "common.h"

#define STDIN_FD 0
#define RETRY 120 // millisecond

int next_seqno = 0;
int send_base = 0;
int window_size = 10;
int eof = 0;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;
int firstByteInWindow;
int lastByteInWindow;
int packetBase;
int length;
char buffer[DATA_SIZE];
FILE *fp;
int acks[20000];
int bytesReceived;
int newPacketBase;
int temp = 0;
int ssthresh = INT_MAX;
int cwnd = MSS_SIZE;
int dupAckCount = 0;

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

void ssTimeout(int sig)
{
    if (sig == SIGALRM)
    {
        if (eof == 1)
        {
            exit(0);
        }
        ssthresh = max(2*MSS_SIZE, cwnd/2);
        dupAckCount = 0;
        //retransmit missing segment
        start_timer();
    }
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int))
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000; // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;
    timer.it_value.tv_sec = delay / 1000; // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

void sendpacket(int cwnd){
    while(lastByteInWindow - firstByteInWindow < cwnd){
                length = fread(buffer, 1, DATA_SIZE, fp);
                if (length <= 0)
                {
                    VLOG(INFO, "End Of File has been reached");
                    sndpkt = make_packet(0);
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                           (const struct sockaddr *)&serveraddr, serverlen);
                    eof = 1;
                    break;
                }
                sndpkt = make_packet(length);
                sndpkt->hdr.seqno = temp;
                memcpy(sndpkt->data, buffer, length);
                printf("Retransmission of packet %d done!\n", sndpkt->hdr.seqno);
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }
                lastByteInWindow+=length;
    }
}


int main(){

    char state[256] = "slow start";
    state = 
    int portno;
    // int next_seqno;
    char *hostname;

    for (int i = 0; i < 20000; i++)
    {
        acks[i] = 0;
    }
    /* check command line arguments */
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL)
    {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0)
    {
        fprintf(stderr, "ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);
    firstByteInWindow = 0;
    lastByteInWindow = firstByteInWindow;
    next_seqno = 0;
    // bytes[i] is the last byte that packet NUMBER i should contain.
    int bytes[20000];
    bzero(bytes, sizeof(bytes));
    int length;

    init_timer(RETRY, resend_packets);

    while(1){
        if(strcmp(state, "slow start") == 0){
            sendpacket(cwnd);
            //receive bytes from sender
            bytesReceived = recvfrom(sockfd, buffer, MSS_SIZE, 0,
                      (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen);
            //if no bytes are received
            if (bytesReceived < 0){
                error("recvfrom");
            }
            recvpkt = (tcp_packet *)buffer;
            //if it's a new ACK
            if (acks[recvpkt->hdr.ackno%20000] == 0){
                if(cwnd < ssthresh){
                    cwnd+=MSS_SIZE;
                    dupAckCount = 0;
                    firstByteInWindow = recvpkt->hdr.ackno+1;
                    sendpacket(cwnd);
                    start_timer();
                }
                else{
                    state = "congestion avoidance";
                    cwnd+=MSS_SIZE * MSS_SIZE/cwnd;
                    sendpacket(cwnd);
                    start_timer();
                }
            }
            acks[recvpkt->hdr.ackno%20000]++;
            dupAckCount++;
            if (acks[recvpkt->hdr.ackno%20000] >= 3){
                state = "fast recovery";
                ssthresh = max(2*MSS_SIZE, cwnd/2);
                start_timer();
                //retransmit missing segments
                sendpacket(cwnd);
            }
        }
        else if(strcmp(state, "congestion avoidance") == 0){

        }
        else if(strcmp(state, "fast recovery") == 0){

        }

    }
}