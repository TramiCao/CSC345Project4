#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define PORT_NUM 3000
#define BUFFER_SIZE 512
#define MAX_ROOMS 100
#define MAX_TRANSFERS 100

// AI Assisted. See report.pdf for details.
// File transfer protocol commands
#define FILE_TRANSFER_CMD "SEND"
#define FILE_TRANSFER_REQUEST "FILE_TRANSFER_REQUEST"
#define FILE_TRANSFER_ACCEPT "FILE_TRANSFER_ACCEPT"
#define FILE_TRANSFER_REJECT "FILE_TRANSFER_REJECT"
#define FILE_TRANSFER_START "FILE_TRANSFER_START"
#define FILE_TRANSFER_CHUNK "FILE_TRANSFER_CHUNK"
#define FILE_TRANSFER_END "FILE_TRANSFER_END"
#define FILE_TRANSFER_ERROR "FILE_TRANSFER_ERROR"

typedef struct _USR
{
    int clisockfd;     // socket file descriptor
    char username[50]; // username added
    int room_number;
    struct sockaddr_in cliaddr;
    struct _USR *next; // for linked list queue
} USR;

typedef struct _FileTransfer
{
    int transfer_id;
    int sender_sockfd;
    int receiver_sockfd;
    char sender_name[50];
    char receiver_name[50];
    char filename[256];
    size_t filesize;
    int active;
    time_t start_time;
    struct _FileTransfer *next;
} FileTransfer;

USR *head = NULL;
USR *tail = NULL;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

FileTransfer *transfer_head = NULL;
pthread_mutex_t transfer_lock = PTHREAD_MUTEX_INITIALIZER;

int room_user_counts[MAX_ROOMS] = {0};
int active_rooms[MAX_ROOMS] = {0};

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

