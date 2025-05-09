#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>

#define PORT_NUM 1004

#define RESET "\x1B[0m"
#define RED "\x1B[31m"
#define GREEN "\x1B[32m"
#define YELLOW "\x1B[33m"
#define BLUE "\x1B[34m"
#define MAGENTA "\x1B[35m"
#define CYAN "\x1B[36m"
#define BRIGHT_RED "\x1B[91m"
#define BRIGHT_GREEN "\x1B[92m"
#define BRIGHT_YELLOW "\x1B[93m"
#define BRIGHT_BLUE "\x1B[94m"
#define BRIGHT_MAGENTA "\x1B[95m"
#define BRIGHT_CYAN "\x1B[96m"



char *color_palette[] = {
    RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN,
    BRIGHT_RED, BRIGHT_GREEN, BRIGHT_YELLOW, BRIGHT_BLUE, BRIGHT_MAGENTA, BRIGHT_CYAN};
int palette_size = sizeof(color_palette) / sizeof(color_palette[0]);

typedef struct
{
    char name[50];
    char *color_code;
} UserColor;

UserColor user_colors[100];
int color_count = 0;

char *get_color_for_user(const char *name)
{
    for (int i = 0; i < color_count; i++)
    {
        if (strcmp(user_colors[i].name, name) == 0)
        {
            return user_colors[i].color_code;
        }
    }
    if (color_count < 100)
    {
        strcpy(user_colors[color_count].name, name);
        user_colors[color_count].color_code = color_palette[color_count % palette_size];
        return user_colors[color_count++].color_code;
    }
    return RESET;
}

void print_colored_message(const char *message)
{
    const char *start = strchr(message, '[');
    const char *end = strchr(message, ']');

    if (start && end && end > start)
    {
        char username[50];
        int len = end - start - 1;
        strncpy(username, start + 1, len);
        username[len] = '\0';

        char *color = get_color_for_user(username);

        char clearLine[512];
        strncpy(clearLine, message, sizeof(clearLine));
        clearLine[strcspn(clearLine, "\n")] = '\0';
        printf("%s%s%s\n", color, clearLine, RESET);
    }
    else
    {
        printf("%s\n", message);
    }
}

void error(const char *msg)
{
    perror(msg);
    exit(0);
}

typedef struct _ThreadArgs
{
    int clisockfd;
} ThreadArgs;

void *thread_main_recv(void *args)
{
    pthread_detach(pthread_self());

    int sockfd = ((ThreadArgs *)args)->clisockfd;
    free(args);

    // keep receiving and displaying message from server
    char buffer[512];
    int n;

    while (1)
    {
        memset(buffer, 0, 512);
        n = recv(sockfd, buffer, 512, 0);
        if (n < 0)
            error("ERROR recv() failed");
        if (n == 0)
            break; // connection closed
        print_colored_message(buffer);
    }

    return NULL;
}

void *thread_main_send(void *args)
{
    pthread_detach(pthread_self());

    int sockfd = ((ThreadArgs *)args)->clisockfd;
    free(args);

    char buffer[256];
    int n;

    while (1)
    {
        memset(buffer, 0, 256);
        fgets(buffer, 255, stdin);

        if (strlen(buffer) == 1)
            buffer[0] = '\0';

        n = send(sockfd, buffer, strlen(buffer), 0);
        if (n < 0)
            error("ERROR writing to socket");

        if (n == 0)
            break; // we stop transmission when user type empty string
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        error("Please specify hostname");
    }

    int room_number = -1;
    int usedRoomNum = 0;

    struct sockaddr_in serv_addr;
    socklen_t slen = sizeof(serv_addr);
    memset((char *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(PORT_NUM);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    printf("Try connecting to %s...\n", inet_ntoa(serv_addr.sin_addr));
    if (connect(sockfd, (struct sockaddr *)&serv_addr, slen) < 0)
        error("ERROR connecting");

    if (argc == 2) {
        // We did not specify room; ask server for room list
        int input = -2; // tell server we want room list
        send(sockfd, &input, sizeof(int), 0);

        char list[1024];
        int n = recv(sockfd, list, sizeof(list) - 1, 0);
        if (n <= 0)
            error("ERROR receiving room list");
        list[n] = '\0';

        printf("%s\n", list);
        printf("Choose the room number or type [new] to create a new room: ");

        char choice[20];
        fgets(choice, sizeof(choice), stdin);
        choice[strcspn(choice, "\n")] = 0;

        close(sockfd);

        if (strcmp(choice, "new") == 0) {
            room_number = -1;
        } else {
            room_number = atoi(choice);
        }

        // reconnect
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
            error("ERROR reopening socket");

        if (connect(sockfd, (struct sockaddr *)&serv_addr, slen) < 0)
            error("ERROR reconnecting");

        send(sockfd, &room_number, sizeof(int), 0);
        usedRoomNum = 1;
    } else if (argc == 3) {
        if (strcmp(argv[2], "new") != 0) {
            room_number = atoi(argv[2]);
        }
        // else room_number stays -1 (new)
    }

    if (!usedRoomNum) {
        send(sockfd, &room_number, sizeof(int), 0);
    }

    // Username
    char uname[50];
    printf("Type your user name: ");
    fgets(uname, sizeof(uname), stdin);
    uname[strcspn(uname, "\n")] = '\0';
    send(sockfd, uname, strlen(uname), 0);

    // Welcome or error
    char welcome[256] = {0};
    int n = recv(sockfd, welcome, sizeof(welcome) - 1, 0);
    if (n > 0) {
        welcome[n] = '\0';
        if (strstr(welcome, "Error:") != NULL) {
            printf("%s\n", welcome);
            close(sockfd);
            exit(1);
        }
        printf("%s", welcome);
    }

    printf("%s joined the chat room!\n", uname);

    // Threads to send and receive
    pthread_t tid1, tid2;
    ThreadArgs *args;

    usleep(100000);

    args = malloc(sizeof(ThreadArgs));
    args->clisockfd = sockfd;
    pthread_create(&tid1, NULL, thread_main_send, args);

    args = malloc(sizeof(ThreadArgs));
    args->clisockfd = sockfd;
    pthread_create(&tid2, NULL, thread_main_recv, args);

    pthread_join(tid1, NULL);
    pthread_cancel(tid2);
    pthread_join(tid2, NULL);
    close(sockfd);

    return 0;
}

	
