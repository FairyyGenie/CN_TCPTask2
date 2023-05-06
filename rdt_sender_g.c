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
#include <limits.h>
#include <math.h>
#include "packet.h"
#include "common.h"

#define STDIN_FD 0
#define RETRY 120 // millisecond

int next_seqno = 0;
int send_base = 0;
int window_size = 10;
int eof = 0;
int retranx=0;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;
int firstByteInWindow;
int firstByteNotInWindow;
int packetBase;
int length;
char buffer[DATA_SIZE];
FILE *fp;
int acks[20000];
int bytesReceived;
int newPacketBase;
int temp = 0;
int ssthresh = INT_MAX;
float cwnd = MSS_SIZE;
int dupAckCount = 0;
float EstimatedRTT;
float DevRTT = 0;
int startTimes[20000];
float timeOutInterval;

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

int karn(int temp)
{
    float alpha = 0.125;
    float beta = 0.25;

    int sampleRTT = &timer.it_value - startTimes[temp % 20000];
    printf("current timer value: %d\n", &timer.it_value);
    printf("this is when we started the timer for this particular packet: ", startTimes[temp % 20000]);
    printf("sample RTT %d!\n", sampleRTT);

    EstimatedRTT = (1 - alpha) * EstimatedRTT + alpha * sampleRTT;
    printf("Estimated RTT %f!\n", EstimatedRTT);
    DevRTT = (1 - beta) * DevRTT + beta * fabs(sampleRTT - EstimatedRTT);
    printf("Dev RTT: %d\n", DevRTT);
    int timeoutInterval = EstimatedRTT + 4 * DevRTT;
    printf("Timeoutinterval %f!\n", timeOutInterval);

    return timeoutInterval;
}

void sendpacket(float cwnd)
{
    if (firstByteNotInWindow < firstByteInWindow)
    {
        firstByteNotInWindow = firstByteInWindow;
    }
    if(firstByteNotInWindow > firstByteInWindow+cwnd){
        firstByteNotInWindow = firstByteInWindow;
    }

    while (firstByteNotInWindow < firstByteInWindow + cwnd)
    {
        fseek(fp, firstByteNotInWindow, SEEK_SET);
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
        sndpkt->hdr.seqno = firstByteNotInWindow;
        memcpy(sndpkt->data, buffer, length);
        printf("transmission of packet %d done!\n", sndpkt->hdr.seqno);
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
        startTimes[sndpkt->hdr.seqno % 20000] = &timer.it_value;
        firstByteNotInWindow += length;
    }
}

void resendpacket(int temp)
{
    fseek(fp, temp, SEEK_SET);
    length = fread(buffer, 1, DATA_SIZE, fp);
    if (length <= 0)
    {
        VLOG(INFO, "End Of File has been reached");
        sndpkt = make_packet(0);
        sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
               (const struct sockaddr *)&serveraddr, serverlen);
        eof = 1;
    }
    sndpkt = make_packet(length);
    sndpkt->hdr.seqno = temp;
    memcpy(sndpkt->data, buffer, length);
    printf("Retransmission of packet %d done!\n", sndpkt->hdr.seqno);
    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
    {
        error("sendto");
    }
    startTimes[sndpkt->hdr.seqno % 20000] = &timer.it_value;
    acks[sndpkt->hdr.seqno % 20000] = 0;
}

int max(int x, int y)
{
    if (x > y)
    {
        return x;
    }
    else
    {
        return y;
    }
}