// Find user by socket file descriptor
USR *find_user_by_sockfd(int sockfd)
{
    pthread_mutex_lock(&lock);
    USR *cur = head;
    while (cur != NULL)
    {
        if (cur->clisockfd == sockfd)
        {
            pthread_mutex_unlock(&lock);
            return cur;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

// Find user by username in a specific room
USR *find_user_by_name(const char *username, int room_number)
{
    pthread_mutex_lock(&lock);
    USR *cur = head;
    while (cur != NULL)
    {
        if (cur->room_number == room_number && strcmp(cur->username, username) == 0)
        {
            pthread_mutex_unlock(&lock);
            return cur;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

// Add a new file transfer to the list
void add_file_transfer(int transfer_id, int sender_sockfd, int receiver_sockfd, 
                      const char *sender_name, const char *receiver_name, 
                      const char *filename, size_t filesize)
{
    FileTransfer *new_transfer = (FileTransfer *)malloc(sizeof(FileTransfer));
    new_transfer->transfer_id = transfer_id;
    new_transfer->sender_sockfd = sender_sockfd;
    new_transfer->receiver_sockfd = receiver_sockfd;
    strcpy(new_transfer->sender_name, sender_name);
    strcpy(new_transfer->receiver_name, receiver_name);
    strcpy(new_transfer->filename, filename);
    new_transfer->filesize = filesize;
    new_transfer->active = 1;
    new_transfer->start_time = time(NULL);
    new_transfer->next = NULL;

    pthread_mutex_lock(&transfer_lock);
    if (transfer_head == NULL)
    {
        transfer_head = new_transfer;
    }
    else
    {
        FileTransfer *cur = transfer_head;
        while (cur->next != NULL)
        {
            cur = cur->next;
        }
        cur->next = new_transfer;
    }
    pthread_mutex_unlock(&transfer_lock);
}

// Find transfer by ID
FileTransfer *find_transfer_by_id(int transfer_id)
{
    pthread_mutex_lock(&transfer_lock);
    FileTransfer *cur = transfer_head;
    while (cur != NULL)
    {
        if (cur->transfer_id == transfer_id)
        {
            pthread_mutex_unlock(&transfer_lock);
            return cur;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&transfer_lock);
    return NULL;
}

// Remove transfer by ID
void remove_transfer(int transfer_id)
{
    pthread_mutex_lock(&transfer_lock);
    FileTransfer *prev = NULL;
    FileTransfer *cur = transfer_head;
    
    while (cur != NULL)
    {
        if (cur->transfer_id == transfer_id)
        {
            if (prev == NULL)
            {
                transfer_head = cur->next;
            }
            else
            {
                prev->next = cur->next;
            }
            free(cur);
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&transfer_lock);
}

// Clean up inactive transfers (timeout after 10 minutes)
void cleanup_transfers()
{
    time_t now = time(NULL);
    pthread_mutex_lock(&transfer_lock);
    
    FileTransfer *prev = NULL;
    FileTransfer *cur = transfer_head;
    
    while (cur != NULL)
    {
        // Check for transfers that have been active for more than 10 minutes
        if (difftime(now, cur->start_time) > 600)
        {
            FileTransfer *to_remove = cur;
            
            if (prev == NULL)
            {
                transfer_head = cur->next;
                cur = transfer_head;
            }
            else
            {
                prev->next = cur->next;
                cur = cur->next;
            }
            
            // If transfer is still active, send error to both sender and receiver
            if (to_remove->active)
            {
                char buffer[BUFFER_SIZE];
                sprintf(buffer, "%s %d %s", FILE_TRANSFER_ERROR, 
                        to_remove->transfer_id, "Transfer timeout");
                
                // Only send if the socket is still valid
                USR *sender = find_user_by_sockfd(to_remove->sender_sockfd);
                USR *receiver = find_user_by_sockfd(to_remove->receiver_sockfd);
                
                if (sender != NULL)
                {
                    send(to_remove->sender_sockfd, buffer, strlen(buffer), 0);
                }
                
                if (receiver != NULL)
                {
                    send(to_remove->receiver_sockfd, buffer, strlen(buffer), 0);
                }
            }
            
            free(to_remove);
        }
        else
        {
            prev = cur;
            cur = cur->next;
        }
    }
    
    pthread_mutex_unlock(&transfer_lock);
}

void send_room_list(int clisockfd)
{
    pthread_mutex_lock(&lock);
    USR *cur = head;
    int rooms[100] = {0}; // track people in each room
    int max_room = 0;

    while (cur != NULL)
    {
        int r = cur->room_number;
        if (r < 1)
        {
            cur = cur->next;
            continue;
        } // skip invalid rooms
        if (r >= 1 && r < MAX_ROOMS)
        {
            rooms[r - 1]++;
            if (r > max_room)
                max_room = r;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&lock);

    char msg[1024] = "Server says following options are available:\n";
    for (int i = 0; i < max_room; i++)
    {
        if (rooms[i] > 0)
        {
            char line[64];
            sprintf(line, "Room %d: %d people\n", i + 1, rooms[i]);
            strcat(msg, line);
        }
    }
    
    send(clisockfd, msg, strlen(msg), 0);
}

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
    if (room_number >= 1 && room_number <= MAX_ROOMS)
    {
        room_user_counts[room_number - 1]++;
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
            if (cur->room_number >= 1 && cur->room_number <= MAX_ROOMS)
            {
                room_user_counts[cur->room_number - 1]--;
            }

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
    
    // Cancel any active transfers involving this client
    pthread_mutex_lock(&transfer_lock);
    FileTransfer *transfer = transfer_head;
    while (transfer != NULL)
    {
        if (transfer->active && (transfer->sender_sockfd == sockfd || transfer->receiver_sockfd == sockfd)){
            
            // Notify the other party that the transfer has been cancelled
            int other_sockfd;
            if (transfer->sender_sockfd == sockfd){
                other_sockfd = transfer->receiver_sockfd;
            } else{
                other_sockfd = transfer->sender_sockfd;
            }
            
            char buffer[BUFFER_SIZE];
            sprintf(buffer, "%s %d %s", FILE_TRANSFER_ERROR, 
                    transfer->transfer_id, "Other party disconnected\n");
            
            // Only send if we can find the user (they might have disconnected too)
            USR *other_user = find_user_by_sockfd(other_sockfd);
            if (other_user != NULL){
                send(other_sockfd, buffer, strlen(buffer), 0);
            }
            
            transfer->active = 0;
        }
        transfer = transfer->next;
    }
    pthread_mutex_unlock(&transfer_lock);
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
                char buffer[BUFFER_SIZE];
                sprintf(buffer, "[%s] %s\n", sender->username, message);
                send(cur->clisockfd, buffer, strlen(buffer), 0);
            }
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&lock);
}

// Send message to a specific user
int send_to_user(const char *username, int room_number, const char *message)
{
    USR *receiver = find_user_by_name(username, room_number);
    if (receiver != NULL){
        send(receiver->clisockfd, message, strlen(message), 0);
        return 1;
    }
    return 0;
}

// Handle file transfer command from client
void handle_file_transfer_command(int sockfd, char *buffer)
{
    // The format is: SEND transfer_id receiver_name filename
    
    char cmd[20], subcmd[20], receiver_name[50], filename[256];
    int transfer_id;
    
    // Parse the command
    int result = sscanf(buffer, "%s %s %d %s %s", cmd, subcmd, &transfer_id, receiver_name, filename);
    
    if (result != 5){
        return;
    }
    
    if (strcmp(cmd, "CMD") != 0 || strcmp(subcmd, FILE_TRANSFER_CMD) != 0){
        return;
    }
    
    // Find the sender
    USR *sender = find_user_by_sockfd(sockfd);
    if (sender == NULL){
        return;
    }
    
    // Find the receiver in the same room
    USR *receiver = find_user_by_name(receiver_name, sender->room_number);
    if (receiver == NULL){
        // Receiver not found or not in the same room
        char error_msg[BUFFER_SIZE];
        sprintf(error_msg, "%s %d %s", FILE_TRANSFER_ERROR, transfer_id, "User not found or not in the same room");
        send(sockfd, error_msg, strlen(error_msg), 0);
        return;
    }
    
    // Send request to receiver, size will be sent later
    char request[BUFFER_SIZE];
    sprintf(request, "%s %d %s %s %d", FILE_TRANSFER_REQUEST, transfer_id, sender->username, filename, 0);
    
    send(receiver->clisockfd, request, strlen(request), 0);
    
    // Add to active transfers
    add_file_transfer(transfer_id, sockfd, receiver->clisockfd, sender->username, receiver_name, filename, 0);
    
    printf("File transfer request: %s wants to send %s to %s (ID: %d)\n", sender->username, filename, receiver_name, transfer_id);
}

// Handle file transfer accept/reject
void handle_file_transfer_response(int sockfd, char *buffer)
{
    // The format: CMD [ACCEPT/REJECT] transfer_id
    
    char cmd[20], subcmd[20];
    int transfer_id;
    
    // Parse the command
    int result = sscanf(buffer, "%s %s %d", cmd, subcmd, &transfer_id);
    
    if (result != 3){
        return;
    }
    
    if (strcmp(cmd, "CMD") != 0){
        return;
    }
    
    // Find the transfer
    FileTransfer *transfer = find_transfer_by_id(transfer_id);
    if (transfer == NULL || !transfer->active){
        return;
    }
    
    // Make sure this is the receiver responding
    if (transfer->receiver_sockfd != sockfd){
        return;
    }
    
    if (strcmp(subcmd, FILE_TRANSFER_ACCEPT) == 0){
        // Transfer accepted, notify sender to start
        char accept_msg[BUFFER_SIZE];
        sprintf(accept_msg, "%s %d", FILE_TRANSFER_ACCEPT, transfer_id);
        send(transfer->sender_sockfd, accept_msg, strlen(accept_msg), 0);
        
        printf("File transfer accepted: %s will receive %s from %s (ID: %d)\n", transfer->receiver_name, transfer->filename, transfer->sender_name, transfer_id);
    }
    else if (strcmp(subcmd, FILE_TRANSFER_REJECT) == 0){
        // Transfer rejected, notify sender
        char reject_msg[BUFFER_SIZE];
        sprintf(reject_msg, "%s %d", FILE_TRANSFER_REJECT, transfer_id);
        send(transfer->sender_sockfd, reject_msg, strlen(reject_msg), 0);
        
        // Remove the transfer
        transfer->active = 0;
        
        printf("File transfer rejected: %s declined %s from %s (ID: %d)\n", transfer->receiver_name, transfer->filename, transfer->sender_name, transfer_id);
    }
}

// Forward file transfer data between clients
void forward_transfer_data(int sockfd, char *buffer, int buffer_len)
{
    // Check if it's a file transfer protocol message
    if (strncmp(buffer, FILE_TRANSFER_START, strlen(FILE_TRANSFER_START)) == 0 ||
        strncmp(buffer, FILE_TRANSFER_CHUNK, strlen(FILE_TRANSFER_CHUNK)) == 0 ||
        strncmp(buffer, FILE_TRANSFER_END, strlen(FILE_TRANSFER_END)) == 0)
    {
        // Extract transfer ID
        char protocol_type[30];
        int transfer_id;
        
        sscanf(buffer, "%s %d", protocol_type, &transfer_id);
        
        // Find the transfer
        FileTransfer *transfer = find_transfer_by_id(transfer_id);
        if (transfer != NULL && transfer->active)
        {
            // Make sure this is the sender sending the data
            if (transfer->sender_sockfd == sockfd)
            {
                // Forward to receiver
                send(transfer->receiver_sockfd, buffer, buffer_len, 0);
                
                // If this is the end message, mark transfer as inactive
                if (strcmp(protocol_type, FILE_TRANSFER_END) == 0)
                {
                    transfer->active = 0;
                    printf("File transfer completed: %s sent %s to %s (ID: %d)\n", 
                           transfer->sender_name, transfer->filename, 
                           transfer->receiver_name, transfer_id);
                }
            }
            // Or the receiver sending an error
            else if (transfer->receiver_sockfd == sockfd && 
                     strcmp(protocol_type, FILE_TRANSFER_ERROR) == 0)
            {
                // Forward error to sender
                send(transfer->sender_sockfd, buffer, buffer_len, 0);
                transfer->active = 0;
            }
        }
        return;
    }
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
    // free(targs);

    int room_number;
    recv(clisockfd, &room_number, sizeof(int), 0);

    if (room_number == -2)
    {
        char list_msg[1024] = "Available chat rooms:\n";
        int any = 0;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_ROOMS; ++i)
        {
            if (room_user_counts[i] > 0)
            {
                char line[100];
                snprintf(line, sizeof(line), "Room %d: %d people\n", i + 1, room_user_counts[i]);
                strcat(list_msg, line);
                any = 1;
            }
        }
        pthread_mutex_unlock(&lock);
        if (!any)
        {
            strcpy(list_msg, "No rooms available. Type 'new' to create one.\n");
        }
        send(clisockfd, list_msg, strlen(list_msg), 0);
        close(clisockfd);
        return NULL;
    }

    char uname[50];
    int n = recv(clisockfd, uname, sizeof(uname), 0);

    if (room_number < 0)
    {
        static int next_room = 1; // start room IDs from 1
        pthread_mutex_lock(&lock);
        room_number = next_room++;
        active_rooms[room_number - 1] = 1;
        pthread_mutex_unlock(&lock);
    }
    else
    {
        if (room_number <= 0 || room_number > MAX_ROOMS)
        {
            char err[] = "Error: Room does not exist\n";
            send(clisockfd, err, strlen(err), 0);
            close(clisockfd);
            return NULL;
        }
    }

    if (n <= 0)
    {
        close(clisockfd);
        return NULL;
    }
    uname[n] = '\0';

    // Check if username already exists in the room
    if (find_user_by_name(uname, room_number) != NULL)
    {
        char err[] = "Error: Username already exists in this room\n";
        send(clisockfd, err, strlen(err), 0);
        close(clisockfd);
        return NULL;
    }

    if (room_number >= 1)
    {
        char msg[128];
        sprintf(msg, "Connected to %s with room number %d\n", inet_ntoa(cliaddr.sin_addr), room_number);
        send(clisockfd, msg, strlen(msg), 0);
    }

    add_tail(clisockfd, cliaddr, uname, room_number);
    free(targs);

    char join_msg[256];
    sprintf(join_msg, "%s (%s) joined the chat room!\n", uname, inet_ntoa(cliaddr.sin_addr));
    broadcast(-1, join_msg);
    printf("%s", join_msg);

    char buffer[BUFFER_SIZE];
    int nrcv = recv(clisockfd, buffer, BUFFER_SIZE - 1, 0);
    while (nrcv > 0)
    {
        buffer[nrcv] = '\0';
        
        // Check if it's a command or file transfer data
        if (strncmp(buffer, "CMD", 3) == 0)
        {
            // Check if it's a file transfer command
            if (strstr(buffer, FILE_TRANSFER_CMD) != NULL)
            {
                handle_file_transfer_command(clisockfd, buffer);
            }
            // Check if it's a file transfer response
            else if (strstr(buffer, FILE_TRANSFER_ACCEPT) != NULL || 
                     strstr(buffer, FILE_TRANSFER_REJECT) != NULL)
            {
                handle_file_transfer_response(clisockfd, buffer);
            }
        }
        // Check if it's file transfer data
        else if (strncmp(buffer, FILE_TRANSFER_START, strlen(FILE_TRANSFER_START)) == 0 ||
                 strncmp(buffer, FILE_TRANSFER_CHUNK, strlen(FILE_TRANSFER_CHUNK)) == 0 ||
                 strncmp(buffer, FILE_TRANSFER_END, strlen(FILE_TRANSFER_END)) == 0 ||
                 strncmp(buffer, FILE_TRANSFER_ERROR, strlen(FILE_TRANSFER_ERROR)) == 0)
        {
            forward_transfer_data(clisockfd, buffer, nrcv);
        }
        // Regular chat message
        else
        {
            buffer[strcspn(buffer, "\n")] = '\0';
            broadcast(clisockfd, buffer);
        }
        
        // Periodically clean up stale transfers (every ~10 messages)
        static int cleanup_counter = 0;
        if (++cleanup_counter % 10 == 0)
        {
            cleanup_transfers();
        }
        
        nrcv = recv(clisockfd, buffer, BUFFER_SIZE - 1, 0);
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

    // Initialize random number generator (for transfer IDs if needed)
    srand(time(NULL));

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