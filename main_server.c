#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT_NUM 1004

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

typedef struct _USR
{
    int clisockfd;     // socket file descriptor
    char username[50]; // username added
    int room_number;
    struct sockaddr_in cliaddr;
    struct _USR *next; // for linked list queue
} USR;

USR *head = NULL;
USR *tail = NULL;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void add_tail(int newclisockfd, struct sockaddr_in addr, const char *uname, int room_number)
{
    USR *new_node = (USR *)malloc(sizeof(USR));
    new_node->clisockfd = newclisockfd;
    new_node->cliaddr = addr;
    new_node->room_number = room_number;
    strcpy(new_node->username, uname);
    new_node->next = NULL;

    pthread_mutex_lock(&lock);
    if (head == NULL)
    {
        head = tail = new_node;
    }
    else
    {
        tail->next = new_node;
        tail = new_node;
    }
    pthread_mutex_unlock(&lock);
}

void remove_client(int sockfd)
{
    pthread_mutex_lock(&lock);
    USR *prev = NULL, *cur = head;
    while (cur != NULL)
    {
        if (cur->clisockfd == sockfd)
        {
            if (prev == NULL)
                head = cur->next;
            else
                prev->next = cur->next;
            if (cur == tail)
                tail = prev;
            free(cur);
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&lock);
}

void broadcast(int fromfd, char *message)
{
    USR *sender = NULL;
    USR *cur;

    pthread_mutex_lock(&lock);
    cur = head;
    while (cur != NULL)
    {
        if (cur->clisockfd == fromfd)
        {
            sender = cur;
            break;
        }
        cur = cur->next;
    }

    if (sender != NULL)
    {
        cur = head;
        while (cur != NULL)
        {
            if (cur->clisockfd != fromfd && cur->room_number == sender->room_number)
            {
                char buffer[512];
                sprintf(buffer, "[%s] %s\n", sender->username, message);
                send(cur->clisockfd, buffer, strlen(buffer), 0);
            }
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&lock);
}

typedef struct _ThreadArgs
{
    int clisockfd;
    struct sockaddr_in cliaddr;
} ThreadArgs;

void *thread_main(void *args)
{
    pthread_detach(pthread_self());
    ThreadArgs *targs = (ThreadArgs *)args;
    int clisockfd = targs->clisockfd;
    struct sockaddr_in cliaddr = targs->cliaddr;
    //free(targs);

    int room_number;
    recv(clisockfd, &room_number, sizeof(int), 0);

    if (room_number < 0)
    {
        static int next_room = 100; // start room IDs from 100
        pthread_mutex_lock(&lock);
        room_number = next_room++;
        pthread_mutex_unlock(&lock);

        //char msg[128];
        //sprintf(msg, "Connected to %s with new room number %d\n", inet_ntoa(cliaddr.sin_addr), room_number);
        //send(clisockfd, msg, strlen(msg), 0);
    }
    else
    {
        // check if room exists
        int found = 0;
        pthread_mutex_lock(&lock);
        USR *cur = head;
        while (cur != NULL)
        {
            if (cur->room_number == room_number)
            {
                found = 1;
                break;
            }
            cur = cur->next;
        }
        pthread_mutex_unlock(&lock);
        if (!found)
        {
            char err[] = "Error: Room does not exist\n";
            send(clisockfd, err, strlen(err), 0);
            close(clisockfd);
            return NULL;
        }
    }

    //struct sockaddr_in cliaddr = ((ThreadArgs *)args)->cliaddr;
    //free(targs);

    char uname[50];
    int n = recv(clisockfd, uname, sizeof(uname), 0);
    //send(clisockfd, msg, strlen(msg), 0);

    if (n <= 0)
    {
        close(clisockfd);
        return NULL;
    }
    uname[n] = '\0';

    if(room_number >= 100){
        char msg[128];
        sprintf(msg, "Connected to %s with new room number %d\n", inet_ntoa(cliaddr.sin_addr), room_number);
        send(clisockfd, msg, strlen(msg), 0);
    }

    add_tail(clisockfd, cliaddr, uname, room_number);
    free(args);

    char join_msg[256];
    sprintf(join_msg, "%s (%s) joined the chat room!\n", uname, inet_ntoa(cliaddr.sin_addr));
    broadcast(-1, join_msg);
    printf("%s", join_msg);

    //char flush_buf[256];
    //recv(clisockfd, flush_buf, sizeof(flush_buf), MSG_DONTWAIT);

    char buffer[256];
    int nrcv = recv(clisockfd, buffer, 255, 0);
    while (nrcv > 0)
    {
        buffer[strcspn(buffer, "\n")] = '\0';
        broadcast(clisockfd, buffer);
        nrcv = recv(clisockfd, buffer, 255, 0);
    }

    char leave_msg[256];
    sprintf(leave_msg, "%s (%s) left the room!\n", uname, inet_ntoa(cliaddr.sin_addr));
    broadcast(-1, leave_msg);
    printf("%s", leave_msg);

    remove_client(clisockfd);
    close(clisockfd);

    return NULL;
}

int main(int argc, char *argv[])
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    // Allow address reuse to prevent 'address already in use' errors
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    socklen_t slen = sizeof(serv_addr);
    memset((char *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT_NUM);

    int status = bind(sockfd, (struct sockaddr *)&serv_addr, slen);
    if (status < 0)
        error("ERROR on binding");

    if (listen(sockfd, 5) < 0)
        error("ERROR on listen");

    printf("Server started on port %d\n", PORT_NUM);

    while (1)
    {
        struct sockaddr_in cli_addr;
        socklen_t clen = sizeof(cli_addr);
        int newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clen);
        if (newsockfd < 0)
            error("ERROR on accept");

        printf("Connected: %s\n", inet_ntoa(cli_addr.sin_addr));

        ThreadArgs *args = (ThreadArgs *)malloc(sizeof(ThreadArgs));
        args->clisockfd = newsockfd;
        args->cliaddr = cli_addr;

        pthread_t tid;
        if (pthread_create(&tid, NULL, thread_main, (void *)args) != 0)
            error("ERROR creating a new thread");
    }

    return 0;
}