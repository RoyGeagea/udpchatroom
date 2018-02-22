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

#define PORT "2000"
#define MAX_BUF 200
#define STDIN 0

#define ERR(source) ( fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                        perror(source), exit(EXIT_FAILURE) )

#define MSG_LOGIN "#login"
#define MSG_LOGOUT "#logout"

#define MSG_PING "#ping"
#define MSG_PONG "#pong"
#define MSG_CLOSED "#closed"

char whoCommand[] = "_who";
char quitCommand[] = "_quit";

void usage(char *fidataLengthame) {
    fprintf(stderr, "Usage: %s\n", fidataLengthame);
    exit(EXIT_FAILURE);
}

void setSignalHandler(int signal, void (*handler)(int)) {
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = handler;
    if (sigaction(signal, &action, NULL) < 0)
        ERR("sigaction");
}

volatile sig_atomic_t shouldQuit = 0;
void handleSigInt(int signal) {
    shouldQuit = 1;
}

struct sockaddr_in makeAddress(char machine[], char port[]) {
    int errorCode = 0;
    struct sockaddr_in udpAddress;
    struct addrinfo *addressInfo;
    struct addrinfo hints = {};
    
    memset(&udpAddress, 0, sizeof(struct sockaddr_in));
    hints.ai_family = AF_INET;
    if((errorCode = getaddrinfo(machine, port, &hints, &addressInfo))) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(errorCode));
        exit(EXIT_FAILURE);
    }

    udpAddress = *(struct sockaddr_in *)(addressInfo->ai_addr);
    freeaddrinfo(addressInfo);
    return udpAddress;
}

int makeSocket(int domain, int type) {
    int socketFd;
    socketFd = socket(domain, type, 0);
    if(socketFd < 0) 
        ERR("socket");

    return socketFd;
}

void sendMessage(int clientFd, struct sockaddr_in serverAddress, char* message) {
    int bytesSent;
//    if (strcmp(message, MSG_PONG) != 0) {
//        printf("Sent %s\n", message);
//    }
    if ((bytesSent=sendto(clientFd, message, strlen(message), 0,
         (struct sockaddr *)&serverAddress, sizeof(struct sockaddr))) < 0)  {
			ERR("sendto");
    }
}

void receiveMessage(int socketFd, char* message) {
    uint bytesReceived;

    if ((bytesReceived = recvfrom(socketFd, message, MAX_BUF-1, 0, NULL, NULL)) == -1) {
        if (errno != EINTR && errno != EAGAIN) { // signal or timeout
            ERR("recvfrom");
        }
    }
    message[bytesReceived] = '\0';
    if (strcmp(message, MSG_PING) != 0 && strcmp(message, MSG_LOGOUT) != 0) {
        printf("%s\n", message);
    }
}

void doClient(int clientFd, char nom[], char machine[], char port[]) {
    struct sockaddr_in serverAddress = makeAddress(machine, port);

    char message[MAX_BUF];

    sendMessage(clientFd, serverAddress, MSG_LOGIN);
    sendMessage(clientFd, serverAddress, nom);
    receiveMessage(clientFd, message);
    
    fd_set masterFdsSet, readFdsSet;
    int dataLength;
    char stdinData[MAX_BUF] = {0};

    FD_ZERO(&masterFdsSet);
    FD_SET(STDIN, &masterFdsSet);
    FD_SET(clientFd, &masterFdsSet);

    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    struct timeval lastPing;
    gettimeofday(&lastPing, NULL);

    while(!shouldQuit) {
        struct timeval currentTime;
        gettimeofday(&currentTime, NULL);
        long secondsSinceLastPing  = currentTime.tv_sec  - lastPing.tv_sec;
        if(secondsSinceLastPing >= 6) {
            shouldQuit = 1;
            continue;
        }

        readFdsSet = masterFdsSet;
        struct timespec pselectTimeout;
        pselectTimeout.tv_sec = 6;
        pselectTimeout.tv_nsec = 0;
        if (pselect(clientFd+1, &readFdsSet, NULL, NULL, &pselectTimeout, &oldmask) == -1) {
            if (errno == EINTR || errno == EAGAIN) 
                continue;

            ERR("pselect");
        }

        if (FD_ISSET(STDIN, &readFdsSet)) {
            if (fgets(stdinData, sizeof(stdinData), stdin) == NULL) {
                shouldQuit = 1;
                continue;
            }

            if (strlen(stdinData) - 1 == strlen(quitCommand) && stdinData[0] == '_' && stdinData[1] == 'q' && stdinData[2] == 'u' && stdinData[3] == 'i' && stdinData[4] == 't') {
                shouldQuit = 1;
            }
            else {
                dataLength = strlen(stdinData) - 1;
                if (stdinData[dataLength] == '\n')
                    stdinData[dataLength] = '\0';
                
                sendMessage(clientFd, serverAddress, stdinData);
            }
        }
        if (FD_ISSET(clientFd, &readFdsSet)) {
            receiveMessage(clientFd, message);
            if(strcmp(message, MSG_LOGOUT) == 0 || strcmp(message, MSG_CLOSED) == 0) {
                shouldQuit = 1;
            }
            else if(strcmp(message, MSG_PING) == 0) {
                gettimeofday(&lastPing, NULL);
                sendMessage(clientFd, serverAddress, MSG_PONG);
            }
        }
    }
    
    sendMessage(clientFd, serverAddress, MSG_LOGOUT);
    receiveMessage(clientFd, message);
    // EXIT
}

void clientEngine(char nom[], char machine[], char port[]) {
    int socketFd = makeSocket(AF_INET, SOCK_DGRAM);
    setSignalHandler(SIGINT, handleSigInt);
    
    struct timeval messageTimeout;
    messageTimeout.tv_sec = 2;
    messageTimeout.tv_usec = 0;
    if (setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &messageTimeout, sizeof(messageTimeout)) < 0){
        ERR("setsockopt");
    }
    
    doClient(socketFd, nom, machine, port);
    
    if (close(socketFd) < 0)
        ERR("close");
    
//    printf("Exiting\n");
    return;
}

int main(int argc, char**argv) {
    char command[2000];
    char connect[2000];
    char nom[2000];
    char machine[2000];
    char port[2000];
    
    char firstSpace;
    char secondSpace;
    char thirdSpace;
    
    printf("Welcome to talk :)\n");
    // instruction pour le client
    printf("Type '_connect <surnom> <machine> <port>' to connect to your server.\n");
    printf("Type '_quit' to quit.\n");
    //    printf("Type '_who' to request the list of users from the server.\n");
    fputs("", stdout);
    fgets(command, sizeof (command), stdin);
    
    if (command[0] == '_' && command[1] == 'q' && command[2] == 'u' && command[3] == 'i' && command[4] == 't') {
//        printf("you enter the following command: _quit\n");
        return 0;
    }
    else {
        if (command[0] == '_' && command[1] == 'c' && command[2] == 'o' && command[3] == 'n' && command[4] == 'n' && command[5] == 'e' && command[6] == 'c' && command[7] == 't') {
            
            sscanf(command, "%s%c%s%c%s%c%s", connect, &firstSpace, nom, &secondSpace, machine, &thirdSpace, port);
//            printf("Name: %s\n", nom);
//            printf("Machine: %s\n", machine);
//            printf("Port: %s\n", port);
            
            clientEngine(nom, machine, port);
        }
        else {
            printf("We are not here to play looser, BYE !!!\n");
            return 0;
        }
    }
}
