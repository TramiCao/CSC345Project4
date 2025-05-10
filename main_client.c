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
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#define PORT_NUM 3000
#define BUFFER_SIZE 512
#define FILE_CHUNK_SIZE 4096

// AI Assisted list. See report.pdf for details.
#define FILE_TRANSFER_CMD "SEND"
#define FILE_TRANSFER_REQUEST "FILE_TRANSFER_REQUEST"
#define FILE_TRANSFER_ACCEPT "FILE_TRANSFER_ACCEPT"
#define FILE_TRANSFER_REJECT "FILE_TRANSFER_REJECT"
#define FILE_TRANSFER_START "FILE_TRANSFER_START"
#define FILE_TRANSFER_CHUNK "FILE_TRANSFER_CHUNK"
#define FILE_TRANSFER_END "FILE_TRANSFER_END"
#define FILE_TRANSFER_ERROR "FILE_TRANSFER_ERROR"

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

typedef struct {
    int transfer_id;
    char sender_name[50];
    char receiver_name[50];
    char filename[256];
    char output_filename[256];
    size_t filesize;
    int active;
    int socket_fd;
    pthread_t thread_id;
} FileTransfer;

char *color_palette[] = {
    RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN,
    BRIGHT_RED, BRIGHT_GREEN, BRIGHT_YELLOW, BRIGHT_BLUE, BRIGHT_MAGENTA, BRIGHT_CYAN};
int palette_size = sizeof(color_palette) / sizeof(color_palette[0]);

typedef struct {
    char name[50];
    char *color_code;
} UserColor;

UserColor user_colors[100];
int color_count = 0;

// Global variables
char username[50];
int sockfd;
int room_number;
int waiting_for_transfer_response = 0;
int receiving_file = 0;
FileTransfer current_transfer = {0};
pthread_mutex_t transfer_mutex = PTHREAD_MUTEX_INITIALIZER;

char *get_color_for_user(const char *name) {
    for (int i = 0; i < color_count; i++) {
        if (strcmp(user_colors[i].name, name) == 0) {
            return user_colors[i].color_code;
        }
    }
    if (color_count < 100) {
        strcpy(user_colors[color_count].name, name);
        user_colors[color_count].color_code = color_palette[color_count % palette_size];
        return user_colors[color_count++].color_code;
    }
    return RESET;
}

void print_colored_message(const char *message) {
    const char *start = strchr(message, '[');
    const char *end = strchr(message, ']');

    if (start && end && end > start) {
        char username[50];
        int len = end - start - 1;
        strncpy(username, start + 1, len);
        username[len] = '\0';

        char *color = get_color_for_user(username);

        char clearLine[BUFFER_SIZE];
        strncpy(clearLine, message, sizeof(clearLine));
        clearLine[strcspn(clearLine, "\n")] = '\0';
        printf("%s%s%s\n", color, clearLine, RESET);
    } else {
        printf("%s\n", message);
    }
}

void error(const char *msg) {
    perror(msg);
    exit(1);
}

// Generate a unique output filename to avoid overwriting existing files
void generate_unique_filename(const char *base_filename, char *output) {
    // First check if file exists
    if (access(base_filename, F_OK) != 0) {
        strcpy(output, base_filename);
        return;
    }

    // File exists, generate a unique name
    char filename[256];
    char extension[32] = "";
    
    // Split filename and extension
    char *dot = strrchr(base_filename, '.');
    if (dot != NULL) {
        strncpy(filename, base_filename, dot - base_filename);
        filename[dot - base_filename] = '\0';
        strcpy(extension, dot);
    } else {
        strcpy(filename, base_filename);
    }
    
    // Try appending numbers until we find a unique name
    int counter = 1;
    char temp[256];
    size_t base_len = strlen(filename) + strlen(extension) + 2; // + 2 for "_" and potential null terminator
    
    // Make sure we have enough space (filename + "_" + counter + extension)
    // Counter needs max 3 digits (1 - 999)
    if (base_len > sizeof(temp) - 4) { 
        // Truncate the filename if it's too long
        size_t max_filename_len = sizeof(temp) - strlen(extension) - 6; // - 6 for "_", counter, and null terminator
        filename[max_filename_len] = '\0';
    }
    
    do {
        // Safer to use snprintf than sprintf for byte transfer
        int result = snprintf(temp, sizeof(temp), "%s_%d%s", filename, counter, extension);
        if (result < 0 || result >= (int)sizeof(temp)) {
            // If truncation occurs, instead use a simpler name
            snprintf(temp, sizeof(temp), "file_%d%s", counter, extension);
        }
        counter++;
    } while (access(temp, F_OK) == 0 && counter < 1000);
    
    strcpy(output, temp);
}

