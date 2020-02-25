#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "msg.h"

#ifdef __APPLE__
#define MSG_CONFIRM 0
#endif

// The purpose of this file is to provide insight into how to make various
// library calls to perfom some of the key functions required by the
// application. You are free to completely ignore anything in this file and do
// things whatever way you want provided it conforms with the assignment
// specifications. Note: this file compiles and works on the deparment servers
// running Linux. If you work in a different environment your mileage may vary.
// Remember that whatever you do the final program must run on the department
// Linux machines.

typedef struct node {
  unsigned long id;
  char *hostname;
  struct node *next;
  struct node *prev;
} Node;

typedef struct coordinator {
  unsigned long port;
  char *hostname;
} Coordinator;

static int sockfd;
static Node *nodes = NULL;
static Node myNode = {-1};
static Node coord = {-1};
static int myIndex;
static FILE *logFile;
static vectorClock nodeTimes[MAX_NODES];
static int isCoord = 1;

void logEvent(char *description) {
  ++(nodeTimes[myIndex].time);
  fprintf(logFile, "%s\n", description);
  fprintf(logFile, "N%d {", myIndex + 1);
  int first = 1;
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodeTimes[i].time != 0) {
      if (!first)
        fprintf(logFile, ", ");
      fprintf(logFile, "\"N%d\": %u", i + 1, nodeTimes[i].time);
      first = 0;
    }
  }
  fprintf(logFile, "}\n");
  fflush(logFile);
}

void usage(char *cmd) {
  printf("usage: %s  portNum groupFileList logFile timeoutValue averageAYATime "
         "failureProbability \n",
         cmd);
}

int validatePort(Node *nodes, unsigned long port) {
  // Verify that the port number passed to this program is in the list of nodes;
  Node *curr = nodes;
  int hasPort = -1;
  int idx = 0;
  while (curr != NULL) {
    if (port == curr->id) {
      hasPort = 0;
      myIndex = idx;
    }
    curr = curr->next;
    ++idx;
  }
  return hasPort;
}

Node *readGroupListFile(char *fileName) {
  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  Node *prev = NULL, *head = NULL;

  fp = fopen(fileName, "r");
  if (fp == NULL) {
    perror("Group list exit failure: ");
    exit(EXIT_FAILURE);
  }

  while ((read = getline(&line, &len, fp)) != -1) {
    char *hostname = strtok(line, " ");
    if (strcmp(hostname, line) != 0)
      break;

    char *id = strtok(NULL, " ");
    if (strtok(NULL, " ") != NULL)
      break;

    unsigned long nodeId = strtoul(id, NULL, 0);
    if (nodeId == 0)
      break;

    Node *curr = malloc(sizeof(Node));
    curr->id = nodeId;
    curr->hostname = strdup(hostname);
    curr->prev = prev;
    curr->next = NULL;

    if (prev)
      prev->next = curr;
    else
      head = curr;
    prev = curr;
    curr = curr->next;
  }

  return head;
}