void ssTimeout(int sig)
{
    if (sig == SIGALRM)
    {
        if (eof == 1)
        {
            exit(0);
        }
        ssthresh = max(2 * MSS_SIZE, cwnd / 2);
        dupAckCount = 0;
        sendpacket(floor(cwnd));
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

int main(int argc, char **argv)
{

    char state[256] = "slow start";
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
    firstByteNotInWindow = firstByteInWindow;
    next_seqno = 0;
    // bytes[i] is the last byte that packet NUMBER i should contain.
    int bytes[20000];
    bzero(bytes, sizeof(bytes));

    init_timer(RETRY, ssTimeout);
    ssthresh = 128 * MSS_SIZE;

    while (1)
    {
        // IMPORTANT: ACKS may arrive out of order.
        if (strcmp(state, "slow start") == 0)
        {
            if (retranx==0){
                start_timer();
                sendpacket(floor(cwnd));
            }
            if (eof == 1)
            {
                VLOG(INFO, "End Of File has been reached");
                 exit(0);
            }
            // receive bytes from sender
            bytesReceived = recvfrom(sockfd, buffer, MSS_SIZE, 0,
                                     (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen);
            // if no bytes are received
            if (bytesReceived < 0)
            {
                error("recvfrom");
            }
            recvpkt = (tcp_packet *)buffer;
            temp = recvpkt->hdr.ackno;
            // if it's a new ACK
            if (acks[recvpkt->hdr.ackno % 20000] == 0)
            {
                retranx=0;
                printf("%s\n", "Going through ");
                timeOutInterval = karn(temp);
                if (cwnd < ssthresh / 2)
                {
                    printf("%s\n", "in SS we good and sending ");
                    cwnd += MSS_SIZE;
                    dupAckCount = 0;
                    if (recvpkt->hdr.ackno >= firstByteInWindow)
                    {
                        firstByteInWindow = recvpkt->hdr.ackno ;
                    }
                    char newstate[256] = "slow start";
                    strcpy(state, newstate);
                    stop_timer();
                    init_timer(timeOutInterval, ssTimeout);
                }
                else
                {
                    printf("%s\n", "in SS going into CA sending and transmiting");
                    char newstate[256] = "congestion avoidance";
                    strcpy(state, newstate);
                    if (recvpkt->hdr.ackno >= firstByteInWindow)
                    {
                        firstByteInWindow = recvpkt->hdr.ackno ;
                    }
                    // after congestion avoidance starts move to CA
                    cwnd += MSS_SIZE * MSS_SIZE / cwnd;
                    stop_timer();
                    init_timer(timeOutInterval, ssTimeout);
                }
            }
            acks[recvpkt->hdr.ackno % 20000]++;
            // dupAckCount++;
            // packet lost case in slow start
            if (acks[recvpkt->hdr.ackno % 20000] >= 3)
            {
                retranx=1;
                printf("in SS we recv dupack and retransmitting and going into SS and num %d\n", temp);
                char newstate[256] = "slow start";
                strcpy(state, newstate);
                // fast retransmit
                ssthresh = max(2 * MSS_SIZE, cwnd / 2);
                cwnd = 1 * MSS_SIZE;
                stop_timer();
                // retransmit missing segments
                init_timer(timeOutInterval, ssTimeout);
                start_timer();
                resendpacket(temp);
            }
        }
        // CA state
        else if (strcmp(state, "congestion avoidance") == 0)
        {
             if (retranx==0){
                start_timer();
                sendpacket(floor(cwnd));
            }
             if (eof == 1)
            {
                VLOG(INFO, "End Of File has been reached");
                 exit(0);
            }
            printf("first byte in the window: %d\n", firstByteInWindow);
            printf("first byte not in the window: %d\n", firstByteNotInWindow);
            printf("current cwnd size %f\n", cwnd);
            // receive bytes from sender
            bytesReceived = recvfrom(sockfd, buffer, MSS_SIZE, 0,
                                     (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen);
            // if no bytes are received
            if (bytesReceived < 0)
            {
                error("recvfrom");
            }
            recvpkt = (tcp_packet *)buffer;
            temp = recvpkt->hdr.ackno;
            printf("This is the ackno of our received packet: %d\n", temp);
            // if it's a new ACK
            if (acks[recvpkt->hdr.ackno % 20000] == 0)
            {
                timeOutInterval = karn(temp);
                retranx=0;
                if (recvpkt->hdr.ackno >= firstByteInWindow)
                {
                    firstByteInWindow = recvpkt->hdr.ackno;
                }
                char newstate[256] = "congestion avoidance";
                strcpy(state, newstate);
                // after congestion avoidance starts move to CA
                cwnd += MSS_SIZE / cwnd;
                printf("%s\n", "in CA we good sending and transmiting");
                stop_timer();
                init_timer(timeOutInterval, ssTimeout);
            }
            acks[recvpkt->hdr.ackno % 20000]++;
            // dupAckCount++;
            // packet lost case in CA
            if (acks[recvpkt->hdr.ackno % 20000] >= 3)
            {
                retranx=1;
                printf("in CA we recv dupack and retransmitting and going into SS and num %d\n", temp);
                char newstate[256] = "slow start";
                strcpy(state, newstate);
                // fast retransmit
                ssthresh = max(2 * MSS_SIZE, cwnd / 2);
                cwnd += MSS_SIZE / cwnd;
                // retransmit missing segments
                stop_timer();
                // retransmit missing segments
                init_timer(timeOutInterval, ssTimeout);
                start_timer();
                resendpacket(temp);
            }
        }
    }
}