// Send a file to another user
// AI Assisted. See report.pdf for details.
void *send_file_thread(void *arg) {
    FileTransfer *transfer = (FileTransfer *)arg;
    // Header + chuck size
    char buffer[FILE_CHUNK_SIZE + 256];
    FILE *file = fopen(transfer->filename, "rb");
    
    if (file == NULL) {
        sprintf(buffer, "%s %d %s", FILE_TRANSFER_ERROR, transfer->transfer_id, "Could not open file");
        send(transfer->socket_fd, buffer, strlen(buffer), 0);
        pthread_mutex_lock(&transfer_mutex);
        transfer->active = 0;
        pthread_mutex_unlock(&transfer_mutex);
        return NULL;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    transfer->filesize = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Send file start message with metadata
    sprintf(buffer, "%s %d %s %zu", FILE_TRANSFER_START, transfer->transfer_id, transfer->filename, transfer->filesize);
    send(transfer->socket_fd, buffer, strlen(buffer), 0);
    
    size_t bytes_read;
    size_t total_sent = 0;
    int chunk_num = 0;
    
    // Send file in chunks
    while ((bytes_read = fread(buffer + 256, 1, FILE_CHUNK_SIZE, file)) > 0) {
        // Create header for this chunk
        int header_len = sprintf(buffer, "%s %d %d %zu ", FILE_TRANSFER_CHUNK, transfer->transfer_id, chunk_num, bytes_read);
        
        // Move data to make room for header
        memmove(buffer + header_len, buffer + 256, bytes_read);
        
        // Send the chunk
        int bytes_sent = send(transfer->socket_fd, buffer, header_len + bytes_read, 0);
        if (bytes_sent <= 0) {
            fclose(file);
            pthread_mutex_lock(&transfer_mutex);
            transfer->active = 0;
            pthread_mutex_unlock(&transfer_mutex);
            return NULL;
        }
        
        total_sent += bytes_read;
        chunk_num++;
        
        // Show progress
        printf("\rSending %s: %.2f%% (%zu/%zu bytes)", transfer->filename, (float)total_sent / transfer->filesize * 100, total_sent, transfer->filesize);
        fflush(stdout);
        
        // Delay to prevent CPU flooding
        usleep(1000);
    }
    
    fclose(file);
    
    // Send end message
    sprintf(buffer, "%s %d", FILE_TRANSFER_END, transfer->transfer_id);
    send(transfer->socket_fd, buffer, strlen(buffer), 0);
    
    printf("\nFile %s sent successfully!\n", transfer->filename);
    
    pthread_mutex_lock(&transfer_mutex);
    transfer->active = 0;
    pthread_mutex_unlock(&transfer_mutex);
    
    return NULL;
}

// Handle receiving a file
// AI Assisted. See report.pdf for details.
void *receive_file_thread(void *arg) {
    FileTransfer *transfer = (FileTransfer *)arg;
    FILE *file = fopen(transfer->output_filename, "wb");
    
    if (file == NULL) {
        printf("Error: Could not create file %s\n", transfer->output_filename);
        pthread_mutex_lock(&transfer_mutex);
        transfer->active = 0;
        receiving_file = 0;
        pthread_mutex_unlock(&transfer_mutex);
        return NULL;
    }
    
    printf("Receiving file: %s (Size: %zu bytes)\n", transfer->filename, transfer->filesize);
    
    // File will be received in the main receive loop
    // This thread just waits until the transfer is complete
    while (1) {
        pthread_mutex_lock(&transfer_mutex);
        int active = transfer->active;
        pthread_mutex_unlock(&transfer_mutex);
        
        if (!active) {
            break;
        }
        
        // Sleep a bit to avoid CPU flooding
        usleep(100000);
    }
    
    fclose(file);
    printf("\nFile received and saved as: %s\n", transfer->output_filename);
    
    pthread_mutex_lock(&transfer_mutex);
    receiving_file = 0;
    pthread_mutex_unlock(&transfer_mutex);
    
    return NULL;
}

// Parse the SEND file transfer command
int parse_send_command(const char *buffer, char *receiver, char *filename) {
    // Format is: SEND receiver_name filename
    char cmd[10];
    int result = sscanf(buffer, "%s %s %s", cmd, receiver, filename);
    
    if (result != 3) {
        return 0;
    }
    
    if (strcmp(cmd, FILE_TRANSFER_CMD) != 0) {
        return 0;
    }
    
    return 1;
}

// Handle special commands and file transfer messages
// AI Assisted. See report.pdf for details.
int handle_special_message(char *buffer) {
    // Check if it's a file transfer protocol message
    if (strncmp(buffer, FILE_TRANSFER_REQUEST, strlen(FILE_TRANSFER_REQUEST)) == 0) {
        // Parse: FILE_TRANSFER_REQUEST transfer_id sender_name filename filesize
        int transfer_id;
        char sender[50], filename[256];
        size_t filesize;
        
        sscanf(buffer, "%*s %d %s %s %zu", &transfer_id, sender, filename, &filesize);
        
        printf("\nFile transfer request from %s:\n", sender);
        printf("File: %s (Size: %zu bytes)\n", filename, filesize);
        printf("Accept? (Y/N): ");
        
        pthread_mutex_lock(&transfer_mutex);
        waiting_for_transfer_response = 1;
        current_transfer.transfer_id = transfer_id;
        strcpy(current_transfer.sender_name, sender);
        strcpy(current_transfer.filename, filename);
        current_transfer.filesize = filesize;
        current_transfer.socket_fd = sockfd;
        pthread_mutex_unlock(&transfer_mutex);
        
        return 1;
    }
    else if (strncmp(buffer, FILE_TRANSFER_START, strlen(FILE_TRANSFER_START)) == 0) {
        // Parse: FILE_TRANSFER_START transfer_id filename filesize
        int transfer_id;
        char filename[256];
        size_t filesize;
        
        sscanf(buffer, "%*s %d %s %zu", &transfer_id, filename, &filesize);
        
        // Generate unique filename to avoid overwriting
        char output_filename[256];
        generate_unique_filename(filename, output_filename);
        
        pthread_mutex_lock(&transfer_mutex);
        current_transfer.transfer_id = transfer_id;
        strcpy(current_transfer.filename, filename);
        strcpy(current_transfer.output_filename, output_filename);
        current_transfer.filesize = filesize;
        current_transfer.active = 1;
        receiving_file = 1;
        pthread_mutex_unlock(&transfer_mutex);
        
        // Create a new thread to handle the file reception
        pthread_create(&current_transfer.thread_id, NULL, receive_file_thread, &current_transfer);
        
        return 1;
    }
    else if (strncmp(buffer, FILE_TRANSFER_CHUNK, strlen(FILE_TRANSFER_CHUNK)) == 0) {
        // Parse: FILE_TRANSFER_CHUNK transfer_id chunk_num chunk_size [data]
        int transfer_id, chunk_num;
        size_t chunk_size;
        char *header_end;
        
        // Find the fourth space which marks the end of the header
        char *space1 = strchr(buffer, ' ');
        if (!space1) return 0;
        char *space2 = strchr(space1 + 1, ' ');
        if (!space2) return 0;
        char *space3 = strchr(space2 + 1, ' ');
        if (!space3) return 0;
        header_end = strchr(space3 + 1, ' ');
        if (!header_end) return 0;
        
        // Parse header
        sscanf(buffer, "%*s %d %d %zu", &transfer_id, &chunk_num, &chunk_size);
        
        // Get data portion
        char *data = header_end + 1;
        
        // Check if we're actively receiving a file
        pthread_mutex_lock(&transfer_mutex);
        int is_receiving = receiving_file && current_transfer.active && current_transfer.transfer_id == transfer_id;
        pthread_mutex_unlock(&transfer_mutex);
        
        if (is_receiving) {
            // Write chunk to file
            FILE *file = fopen(current_transfer.output_filename, "ab");
            if (file) {
                fwrite(data, 1, chunk_size, file);
                fclose(file);
                
                // Show progress
                static time_t last_update = 0;
                time_t now = time(NULL);
                
                // Get current file size
                struct stat st;
                stat(current_transfer.output_filename, &st);
                size_t current_size = st.st_size;
                
                // Update progress not more than once per second
                if (now > last_update || current_size == current_transfer.filesize) {
                    printf("\rReceiving %s: %.2f%% (%zu/%zu bytes)", current_transfer.filename, (float)current_size / current_transfer.filesize * 100, current_size, current_transfer.filesize);
                    fflush(stdout);
                    last_update = now;
                }
            }
        }
        
        return 1;
    }
    else if (strncmp(buffer, FILE_TRANSFER_END, strlen(FILE_TRANSFER_END)) == 0) {
        // Parse: FILE_TRANSFER_END transfer_id
        int transfer_id;
        sscanf(buffer, "%*s %d", &transfer_id);
        
        pthread_mutex_lock(&transfer_mutex);
        if (receiving_file && current_transfer.transfer_id == transfer_id) {
            current_transfer.active = 0;
            printf("\nFile transfer completed!\n");
        }
        pthread_mutex_unlock(&transfer_mutex);
        
        return 1;
    }
    else if (strncmp(buffer, FILE_TRANSFER_ERROR, strlen(FILE_TRANSFER_ERROR)) == 0) {
        // Parse: FILE_TRANSFER_ERROR transfer_id error_message
        int transfer_id;
        char error_msg[256];
        
        char *space1 = strchr(buffer, ' ');
        if (!space1) return 0;
        char *space2 = strchr(space1 + 1, ' ');
        if (!space2) return 0;
        
        strncpy(error_msg, space2 + 1, sizeof(error_msg) - 1);
        error_msg[sizeof(error_msg) - 1] = '\0';
        
        sscanf(buffer, "%*s %d", &transfer_id);
        
        printf("\nFile transfer error: %s\n", error_msg);
        
        pthread_mutex_lock(&transfer_mutex);
        if ((receiving_file || waiting_for_transfer_response) && 
            current_transfer.transfer_id == transfer_id) {
            receiving_file = 0;
            waiting_for_transfer_response = 0;
            current_transfer.active = 0;
        }
        pthread_mutex_unlock(&transfer_mutex);
        
        return 1;
    }
    
    // Check if it's a local SEND command
    if (strncmp(buffer, FILE_TRANSFER_CMD, strlen(FILE_TRANSFER_CMD)) == 0) {
        char receiver[50], filename[256];
        
        if (parse_send_command(buffer, receiver, filename)) {
            // Check if file exists
            if (access(filename, F_OK) != 0) {
                printf("Error: File '%s' not found.\n", filename);
                return 1;
            }
            
            // Construct and send file transfer request
            char req_buffer[BUFFER_SIZE];

            // Make a simple transfer ID
            int transfer_id = rand() % 10000;
            
            sprintf(req_buffer, "%s %s %d %s %s", "CMD", FILE_TRANSFER_CMD, transfer_id, receiver, filename);
            send(sockfd, req_buffer, strlen(req_buffer), 0);
            
            printf("File transfer request sent to %s for file '%s'.\n", receiver, filename);
            
            // Store transfer info
            pthread_mutex_lock(&transfer_mutex);
            current_transfer.transfer_id = transfer_id;
            strcpy(current_transfer.receiver_name, receiver);
            strcpy(current_transfer.filename, filename);
            strcpy(current_transfer.sender_name, username);
            current_transfer.socket_fd = sockfd;
            pthread_mutex_unlock(&transfer_mutex);
            
            return 1;
        }
    }
    
    return 0;
}

typedef struct {
    int clisockfd;
} ThreadArgs;

void *thread_main_recv(void *args) {
    pthread_detach(pthread_self());

    int sockfd = ((ThreadArgs *)args)->clisockfd;
    free(args);

    // keep receiving and displaying message from server
    char buffer[BUFFER_SIZE];
    int n;

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        n = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
        
        if (n < 0)
            error("ERROR recv() failed");
        if (n == 0)
            break; // connection closed
        
        // Handle file transfer protocol messages first
        if (handle_special_message(buffer)) {
            continue;
        }
        
        print_colored_message(buffer);
    }

    return NULL;
}