void initServer() {
  char portBuf[10];
  snprintf(portBuf, 10, "%lu", myNode.id);
  const char *hostname = myNode.hostname;
  struct addrinfo hints, *address;

  memset(&hints, 0, sizeof(hints));

  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  int err = getaddrinfo(hostname, portBuf, &hints, &address);
  if (err != 0) {
    perror("Invalid address: ");
    exit(EXIT_FAILURE);
  }

  sockfd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
  if (sockfd == -1) {
    perror("Socket creation failure");
    exit(EXIT_FAILURE);
  }

  if (bind(sockfd, address->ai_addr, address->ai_addrlen) == -1) {
    perror("Bind failure");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in *socketAddress = (struct sockaddr_in*) address->ai_addr;
  printf("[SUCCESS] Socket bound to %s:%d\n", inet_ntoa(socketAddress->sin_addr), htons(socketAddress->sin_port));

  freeaddrinfo(address);
}

void setSocketTimeout(int sockfd, unsigned long timeoutValue) {
  struct timeval tv;
  tv.tv_sec = timeoutValue;
  tv.tv_usec = 0;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
}

int receiveMessage(message *message, struct sockaddr_in *client) {
  socklen_t len = sizeof(*client);

  int n = recvfrom(sockfd, message, sizeof(*message), MSG_WAITALL,
                   (struct sockaddr *)client, &len);

  // Timeout occurred.
  if (errno == EAGAIN || n == EWOULDBLOCK) {
    perror("Timeout occurred.");
    return -1;
  }

  if (n < 0) {
    perror("Receiving error");
    return -3;
  }

  printf("Data: %du -> Received from %s:%d\n\n", message->electionID,
         inet_ntoa(client->sin_addr), ntohs(client->sin_port));

  return 0;
}

void getRandomNumber(unsigned long AYATime) {
  int i;
  for (i = 0; i < 10; i++) {
    int rn;
    rn = random();

    // scale to number between 0 and the 2*AYA time so that
    // the average value for the timeout is AYA time.

    int sc = rn % (2 * AYATime);
    printf("Random number %d is: %d\n", i, sc);
  }
}

int getAddress(Node *node, struct sockaddr_in **sockAddr) {
  char portBuf[10];
  struct addrinfo hints, *res, *feed_server;

  snprintf(portBuf, 10, "%lu", node->id);
  memset(&hints, 0, sizeof(hints));

  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_family = AF_INET;
  hints.ai_protocol = IPPROTO_UDP;

  if (getaddrinfo(node->hostname, portBuf, &hints, &feed_server)) {
    perror("Couldn't lookup hostname\n");
    exit(EXIT_FAILURE);
  }

  for (res = feed_server; res != NULL; res = res->ai_next) {
    *sockAddr = (struct sockaddr_in *)res->ai_addr;
  }
  return 0;
}

int sendMessage(message *msg, struct sockaddr_in *sockAddr) {
  int bytesSent =
      sendto(sockfd, msg, sizeof(*msg), MSG_CONFIRM,
             (struct sockaddr *)sockAddr, sizeof(struct sockaddr_in));
  if (bytesSent != sizeof(*msg)) {
    perror("UDP send failed");
    return -1;
  }
  printf("[%d] Sent message (%d/%lu b) to '%s:%d'\n",
         msg->electionID, bytesSent, sizeof(*msg),
         inet_ntoa(sockAddr->sin_addr), ntohs(sockAddr->sin_port));
  return bytesSent;
}

// Initialize the node's vector clock to match the list of nodes given.
// The time for each node is initially 0.
// The times will change once messages start being received.
void initVectorClock(vectorClock *nodeTimes, int numClocks, Node *node) {
  Node *currentNode = node;
  for (int i = 0; i < numClocks; i++) {
    vectorClock clock = nodeTimes[i];
    if (currentNode == NULL) {
      perror("Number of nodes does not match number of clocks.");
      exit(EXIT_FAILURE);
    }
    clock.nodeId = node->id;
    clock.time = 0;
    currentNode = node->next;
  }
}

void createMessage(message *msg, unsigned long electionId, msgType type) {
  msg->electionID = electionId;
  msg->msgID = type;
  memcpy(msg->vectorClock, nodeTimes, sizeof(nodeTimes));
}

void coordinate() {
  Node node;
  message response;
  struct sockaddr_in *client = NULL;

  receiveMessage(&response, client);

  getAddress(&node, &client);
  createMessage(&response, response.electionID, IAA);
  sendMessage(&response, client);
}

void sendAYA(Node *node, struct sockaddr_in *sockAddr) {
  message msg;
  createMessage(&msg, node->id, AYA);
  sendMessage(&msg, sockAddr);
  printf("Sent AYA [%ul, %ul] to %s\n", msg.electionID, msg.msgID,
         node->hostname);
}

// coordinatorId will be set to the new coordinator's ID at the end.
void election() {
  isCoord = 0;
  unsigned long electionId = (unsigned long)rand();
  Node *currentNode = nodes;

  // Send message to all nodes that have an ID > than the current node's ID.
  while (currentNode != NULL) {
    if (currentNode->id > myNode.id) {
      message msg;
      struct sockaddr_in *sockAddr = NULL;
      getAddress(currentNode, &sockAddr);
      createMessage(&msg, electionId, ELECT);
      sendMessage(&msg, sockAddr);
    }
    currentNode = currentNode->next;
  }

  message response;
  struct sockaddr_in client;

  // Wait for at least one ANSWER message (with the same election ID) to
  // determine if this node should be the coordinator or not.
  while (response.electionID != electionId) {
    if (receiveMessage(&response, &client) > 0) {
      // This node received an ANSWER message -> it is NOT the coordinator.
      if (response.electionID == electionId) {
        return;
      }
    } else {
      // This node is the coordinator -> send out COORD messages to all nodes.
      // TODO: Revert isCoord to 0 when it isn't coord anymore.
      isCoord = 1;
      Node *currentNode = nodes;
      while (currentNode != NULL) {
        message coordMsg;
        createMessage(&coordMsg, electionId, COORD);
        currentNode = currentNode->next;
      }
    }
  }
  // This should never happen.
  perror("Why this happen...");
}

int sendAYAAndRespond(Node *node, struct sockaddr_in *sockAddr) {
  message msg;
  struct sockaddr_in *client = NULL;

  // Send an initial AYA message to coordinator to start off the process.
  sendAYA(node, sockAddr);

  // Now wait for an IAA message.
  if (receiveMessage((void *)&msg, client) == -1) {
    // IAA not received before <timeout> seconds -> call election.
    printf("Failed to receive IAA.\n");
    return -1;
  } else {
    // IAA message was received.
    printf("Received IAA [%ul, %ul].\n", msg.electionID, msg.msgID);
    sleep(1000);
  }
  return 0;
}

int main(int argc, char **argv) {
  // If you want to produce a repeatable sequence of "random" numbers
  // replace the call to  time() with an integer.
  srandom(time(0));

  char *groupListFileName;
  char *logFileName;
  unsigned long timeoutValue;
  unsigned long AYATime;
  unsigned long myClock = 1;
  unsigned long sendFailureProbability;
  if (argc != 7) {
    usage(argv[0]);
    return -1;
  }

  // Some code illustrating how to parse command line arguments.
  // This cod will probably have to be changed to match how you
  // decide to do things.

  char *end;
  int err = 0;

  myNode.id = strtoul(argv[1], &end, 10);
  if (argv[1] == end) {
    printf("Port conversion error\n");
    err++;
  }

  groupListFileName = argv[2];
  logFileName = argv[3];

  timeoutValue = strtoul(argv[4], &end, 10);
  if (argv[4] == end) {
    printf("Timeout value conversion error\n");
    err++;
  }

  AYATime = strtoul(argv[5], &end, 10);
  if (argv[5] == end) {
    printf("AYATime conversion error\n");
    err++;
  }

  sendFailureProbability = strtoul(argv[6], &end, 10);
  if (argv[5] == end) {
    printf("sendFailureProbability conversion error\n");
    err++;
  }

  printf("Port number:              %lu\n", myNode.id);
  printf("Group list file name:     %s\n", groupListFileName);
  printf("Log file name:            %s\n", logFileName);
  printf("Timeout value:            %lu\n", timeoutValue);
  printf("AYATime:                  %lu\n", AYATime);
  printf("Send failure probability: %lu\n", sendFailureProbability);

  if (err) {
    printf("%d conversion error%sencountered, program exiting.\n", err,
           err > 1 ? "s were " : " was ");
    return -1;
  }

  if (strcmp(groupListFileName, "-") == 0) {
    // Group list will be specified by stdin.
    printf("Group list specified by command line.\n");
  } else {
    // Process group list file
    nodes = readGroupListFile(groupListFileName);
  }

  int isValidPort = validatePort(nodes, myNode.id);
  if (isValidPort == -1) {
    printf("Invalid port: %lu", myNode.id);
    exit(EXIT_FAILURE);
  }

  Node *currentNode = nodes;
  while (currentNode != NULL) {
    if (currentNode->id == myNode.id) {
      myNode.hostname = currentNode->hostname;
    }
    currentNode = currentNode->next;
  }

  initVectorClock(nodeTimes, MAX_NODES, nodes);
  initServer();

  if (sockfd < 0) {
    perror("Invalid socket binding: ");
    exit(EXIT_FAILURE);
  }

  setSocketTimeout(sockfd, timeoutValue);

  struct sockaddr_in *sockAddr = NULL;

  message response;

  // Initialize log file
  logFile = fopen(logFileName, "w+");
  if (logFile == NULL) {
    perror("Log file error: ");
    exit(EXIT_FAILURE);
  }
  logEvent("Starting node");

  while (1) {
    if (isCoord != 0) {
      if (coord.id != -1)
      // The coordinator is known.
      {
        getAddress(&coord, &sockAddr);
      }

      if (sockAddr == NULL || sendAYAAndRespond(&coord, sockAddr) < 0) {
        // Either AYA timed out or coordinator is not known yet -> call
        // election.
        election();
      }
    } else {
      coordinate();
    }
  }

  fclose(logFile);
}
