#include "node.h"
#include "limits.h"

int cmpClock(vectorClock *a, vectorClock *b) {
  const int aPort = a->nodeId == 0 ? USHRT_MAX + 1 : a->nodeId;
  const int bPort = b->nodeId == 0 ? USHRT_MAX + 1 : b->nodeId;
  return aPort - bPort;
}

void mergeClocks(vectorClock *other) {
  qsort(other, MAX_NODES, sizeof(*other), cmpClock);
  for (int i = 0; i < MAX_NODES; ++i) {
    if (i == myIndex) {
      if (other[i].time > nodeTimes[i].time) {
        printf("Received message where local time is greater than current.\n");
      }
    } else {
      nodeTimes[i].time =
          other[i].time > nodeTimes[i].time ? other[i].time : nodeTimes[i].time;
    }
  }
}

void logEvent(char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ++(nodeTimes[myIndex].time);
  vfprintf(logFile, fmt, args);
  fprintf(logFile, "\n");
  fprintf(logFile, "N%d {", nodeTimes[myIndex].nodeId);
  int first = 1;
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodeTimes[i].time != 0) {
      if (!first)
        fprintf(logFile, ", ");
      fprintf(logFile, "\"N%d\": %u", nodeTimes[i].nodeId, nodeTimes[i].time);
      first = 0;
    }
  }
  fprintf(logFile, "}\n");
  fflush(logFile);
  va_end(args);
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

  fp = fopen(fileName, "r");
  if (fp == NULL) {
    perror("Group list exit failure: ");
    exit(EXIT_FAILURE);
  }

  return setNodes(fp);
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

unsigned long getRandomNumber() {
  int i;
  int rn;
  rn = random();
  // in ms
  return rn % (2 * AYATime);
}

struct addrinfo *getAddress(Node *node, struct sockaddr_in **sockAddr) {
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

  return feed_server;
}

// Determines if a message should be sent or not (by randomly choosing a
// number).
int isMessageSent() {
  int p = rand() % 100;
  if (sendFailureProbability == 0) {
    return 1;
  } else if (p < sendFailureProbability) {
    printf("Packet was dropped due to send failure probability = %d\n", p);
    return 0;
  } else
    return 1;
}

int sendMessage(message *msg, struct sockaddr_in *sockAddr) {
  int bytesSent = -1;
  if (isMessageSent()) {
    bytesSent = sendto(sockfd, msg, sizeof(*msg), 0,
                       (struct sockaddr *)sockAddr, sizeof(struct sockaddr_in));

    char *mType = printMessageType(ntohl(msg->msgID));
    uint16_t targetPort = ntohs(sockAddr->sin_port);
    printf("[%u] Sent %s (%d/%lu b) to '%s:%d'\n", ntohl(msg->electionID),
           mType, bytesSent, sizeof(*msg), inet_ntoa(sockAddr->sin_addr),
           targetPort);
    if (bytesSent != sizeof(*msg)) {
      perror("UDP send failed");
      return -1;
    }
  }
  return bytesSent;
}

// Initialize the node's vector clock to match the list of nodes given.
// The time for each node is initially 0.
// The times will change once messages start being received.
void initVectorClock(vectorClock *nodeTimes, int numClocks, Node *node) {
  for (int i = 0; i < numClocks; i++) {
    if (node == NULL) {
      nodeTimes[i].nodeId = 0;
      nodeTimes[i].time = 0;
    } else {
      nodeTimes[i].nodeId = node->id;
      nodeTimes[i].time = 0;
      node = node->next;
    }
  }
  qsort(nodeTimes, MAX_NODES, sizeof(*nodeTimes), cmpClock);
}

void createMessage(message *msg, unsigned long electionId, msgType type) {
  msg->electionID = electionId;
  msg->msgID = type;
  memcpy(msg->vectorClock, nodeTimes, sizeof(nodeTimes));
  // Convert to network order.
  msg->electionID = htonl(msg->electionID);
  msg->msgID = htonl(msg->msgID);
  for (int i = 0; i < MAX_NODES; ++i) {
    msg->vectorClock[i].nodeId = htonl(msg->vectorClock[i].nodeId);
    msg->vectorClock[i].time = htonl(msg->vectorClock[i].time);
  }
}

void sendMessageWithType(Node *node, unsigned long electionId, msgType type) {
  message msg;
  struct sockaddr_in *sockAddr = NULL;

  struct addrinfo *addrInfo = getAddress(node, &sockAddr);
  logEvent("Sent %s to %u", printMessageType(type), node->id);
  createMessage(&msg, electionId, type);
  sendMessage(&msg, sockAddr);
  freeaddrinfo(addrInfo);
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
  while (1) {
    int response = receiveMessage(&client, 0);
    if (response < 0) {
      return 0;
    }
    if (response == ANSWER) {
      return 1;
    }
  }
}