void *thread_main_send(void *args) {
    pthread_detach(pthread_self());

    int sockfd = ((ThreadArgs *)args)->clisockfd;
    free(args);

    char buffer[BUFFER_SIZE];
    int n;

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        
        // Check if we're waiting for a file transfer response
        pthread_mutex_lock(&transfer_mutex);
        int waiting = waiting_for_transfer_response;
        pthread_mutex_unlock(&transfer_mutex);
        
        if (!waiting) {
            fgets(buffer, BUFFER_SIZE - 1, stdin);
        } else {
            // Only accept Y/N for file transfer
            char response;
            scanf(" %c", &response);
            
            // Clear input buffer
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
            
            pthread_mutex_lock(&transfer_mutex);
            waiting_for_transfer_response = 0;
            pthread_mutex_unlock(&transfer_mutex);
            
            if (response == 'Y' || response == 'y') {
                // Accept the transfer
                char req_buffer[BUFFER_SIZE];
                sprintf(req_buffer, "%s %s %d", "CMD", FILE_TRANSFER_ACCEPT, current_transfer.transfer_id);
                send(sockfd, req_buffer, strlen(req_buffer), 0);
                
                // Generate unique filename
                generate_unique_filename(current_transfer.filename, current_transfer.output_filename);
                
                printf("Transfer accepted. File will be saved as %s\n", current_transfer.output_filename);
            } else {
                // Reject the transfer
                char req_buffer[BUFFER_SIZE];
                sprintf(req_buffer, "%s %s %d", "CMD", FILE_TRANSFER_REJECT, current_transfer.transfer_id);
                send(sockfd, req_buffer, strlen(req_buffer), 0);
                
                printf("Transfer rejected.\n");
            }
            
            continue;
        }

        if (strlen(buffer) == 1)
            buffer[0] = '\0';
            
        // Check if it's a special command
        if (handle_special_message(buffer)) {
            continue;
        }

        n = send(sockfd, buffer, strlen(buffer), 0);
        if (n < 0)
            error("ERROR writing to socket");

        if (n == 0)
            break; // we stop transmission when user type empty string
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        error("Please specify hostname");
    }

    room_number = -1;
    int already_sent_room_number = 0;

    struct sockaddr_in serv_addr;
    socklen_t slen = sizeof(serv_addr);
    memset((char *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(PORT_NUM);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    printf("Try connecting to %s...\n", inet_ntoa(serv_addr.sin_addr));
    if (connect(sockfd, (struct sockaddr *)&serv_addr, slen) < 0)
        error("ERROR connecting");

    if (argc == 2) {
        // We did not specify room; ask server for room list
        int special_code = -2; // tell server we want room list
        send(sockfd, &special_code, sizeof(int), 0);

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
        already_sent_room_number = 1;
    } else if (argc == 3) {
        if (strcmp(argv[2], "new") != 0) {
            room_number = atoi(argv[2]);
        }
        // else room_number stays -1 (new)
    }

    if (!already_sent_room_number) {
        send(sockfd, &room_number, sizeof(int), 0);
    }

    // Username
    printf("Type your user name: ");
    fgets(username, sizeof(username), stdin);
    username[strcspn(username, "\n")] = '\0';
    send(sockfd, username, strlen(username), 0);

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

    printf("%s joined the chat room!\n", username);
    
    // Initialize random number generator for transfer IDs
    srand(time(NULL));

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