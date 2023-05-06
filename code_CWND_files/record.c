#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <math.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>

// to compile this code, you need to link math library by using '-lm': gcc -o output code.c -lm


// struct timeval represents an elapsed time. https://pubs.opengroup.org/onlinepubs/7908799/xsh/systime.h.html
//In Linux, the current time is maintained by keeping the number of seconds elapsed since midnight of January 01, 1970 (called epoch)


FILE *csv;
float congestion_window_size = 2.5;
int ssthresh = 64;

struct timeval time_init; //for intial time of prgram.


float timedifference_msec(struct timeval, struct timeval);


int main(int argc, char **argv)
{
	gettimeofday(&time_init, 0); // noting starting time
	
	struct timeval t1; //for intermediate time vales

	csv = fopen("CWND.csv", "w");
	  if (csv == NULL)
	    {
		printf("Error opening csv\n");
		return 1;
	    }

	gettimeofday(&t1, 0);
    	fprintf(csv, "%f,%f,%d\n", timedifference_msec(time_init, t1), congestion_window_size, ssthresh);

	congestion_window_size++;

	gettimeofday(&t1, 0);
    	fprintf(csv, "%f,%f,%d\n", timedifference_msec(time_init, t1), congestion_window_size, ssthresh);

	congestion_window_size = 0;
	ssthresh = 2;
	gettimeofday(&t1, 0);
    	fprintf(csv, "%f,%f,%d\n", timedifference_msec(time_init, t1), congestion_window_size, ssthresh);

	fclose(csv);

}

float timedifference_msec(struct timeval t0, struct timeval t1)
{
    return fabs((t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f);
}