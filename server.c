#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <arpa/inet.h>

#define MAX_BUF 600
#define MAX_CLIENTS 100

#define ERR(source) ( fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                        perror(source), exit(EXIT_FAILURE) )

#define MSG_LOGIN "#login"
#define MSG_LOGOUT "#logout"

#define MSG_PING "#ping"
#define MSG_PONG "#pong"
#define MSG_CLOSED "#closed"

volatile sig_atomic_t clientsCount = 0;

typedef struct clientArgs {
    int id;
    char username[100];
    struct sockaddr_in address;
    struct timeval lastPong;
} clientArgs_t;

clientArgs_t clients[MAX_CLIENTS];
int counter = 0;

char whoCommand[] = "_who";
char quitCommand[] = "_quit";
char shutdownCommand[] = "_shutdown";

int myServerFD;

void setSignalHandler(int signal, void (*handler)(int)) {
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = handler;
    if (sigaction(signal, &action, NULL) < 0)
        ERR("sigaction");
}

volatile sig_atomic_t shouldQuit = 0;
volatile sig_atomic_t lastSignal = 0;
void handleSigInt(int signal) {
    shouldQuit = 1;
    lastSignal = signal;
}

void handleSigAlrm(int signal) {
    lastSignal = signal;
}

int makeSocket(int domain, int type) {
    int socketFd;
    socketFd = socket(domain, type, 0);
    if(socketFd < 0) 
        ERR("socket");

    return socketFd;
}

int bindUdpSocket(uint16_t port) {
    struct sockaddr_in udpAddress; 
    int yes = 1;

    int socketFd = makeSocket(AF_INET, SOCK_DGRAM);
    memset(&udpAddress, 0, sizeof(struct sockaddr_in));
    udpAddress.sin_family = AF_INET;
    udpAddress.sin_port = htons(port);
    udpAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))) 
        ERR("setsockopt");

    if(bind(socketFd, (struct sockaddr*)&udpAddress, sizeof(udpAddress)) < 0)  
        ERR("bind");

    return socketFd;
}

void sendMessage(int serverFd, clientArgs_t client, char* message) {
    int bytesSent;
//    printf("[SENT] \"%s\" to %d\n\n", message, client.id);
    // printf("Family: %d, should be: %d\n", client.address.sin_family, AF_INET);
    // printf("IP: %lu, Port: %d\n", (unsigned long)client.address.sin_addr.s_addr, client.address.sin_port);
    if ((bytesSent=sendto(serverFd, message, strlen(message), 0,
         (struct sockaddr *)&client.address, sizeof(struct sockaddr))) < 0) {
        ERR("sendto");
    }
}

void sendMessageToOther(int serverFd, clientArgs_t clients[], int senderId, char* message) {
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(clients[i].id != -1 && clients[i].id != senderId) {
            sendMessage(serverFd, clients[i], message);
        }
    }
}

void logoutClient(int serverFd, int clientId) {
    char tmpMessage[MAX_BUF];
    sendMessage(serverFd, clients[clientId], MSG_LOGOUT);

    clients[clientId].id = -1;
    clientsCount--;
    counter--;

    snprintf(tmpMessage, MAX_BUF, "%s logged out", clients[clientId].username);
    sendMessageToOther(serverFd, clients, clientId, tmpMessage);
}

void logoutClientKill(int serverFd, int clientId) {
    char tmpMessage[MAX_BUF];
    sendMessage(serverFd, clients[clientId], MSG_LOGOUT);
}

void receiveMessage(int serverFd, clientArgs_t* tmpClient, char* message) {
    uint structSize = sizeof(struct sockaddr), bytesReceived;
    tmpClient->id = -1;

    if ((bytesReceived = recvfrom(serverFd, message, MAX_BUF-1, 0,
            (struct sockaddr *)&tmpClient->address, &structSize)) == -1)  {
        if (errno == EINTR) {
            tmpClient->id = -2;
            return;
        }
            
        ERR("recvfrom");
    }
    message[bytesReceived] = '\0';

    //printf("[REQUEST]Client on port: %hu\n", clientAddress.sin_port);
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(clients[i].id != -1 && clients[i].address.sin_port == tmpClient->address.sin_port) {
            *tmpClient = clients[i];
            break;
        }
    }

//    printf("[GOT] \"%s\" from %s\n", message, tmpClient->username);
}

void checkKeepAlive(int serverFd) {
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);
    long secondsSinceLastPong;

    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(clients[i].id != -1) {
            secondsSinceLastPong  = currentTime.tv_sec  - clients[i].lastPong.tv_sec;
            if (secondsSinceLastPong >= 6) {
//                printf("\tNo response from %d - logging out\n", i);
                logoutClient(serverFd, i);
            }
        }
    }
}

