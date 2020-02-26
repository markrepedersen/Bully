#include "node.h"

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
static Node myNode = (Node){0};
static Node coord = (Node){0};
static int myIndex;
static FILE *logFile;
static vectorClock nodeTimes[MAX_NODES];
static int isCoord = 0;

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

  sockfd =
      socket(address->ai_family, address->ai_socktype, address->ai_protocol);
  if (sockfd < 0) {
    perror("Socket creation failure");
    exit(EXIT_FAILURE);
  }

  if (bind(sockfd, address->ai_addr, address->ai_addrlen) == -1) {
    perror("Bind failure");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in *socketAddress = (struct sockaddr_in *)address->ai_addr;
  printf("[SUCCESS] Socket bound to %s:%d\n",
         inet_ntoa(socketAddress->sin_addr), htons(socketAddress->sin_port));

  freeaddrinfo(address);
}

char *printMessageType(msgType type) {
  switch (type) {
  case ELECT:
    return "ELECT";
    break;
  case ANSWER:
    return "ANSWER";
    break;
  case AYA:
    return "AYA";
    break;
  case IAA:
    return "IAA";
    break;
  case COORD:
    return "COORD";
    break;
  default:
    return "Invalid message type.";
  }
}

void setSocketTimeout(int sockfd, unsigned long timeoutValue) {
  struct timeval tv;
  tv.tv_sec = timeoutValue;
  tv.tv_usec = 0;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
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

  *sockAddr = (struct sockaddr_in *)feed_server->ai_addr;

  return 0;
}

int sendMessage(message *msg, struct sockaddr_in *sockAddr) {
  int bytesSent =
      sendto(sockfd, msg, sizeof(*msg), 0, (struct sockaddr *)sockAddr,
             sizeof(struct sockaddr_in));
  printf("[%d] Sent %s (%d/%lu b) to '%s:%d'\n", msg->electionID,
         printMessageType(msg->msgID), bytesSent, sizeof(*msg),
         inet_ntoa(sockAddr->sin_addr), ntohs(sockAddr->sin_port));
  if (bytesSent != sizeof(*msg)) {
    perror("UDP send failed");
    return -1;
  }
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

void sendMessageWithType(Node *node, unsigned long electionId, msgType type) {
  message msg;
  struct sockaddr_in *sockAddr = NULL;

  getAddress(node, &sockAddr);
  createMessage(&msg, electionId, type);
  sendMessage(&msg, sockAddr);
}

void sendElectionAnswerMessage(Node *node, unsigned long electionId) {
  sendMessageWithType(node, electionId, ANSWER);
}

void sendElectMessage(Node *node, unsigned long electionId) {
  sendMessageWithType(node, electionId, ELECT);
}

void sendCoordinatorMessage(Node *node, unsigned long electionId) {
  sendMessageWithType(node, electionId, COORD);
}

// Return the number of answers received.
int receiveElectionAnswers() {
  struct sockaddr_in client;
  message msg;

  while (msg.msgID != ANSWER) {
    if (receiveMessage(&msg, &client) < 0) {
      // TODO: mark the node as dead if timeout occurred.
      // Temporarily mark this node as coordinator if no ANSWER messages
      // received.
      return 0;
    }
  }
  return 1;
}

// Return whether a COORD response was received or not.
int receiveCoordResponse() {
  struct sockaddr_in client;
  message msg;

  while (msg.msgID != COORD) {
    if (receiveMessage(&msg, &client) < 0) {
      return -1;
    }
  }

  coord.hostname = inet_ntoa(client.sin_addr);
  coord.id = htons(client.sin_port);

  return 0;
}

void sendElectToHigherOrderNodes(unsigned long electionId) {
  isCoord = 0; // New election -> reset coordinator status in case this node is
               // the old coordinator.
  coord = (Node){0};
  Node *currentNode = nodes;
  int answers = 0;
  while (currentNode != NULL) {
    if (currentNode->id > myNode.id) {
      sendElectMessage(currentNode, electionId);
      answers += receiveElectionAnswers();
    }
    currentNode = currentNode->next;
  }

  if (answers == 0) {
    // This node has become the new coordinator.
    isCoord = 1;
    Node *currentNode = nodes;
    while (currentNode != NULL) {
      if (currentNode->id != myNode.id) {
        sendCoordinatorMessage(currentNode, electionId);
      }
      currentNode = currentNode->next;
    }
  } else {
    message response;
    // Wait for COORD message to determine the new coordinator.
    receiveCoordResponse();
  }
}

int receiveMessage(message *message, struct sockaddr_in *client) {
  socklen_t len = sizeof(*client);
  int n = recvfrom(sockfd, message, sizeof(*message), MSG_WAITALL,
                   (struct sockaddr *)client, &len);

  if (n < 0) {
    // Timeout occurred.
    if (errno == EAGAIN || n == EWOULDBLOCK) {
      perror("Timeout occurred.");
      return -1;
    } else {
      perror("Receiving error");
      return -2;
    }
  }

  printf("[%d] Received %s from %s:%d\n", message->electionID,
         printMessageType(message->msgID), inet_ntoa(client->sin_addr),
         ntohs(client->sin_port));

  // Always respond to ELECT messages.
  if (message->msgID == ELECT) {
    Node node;
    node.hostname = inet_ntoa(client->sin_addr);
    node.id = htons(client->sin_port);
    sendElectionAnswerMessage(&node, node.id);
    sendElectToHigherOrderNodes(message->electionID);
  }

  return 0;
}

void coordinate() {
  message response;
  struct sockaddr_in client;

  // Wait for any AYA messages.
  int result = receiveMessage(&response, &client);

  if (result >= 0) {
    // If received an AYA, send back an IAA.
    if (response.msgID == AYA) {
      Node node;
      node.hostname = inet_ntoa(client.sin_addr);
      node.id = htons(client.sin_port);
      sendMessageWithType(&node, node.id, IAA);
    }
  }
}

void election() {
  unsigned long electionId = (unsigned long)rand();
  sendElectToHigherOrderNodes(electionId);
}

int sendAYA(Node *node, struct sockaddr_in *sockAddr) {
  message msg;
  struct sockaddr_in *client = NULL;

  sendMessageWithType(node, node->id, AYA);

  while (msg.msgID != IAA) {
    if (receiveMessage(&msg, client) == -1) {
      return -1;
    }
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
  setSocketTimeout(sockfd, timeoutValue);

  // Initialize log file
  logFile = fopen(logFileName, "w+");
  if (logFile == NULL) {
    perror("Log file error: ");
    exit(EXIT_FAILURE);
  }
  logEvent("Starting node");

  struct sockaddr_in *sockAddr = NULL;
  message response;

  while (1) {
    if (!isCoord) {
      printf("Coordinator? %s\n", coord.hostname);
      if (coord.id != 0)
      // The coordinator is known.
      {
        getAddress(&coord, &sockAddr);
      }

      if (sockAddr == NULL || sendAYA(&coord, sockAddr) < 0) {
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
