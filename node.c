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

typedef struct node
{
    unsigned long id;
    char *hostname;
    struct node *next;
    struct node *prev;
} Node;

typedef struct coordinator
{
    unsigned long port;
    char *hostname;
} Coordinator;

void usage(char *cmd)
{
    printf("usage: %s  portNum groupFileList logFile timeoutValue averageAYATime "
           "failureProbability \n",
           cmd);
}

int validatePort(Node *nodes, unsigned long port)
{
    // Verify that the port number passed to this program is in the list of nodes;
    Node *curr = nodes;
    int hasPort = -1;
    while (curr != NULL)
    {
        if (port == curr->id)
        {
            hasPort = 0;
        }
        curr = curr->next;
    }
    return hasPort;
}

Node *readGroupListFile(char *fileName)
{
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    Node *prev = NULL, *next = NULL, *head = NULL;
    int numNodes = 0;

    fp = fopen(fileName, "r");
    if (fp == NULL)
    {
        exit(EXIT_FAILURE);
    }

    while ((read = getline(&line, &len, fp)) != -1)
    {
        numNodes++;

        char *hostname = strtok(line, " ");
        char *id = strtok(NULL, line);

        Node *curr = malloc(sizeof(Node));

        curr->id = strtoul(id, NULL, 0);
        curr->hostname = hostname;
        curr->prev = prev;
        curr->next = NULL;

        if (prev != NULL)
        {
            prev->next = curr;
        }
        prev = curr;

        if (numNodes <= 1)
        {
            head = curr;
        }
    }

    return head;
}

