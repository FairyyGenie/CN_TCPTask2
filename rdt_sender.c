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
int lastByteinWindow;
int packetBase;
int length;
char buffer[DATA_SIZE];
FILE *fp;
int acks[20000];
int bytesReceived;
int newPacketBase;
int temp;

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        // Resend all packets range between
        // sendBase and nextSeqNum
        fseek(fp, firstByteInWindow,SEEK_SET);
        length = fread(buffer, 1, DATA_SIZE, fp);
        sndpkt = make_packet(length);
        VLOG(INFO, "Timeout happened, retransmission");
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,
                   (const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
        // open the file if it's not already open, go back to the sendbase, retransmit all bytes from there.
    }
    stop_timer();
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

    init_timer(RETRY, resend_packets);
    next_seqno = 0;
    int windowCreated = 0;
    // bytes[i] is the last byte that packet NUMBER i should contain.
    int bytes[20000];
    bzero(bytes, sizeof(bytes));
    int length;

    while (1)
    {
        if (eof == 1)
        {
            VLOG(INFO, "End Of File has been reached");
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                   (const struct sockaddr *)&serveraddr, serverlen);
            break;
        }
        // first ten window creation
        if (!windowCreated)
        {
            for (int i = 0; i < 10; i++)
            {
                if (i == 0)
                {
                    packetBase = 0;
                    bytes[i] = 0;
                    start_timer();
                }
                length = fread(buffer, 1, DATA_SIZE, fp);
                bytes[i + 1] = bytes[i] + length;
                if (length <= 0)
                {
                    VLOG(INFO, "End Of File has been reached");
                    sndpkt = make_packet(0);
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                           (const struct sockaddr *)&serveraddr, serverlen);
                    eof = 1;
                    exit(0);
                }
                sndpkt = make_packet(length);
                sndpkt->hdr.seqno = bytes[i];
                memcpy(sndpkt->data, buffer, length);
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }
                printf("sending seq no: %d\n", sndpkt->hdr.seqno);
            }
            // after first ten out of loop
            windowCreated = 1;
        }
        // packet base for first ten
        printf("packet base: %d\n", packetBase);
        lastByteinWindow = bytes[packetBase + window_size];
        firstByteInWindow = bytes[packetBase];

        // receiving ack from receiver
        do
        {
            bytesReceived = recvfrom(sockfd, buffer, MSS_SIZE, 0,
                                     (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen);
            // if not received
            if (bytesReceived < 0)
            {
                error("recvfrom");
            }
            recvpkt = (tcp_packet *)buffer;
            // print the ack number
            printf("recvpkt ack no: %d\n", recvpkt->hdr.ackno);
            // move the packetbase once ack knowledged
            packetBase = recvpkt->hdr.ackno / DATA_SIZE;
            lastByteinWindow = bytes[packetBase + window_size];
            firstByteInWindow = bytes[packetBase];
            // ignore smaller ack
            if (recvpkt->hdr.ackno < firstByteInWindow)
            {
                printf("ignore!!!\n");
            }
            // receive cumulaative ack for the first byte in window
            stop_timer();
            // ack counting for the packet
            acks[recvpkt->hdr.ackno % 20000] = acks[recvpkt->hdr.ackno % 20000] + 1;
            // if receving duplicate ack
            if (acks[recvpkt->hdr.ackno % 20000] >= 3)
            {
                printf("Received DUPLICATE ACK: %d\n", recvpkt->hdr.ackno);
                temp = recvpkt->hdr.ackno;
                fseek(fp, temp, SEEK_SET);
                length = fread(buffer, 1, DATA_SIZE, fp);
                sndpkt = make_packet(length);
                sndpkt->hdr.seqno = temp;
                memcpy(sndpkt->data, buffer, length);
                printf("Retransmission of packet %d done!\n", sndpkt->hdr.seqno);
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }
                acks[recvpkt->hdr.ackno % 20000] = 0;
            }

            // moving window if correctly acknoledged
            for (int a = packetBase; a < packetBase + window_size; a++)
            {
                if (a == firstByteInWindow)
                {
                    start_timer();
                    bytes[a] = firstByteInWindow;
                }
                fseek(fp, bytes[a],SEEK_SET);
                length = fread(buffer, 1, DATA_SIZE, fp);
                bytes[a + 1] = bytes[a] + length;
                if (length <= 0)
                {
                    VLOG(INFO, "End Of File has been reached");
                    sndpkt = make_packet(0);
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                           (const struct sockaddr *)&serveraddr, serverlen);
                    eof = 1;
                    exit(0);
                }
                // sending packet
                sndpkt = make_packet(length);
                sndpkt->hdr.seqno = bytes[a];
                // moving window using array
                printf("packet base: %d\n", packetBase);
                printf("sending seq no HERES: %d\n", sndpkt->hdr.seqno);
                memcpy(sndpkt->data, buffer, length);
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }
                free(sndpkt);
            }

        } while (recvpkt->hdr.ackno <= lastByteinWindow);
        // stop_timer();
    }
    return 0;
}