void setCoord(struct sockaddr_in *client) {
  coord.hostname = inet_ntoa(client->sin_addr);
  coord.id = htons(client->sin_port);
}

// Return whether a COORD response was received or not.
int receiveCoordResponse() {
  struct sockaddr_in client;
  while (1) {
    int response = receiveMessage(&client, 0);
    if (response == COORD) {
      setCoord(&client);
      return 1;
    }
    if (response < 0) {
      return 0;
    }
  }
}

// Return whether this node becomes the new coordinator or not.
int sendElectToHigherOrderNodes(unsigned long electionId) {
  isOngoingElection = 1;
  isFailedElection = 0;
  isCoord = 0;
  coord = (Node){0};
  Node *currentNode = nodes;
  int hasAnswer = 0;
  while (currentNode != NULL) {
    if (currentNode->id > myNode.id) {
      sendElectMessage(currentNode, electionId);
      int answer = receiveElectionAnswers();
      if (answer) {
        hasAnswer = 1;
      }
    }
    currentNode = currentNode->next;
  }

  if (!hasAnswer) {
    // This node has become the new coordinator.
    logEvent("Declaring self as new coordinator");
    isCoord = 1;
    Node *currentNode = nodes;
    while (currentNode != NULL) {
      if (currentNode->id != myNode.id) {
        sendCoordinatorMessage(currentNode, electionId);
      }
      currentNode = currentNode->next;
    }
  } else {
    isCoord = 0;
    isFailedElection = !receiveCoordResponse();
  }
  isOngoingElection = 0;
  return isCoord;
}

int validateNode(struct sockaddr_in *client) {
  int nodePort = htons(client->sin_port);
  Node *curr = nodes;
  while (curr) {
    if (curr->id == nodePort)
      return 1;
    curr = curr->next;
  }
  return 0;
}

int receiveMessage(struct sockaddr_in *client, int block) {
  message message;
  socklen_t len = sizeof(*client);
  int isBlocking = block == 1 ? MSG_DONTWAIT : MSG_WAITALL;
  int n = recvfrom(sockfd, &message, sizeof(message), isBlocking,
                   (struct sockaddr *)client, &len);

  if (!block && n < 0) {
    // Timeout occurred.
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -1;
    } else {
      perror("Receiving error");
      return -2;
    }
  }

  if (n > 0) {
    // Convert back to host order.
    message.electionID = ntohl(message.electionID);
    message.msgID = ntohl(message.msgID);

    for (int i = 0; i < MAX_NODES; ++i) {
      message.vectorClock[i].nodeId = ntohl(message.vectorClock[i].nodeId);
      message.vectorClock[i].time = ntohl(message.vectorClock[i].time);
    }

    char *mType = printMessageType(message.msgID);
    uint16_t senderPort = ntohs(client->sin_port);
    printf("[%d] Received %s from %s:%d\n", message.electionID, mType,
           inet_ntoa(client->sin_addr), senderPort);
    if (!validateNode(client)) {
      logEvent("Received and ignored %s from invalid node %u", mType,
               senderPort);
      return 0;
    } else {
      mergeClocks(message.vectorClock);
      logEvent("Received %s from %u", mType, senderPort);
    }

    // Always respond to ELECT messages (unless another election is ongoing, in
    // which case keep yours, cancel other).
    if (message.msgID == ELECT) {
      if (!isOngoingElection) {
        Node node;
        node.hostname = inet_ntoa(client->sin_addr);
        node.id = htons(client->sin_port);
        sendElectionAnswerMessage(&node, node.id);
        sendElectToHigherOrderNodes(message.electionID);
      }
      return ELECT;
    } else if (isCoord && message.msgID == AYA) {
      Node node;
      node.hostname = inet_ntoa(client->sin_addr);
      node.id = htons(client->sin_port);
      sendMessageWithType(&node, node.id, IAA);
      return AYA;
    } else if (message.msgID == COORD) {
      return COORD;
    } else if (message.msgID == IAA) {
      resetTimer(getRandomNumber());
      return IAA;
    } else if (message.msgID == ANSWER) {
      return ANSWER;
    }
  }

  return 0;
}

int coordinate() {
  message response;
  struct sockaddr_in client;

  int messageType = receiveMessage(&client, 0);
  if (messageType == COORD) {
    if (ntohs(client.sin_port) > myNode.id) {
      isCoord = 0;
      setCoord(&client);
    } else {
      election();
    }
  }
  return 0;
}