void acceptNewClient(int serverFd, clientArgs_t* currentClient) {
    char tmpMessage[MAX_BUF];
    int freeId = -1;
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].id == -1) {
            freeId = i;
            break;
        }
    }
    snprintf(tmpMessage, MAX_BUF, "%s is added to the group\n", currentClient->username);
    sendMessageToOther(serverFd, clients, freeId, tmpMessage);

    clients[freeId].id = freeId;
    clients[freeId].address = currentClient->address;
    strcpy(clients[freeId].username, currentClient->username);
    gettimeofday(&clients[freeId].lastPong, NULL);
    clientsCount++;

    snprintf(tmpMessage, MAX_BUF, "");
    sendMessage(serverFd, clients[freeId], tmpMessage);
    counter = counter + 1;
}

void doServer(int serverFd) {
    for(int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].id = -1;
    }

    char message[MAX_BUF];
    
    alarm(3);
    while (!shouldQuit) {
        memset(message, 0x00, MAX_BUF);
        checkKeepAlive(serverFd);

        if(lastSignal == SIGALRM) {
            sendMessageToOther(serverFd, clients, -1, MSG_PING);
            lastSignal = -1;
            alarm(3);
        }

        clientArgs_t currentClient;
        receiveMessage(serverFd, &currentClient, message);
        int clientId = currentClient.id;
        if(clientId == -2)
            continue;

        // Detect message type
        char tmpMessage[MAX_BUF];
        if(strcmp(message, MSG_LOGIN) == 0) {
            if(clientId != -1) { // DENY
                logoutClient(serverFd, clientId);
            }
            else if(clientsCount >= MAX_CLIENTS) { // DENY
                sendMessage(serverFd, currentClient, MSG_LOGOUT);
            }
            else if(clientsCount < MAX_CLIENTS) { // ACCEPT
                receiveMessage(serverFd, &currentClient, currentClient.username);
                acceptNewClient(serverFd, &currentClient);
            }
        }
        else if (strcmp(message, MSG_LOGOUT) == 0) {
            logoutClient(serverFd, clientId);
        }
        else if(strcmp(message, MSG_PONG) == 0) {
            struct timeval pongTime;
            gettimeofday(&pongTime, NULL);
            clients[clientId].lastPong = pongTime;
        }
        else { // message from stdin
            if (strlen(message) == strlen(whoCommand) && message[0] == '_' && message[1] == 'w' && message[2] == 'h' && message[3] == 'o') {
                char res[MAX_BUF];
                memset(res, 0x00, MAX_BUF);
                int i = 0;
                int j = 0;
                for(i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].id != -1) {
                        strcat(res, clients[i].username);
                        strcat(res, ", ");
                    }
                }
                res[strlen(res)-2] = '\0';
                strcat(res, "\n");
                res[strlen(res)] = '\0';
                
                sendMessage(serverFd, clients[clientId], res);
            }
            else {
                snprintf(tmpMessage, MAX_BUF, "%s: %s", currentClient.username, message);
                sendMessageToOther(serverFd, clients, clientId, tmpMessage);
            }
        }
    }

    // Closing
    sendMessageToOther(serverFd, clients, -1, MSG_CLOSED);
}

void handleShutdown() {
    int i;
    for(i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].id != -1) {
            sendMessage(myServerFD, clients[i], MSG_LOGOUT);

            clients[i].id = -1;
            clientsCount--;
            counter--;
        }
    }
}

void killConnectionWith(char surnom[]) {
    char res[MAX_BUF];
    int i;
    int j;
    int finded = -1;
    memset(res, 0x00, MAX_BUF);
    for(i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].id != -1) {
            if(strcmp(clients[i].username, surnom) == 0) {
                finded = i;
            }
        }
    }
    if (finded > -1) {
        logoutClientKill(myServerFD, clients[finded].id);
    }
    else {
        printf("This username does not exist\n");
    }
}

void *keyboard() {
    char msg[500];
    int len;
    char kill[500];
    char space;
    char surnom[100];
    while(fgets(msg,500,stdin) > 0) {
        if(len < 0) {
            perror("message not sent");
            exit(1);
        }
        if (strlen(msg) == strlen(shutdownCommand) + 1 && msg[0] == '_' && msg[1] == 's' && msg[2] == 'h' && msg[3] == 'u' && msg[4] == 't' && msg[5] == 'd' && msg[6] == 'o' && msg[7] == 'w' && msg[8] == 'n') {
            handleShutdown();
            exit(1);
        }
        else {
            if (msg[0] == '_' && msg[1] == 'k' && msg[2] == 'i' && msg[3] == 'l' && msg[4] == 'l') {
                sscanf(msg, "%s%c%s", kill, &space, surnom);
                killConnectionWith(surnom);
            }
        }
        memset(msg,'\0',sizeof(msg));
    }
}

int main(int argc,char *argv[]) {
    pthread_t keyboardThread;
    
    int socketFd = bindUdpSocket(atoi(argv[1]));

    myServerFD = socketFd;
    
    pthread_create(&keyboardThread, NULL, keyboard, NULL);
    
    setSignalHandler(SIGINT, handleSigInt);
    setSignalHandler(SIGALRM, handleSigAlrm);
    
    doServer(socketFd);

    if (close(socketFd) < 0)
        ERR("close");

    printf("Exiting\n");
    return EXIT_SUCCESS;
} 
