#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include "msg.h"

typedef struct node {
  unsigned long id;
  char *hostname;
  struct node *next;
  struct node *prev;
} Node;

static char *groupListFileName;
static char *logFileName;
static unsigned long timeoutValue;
static int isOngoingElection = 0;
static int isFailedElection = 0;
static unsigned long AYATime;
static clock_t startTime;
static unsigned int mins = 0;
static unsigned int s = 0;
static unsigned int ms = 0;
static unsigned int timeLeft = 0;
static clock_t currentTime;
static unsigned long countDownTimeInSeconds = 0;
static int sockfd;
static Node *nodes = NULL;
static Node myNode = (Node){0};
static Node coord = (Node){0};
static int myIndex;
static FILE *logFile;
static vectorClock nodeTimes[MAX_NODES];
static int isCoord = 0;

int receiveMessage(struct sockaddr_in*, int);
void election();
void resetTimer(int);