void resetTimer(int seed) {
  countDownTimeInSeconds = seed;
  startTime = clock();
  ms = 0;
  s = 0;
  mins = 0;
  currentTime = 0;
}

void election() {
  unsigned long electionId = (unsigned long)rand();
  logEvent("Starting election %u", electionId);
  sendElectToHigherOrderNodes(electionId);
}

int getTime() {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) {
    perror("clock_gettime()");
  }
  return time.tv_sec;
}

int receiveIAA() {
  struct sockaddr_in client;
  while (1) {
    int response = receiveMessage(&client, 0);
    if (response == -1) {
      return -1;
    } else if (response == IAA) {
      return 0;
    }
  }
}

int sendAYA(Node *node, struct sockaddr_in *sockAddr) {
  sendMessageWithType(node, node->id, AYA);
  return receiveIAA();
}

void updateTimer() {
  currentTime = clock();
  ms = currentTime - startTime;
  s = (ms / (CLOCKS_PER_SEC)) - (mins * 60);
  mins = (ms / (CLOCKS_PER_SEC)) / 60;
  timeLeft = countDownTimeInSeconds - s;
}

void initLogFile(char *logFileName) {
  logFile = fopen(logFileName, "w+");
  if (logFile == NULL) {
    perror("Log file error: ");
    exit(EXIT_FAILURE);
  }
  logEvent("Starting node %lu", myNode.id);
}

Node *setNodes(void *fp) {
  char *line = NULL;
  size_t len;
  ssize_t read;
  Node *prev = NULL, *head = NULL;

  while ((read = getline(&line, &len, fp)) != -1) {
    char *hostname = strtok(line, " ");

    if (strcmp(hostname, line) != 0) {
      break;
    }

    char *id = strtok(NULL, " ");

    if (strtok(NULL, " ") != NULL) {
      break;
    }

    unsigned long nodeId = strtoul(id, NULL, 0);
    if (nodeId == 0) {
      break;
    }

    Node *curr = malloc(sizeof(Node));
    curr->id = nodeId;
    curr->hostname = strdup(hostname);
    curr->prev = prev;
    curr->next = NULL;

    if (prev) {
      prev->next = curr;
    } else {
      head = curr;
    }
    prev = curr;
    curr = curr->next;
  }

  return head;
}

Node *readGroupFromStdin() { return setNodes(stdin); }

void initGroupList() {
  if (strcmp(groupListFileName, "-") == 0) {
    nodes = readGroupFromStdin();
  } else {
    // Process group list file
    nodes = readGroupListFile(groupListFileName);
  }
}

void initPort(unsigned long port) {
  myNode.id = port;
  int isValidPort = validatePort(nodes, myNode.id);
  if (isValidPort == -1) {
    printf("Invalid port: %lu", myNode.id);
    exit(EXIT_FAILURE);
  }
}

void initHostName() {
  Node *currentNode = nodes;
  while (currentNode != NULL) {
    if (currentNode->id == myNode.id) {
      myNode.hostname = currentNode->hostname;
    }
    currentNode = currentNode->next;
  }
}

void init(unsigned long port) {
  initGroupList();
  initPort(port);
  initHostName();
  resetTimer(getRandomNumber());
  initVectorClock(nodeTimes, MAX_NODES, nodes);
  initServer();
  initLogFile(logFileName);
  setSocketTimeout(sockfd, timeoutValue);
}

void processArgs(int argc, char **argv) {
  if (argc != 7) {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  char *end;
  int err = 0;

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
    exit(EXIT_FAILURE);
  }

  init(strtoul(argv[1], &end, 10));
}

void eventLoop(struct addrinfo *addrInfo, struct sockaddr_in *sockAddr) {
  if (!isCoord) {
    if (addrInfo) {
      freeaddrinfo(addrInfo);
    }
    addrInfo = getAddress(&coord, &sockAddr);
    receiveMessage(sockAddr, 1);
    if (isFailedElection) {
      election();
    }
    if (timeLeft <= 0 && sendAYA(&coord, sockAddr) < 0) {
      election();
    }
    updateTimer();
  } else {
    coordinate();
  }
}

int main(int argc, char **argv) {
  srandom(time(0));
  processArgs(argc, argv);

  struct sockaddr_in *sockAddr = NULL, dummy;
  struct addrinfo *addrInfo = NULL;

  if (receiveMessage(&dummy, 0) <= 0) {
    election();
  }

  while (1) {
    eventLoop(addrInfo, sockAddr);
  }

  fclose(logFile);
}