int initServer(unsigned long port)
{
    // This is some sample code to setup a UDP socket for sending and receiving.
    int sockfd;
    struct sockaddr_in servAddr;

    // Create the socket
    // The following must be one of the parameters don't leave this as it is

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Setup my server information
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    // Accept on any of the machine's IP addresses.
    servAddr.sin_addr.s_addr = INADDR_ANY;

    // Bind the socket to the requested addresses and port
    if (bind(sockfd, (const struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

void setSocketTimeout(int sockfd, unsigned long timeoutValue)
{
    struct timeval tv;
    tv.tv_sec = timeoutValue;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
}

int receiveMessage(int sockfd, unsigned long port, char *buf)
{
    struct sockaddr_in client;
    int len;

    memset(&client, 0, sizeof(client));
    client.sin_family = AF_INET;
    client.sin_port = htons(port);
    client.sin_addr.s_addr = INADDR_ANY;

    int n;
    n = recvfrom(sockfd, buf, 100, MSG_WAITALL, (struct sockaddr *)&client,
                 (socklen_t *)&len);

    // Timeout occurred.
    if (errno == EAGAIN || n == EWOULDBLOCK)
    {
        perror("Timeout occurred.");
        return -1;
    }

    // client will point to the address info of the node
    // that sent this message. The information can be used
    // to send back a response, if needed
    if (n < 0)
    {
        perror("Receiving error");
        return -3;
    }

    buf[n] = (char)0;
    printf("from %X:%d Size = %d - %s\n", ntohl(client.sin_addr.s_addr),
           ntohs(client.sin_port), n, buf);
    return 0;
}

void getRandomNumber(unsigned long AYATime)
{
    int i;
    for (i = 0; i < 10; i++)
    {
        int rn;
        rn = random();

        // scale to number between 0 and the 2*AYA time so that
        // the average value for the timeout is AYA time.

        int sc = rn % (2 * AYATime);
        printf("Random number %d is: %d\n", i, sc);
    }
}

int getAddress(Node *node, struct addrinfo *serverAddr)
{
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));

    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_INET;
    hints.ai_protocol = IPPROTO_UDP;

    if (getaddrinfo(node->hostname, (char *)node->id, &hints, &serverAddr))
    {
        perror("Couldn't lookup hostname\n");
        return -1;
    }
    return 0;
}

int sendMessage(message *msg, int sockfd, struct addrinfo *serverAddr)
{
    // Send the message to ourselves
    int bytesSent;
    bytesSent = sendto(sockfd, msg, sizeof(msg), MSG_CONFIRM, serverAddr->ai_addr,
                       serverAddr->ai_addrlen);
    if (bytesSent != sizeof(msg))
    {
        perror("UDP send failed: ");
        return -1;
    }
    return 0;
}

// Initialize the node's vector clock to match the list of nodes given.
// The time for each node is initially 0.
// The times will change once messages start being received.
void initVectorClock(vectorClock *nodeTimes, int numClocks, Node *node)
{
    Node *currentNode = node;
    for (int i = 0; i < numClocks; i++)
    {
        vectorClock clock = nodeTimes[i];
        if (currentNode == NULL)
        {
            perror("Number of nodes does not match number of clocks.");
            exit(EXIT_FAILURE);
        }
        clock.nodeId = node->id;
        clock.time = 0;
        currentNode = node->next;
    }
}

void createMessage(message *msg, unsigned long electionId, msgType type, vectorClock *nodeTimes)
{
    msg->electionID = electionId;
    msg->msgID = type;
    memcpy(msg->vectorClock, nodeTimes, sizeof(*nodeTimes) * MAX_NODES);
}

int isCoordinator()
{
    return 0;
}

void coordinate(int sockfd, vectorClock *nodeTimes)
{
}

void sendAYA(vectorClock *nodeTimes, int sockfd, char *coordinatorHostname, char *port, struct addrinfo *serverAddr)
{
    message msg;
    createMessage(&msg, (unsigned long)port, AYA, nodeTimes);
    sendMessage(&msg, sockfd, serverAddr);
    printf("Sent AYA [%ul, %ul] to %s\n", msg.electionID, msg.msgID, coordinatorHostname);
}

// Return 0 if this node is designated the coordinator, 1 otherwise.
// coordinatorId will be set to the new coordinator's ID at the end.
int election(Node *coord, Node *nodes, vectorClock *nodeTimes, int sockfd, unsigned long currentId)
{
    int isCoord = 0;
    unsigned long electionId = (unsigned long)rand();
    Node *currentNode = nodes;
    int count = 0;

    // Send message to all nodes that have an ID > than the current node's ID.
    while (currentNode != NULL)
    {
        if (currentNode->id > currentId)
        {
            message msg;
            struct addrinfo *serverAddr = NULL;
            getAddress(currentNode, serverAddr);
            createMessage(&msg, electionId, ELECT, nodeTimes);
            sendMessage(&msg, sockfd, serverAddr);
            count++;
        }
    }

    // Wait for ANSWER message from all the nodes above.
    // TODO: check if messages are from correct nodes.
    while (count > 0)
    {
        message response;
        receiveMessage(sockfd, currentId, (void *)&response);

        if (response.msgID == ANSWER)
        {
        }
        else if (response.msgID == ELECT)
        {
        }
        count--;
    }

    if (!isCoord)
    {
        // Wait for COORD message to determine who is the new coordinator.
        message response;
        while (1)
        {
            // TODO: If COORD message not received in <timeout> amount of time, restart election.
            receiveMessage(sockfd, currentId, (void *)&response);
            if (response.msgID == COORD)
            {
                return 1;
            }
        }
    }
    else
        return 0;
}

int sendAYAAndRespond(int sockfd, char *socketPort, Node *node, struct addrinfo *serverAddr, vectorClock *nodeTimes)
{
    message msg;

    // Send an initial AYA message to coordinator to start off the process.
    sendAYA(nodeTimes, sockfd, node->hostname, (char *)node->id, serverAddr);

    // Now wait for an IAA message.
    if (receiveMessage(sockfd, (unsigned long)socketPort, (void *)&msg) == -1)
    {
        // IAA not received before <timeout> seconds -> call election.
        printf("Failed to receive IAA.\n");
        return -1;
    }
    else
    {
        // IAA message was received.
        printf("Received IAA [%ul, %ul].\n", msg.electionID, msg.msgID);
        sleep(1000);
    }
    return 0;
}

int main(int argc, char **argv)
{
    // If you want to produce a repeatable sequence of "random" numbers
    // replace the call to  time() with an integer.
    srandom(time(0));

    unsigned long port;
    char *groupListFileName;
    char *logFileName;
    unsigned long timeoutValue;
    unsigned long AYATime;
    unsigned long myClock = 1;
    unsigned long sendFailureProbability;
    if (argc != 7)
    {
        usage(argv[0]);
        return -1;
    }

    // Some code illustrating how to parse command line arguments.
    // This cod will probably have to be changed to match how you
    // decide to do things.

    char *end;
    int err = 0;

    port = strtoul(argv[1], &end, 10);
    if (argv[1] == end)
    {
        printf("Port conversion error\n");
        err++;
    }

    groupListFileName = argv[2];
    logFileName = argv[3];

    timeoutValue = strtoul(argv[4], &end, 10);
    if (argv[4] == end)
    {
        printf("Timeout value conversion error\n");
        err++;
    }

    AYATime = strtoul(argv[5], &end, 10);
    if (argv[5] == end)
    {
        printf("AYATime conversion error\n");
        err++;
    }

    sendFailureProbability = strtoul(argv[6], &end, 10);
    if (argv[5] == end)
    {
        printf("sendFailureProbability conversion error\n");
        err++;
    }

    printf("Port number:              %lu\n", port);
    printf("Group list file name:     %s\n", groupListFileName);
    printf("Log file name:            %s\n", logFileName);
    printf("Timeout value:            %lu\n", timeoutValue);
    printf("AYATime:                  %lu\n", AYATime);
    printf("Send failure probability: %lu\n", sendFailureProbability);
    /* printf("Some examples of how to format data for shiviz\n"); */
    /* printf("Starting up Node %lu\n", port); */
    /* printf("N%lu {\"N%lu\" : %lu }\n", port, port, myClock++); */
    /* printf("Sending to Node 1\n"); */
    /* printf("N%lu {\"N%lu\" : %lu }\n", port, port, myClock++); */

    if (err)
    {
        printf("%d conversion error%sencountered, program exiting.\n", err,
               err > 1 ? "s were " : " was ");
        return -1;
    }

    Node *nodes = NULL;
    if (strcmp(groupListFileName, "-") == 0)
    {
        // Group list will be specified by stdin.
        printf("Group list specified by command line.\n");
    }
    else
    {
        // Process group list file
        printf("Reading group list...\n");
        nodes = readGroupListFile(groupListFileName);
    }

    int isValidPort = validatePort(nodes, port);

    if (isValidPort == -1)
    {
        exit(EXIT_FAILURE);
    }

    vectorClock nodeTimes[MAX_NODES];
    initVectorClock(nodeTimes, MAX_NODES, nodes);

    int sockfd = initServer(port);
    setSocketTimeout(sockfd, timeoutValue);

    struct addrinfo *serverAddr = NULL;

    message response;

    Node coord;
    coord.hostname = NULL;

    while (1)
    {
        if (!isCoordinator())
        {
            if (coord.hostname != NULL)
            {
                getAddress(&coord, serverAddr);
            }

            if (serverAddr == NULL || sendAYAAndRespond(sockfd, argv[1], &coord, serverAddr, nodeTimes) < 0)
            {
                // Either AYA timed out or coordinator is not known yet -> call election.
                election(&coord, nodes, nodeTimes, sockfd, port);
            }
        }
        else
        {
            coordinate(sockfd, nodeTimes);
        }
    }

    freeaddrinfo(serverAddr);
}
