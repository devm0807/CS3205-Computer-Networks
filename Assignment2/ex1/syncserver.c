#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <netinet/in.h>
#include <pthread.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define BUFFER_SIZE 1024
#define MAX_EVENTS 1024
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (MAX_EVENTS * (EVENT_SIZE + 16))
#define MAX_IGNORE_ENTRIES 100
#define MAX_PATH_LENGTH 256

int PORT;
int MAX_CLIENTS;
int* client_sockets;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;
int client_count = 0;
int server_running = 1;
pthread_t* client_threads;
pthread_t monitor_thread;

typedef struct {
    int socket;
    char **ignore_list;
    int ignore_count;
} ClientInfo;

ClientInfo* clients;

// Function to check if a file is in the ignore list
int is_ignored(const char *filename, char **ignore_list, int ignore_count) {
    for (int i = 0; i < ignore_count; i++) {
        if (strstr(filename, ignore_list[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

#define MAX_WATCHES 1024

struct WatchInfo {
    int wd;
    char path[PATH_MAX];
};

struct WatchInfo watches[MAX_WATCHES];
int watch_count = 0;

// Function to add a watch for a directory
int add_watch(int fd, const char *path) {
    int wd = inotify_add_watch(fd, path, IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd == -1) {
        perror("inotify_add_watch failed");
        return -1;
    }
    
    if (watch_count < MAX_WATCHES) {
        watches[watch_count].wd = wd;
        strncpy(watches[watch_count].path, path, PATH_MAX);
        watch_count++;
        printf("Added watch for directory: %s (wd=%d)\n", path, wd);
    } else {
        printf("Maximum number of watches reached. Cannot add watch for: %s\n", path);
    }
    
    return wd;
}

// Function to remove a watch
void remove_watch(int fd, int wd) {
    inotify_rm_watch(fd, wd);
    for (int i = 0; i < watch_count; i++) {
        if (watches[i].wd == wd) {
            printf("Removed watch for directory: %s (wd=%d)\n", watches[i].path, wd);
            // Shift remaining watches
            for (int j = i; j < watch_count - 1; j++) {
                watches[j] = watches[j + 1];
            }
            watch_count--;
            break;
        }
    }
}

// Function to recursively add watches for all subdirectories
void add_watches_recursive(int fd, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        perror("Failed to open directory");
        return;
    }

    add_watch(fd, dir_path);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char new_path[PATH_MAX];
        snprintf(new_path, sizeof(new_path), "%s/%s", dir_path, entry->d_name);

        struct stat statbuf;
        if (stat(new_path, &statbuf) == -1) {
            perror("stat failed");
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            add_watches_recursive(fd, new_path);
        }
    }

    closedir(dir);
}

// Function to read file content
char* read_file(const char *path, int *size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror("[ERROR] File open error");
        *size = 0;
        return NULL;
    }

    // Check if it's a directory before proceeding
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("[ERROR] %s is a directory! Not a file.\n", path);
        fclose(file);
        *size = 0;
        return NULL;
    }

    // Seek to end and get file size
    if (fseek(file, 0, SEEK_END) != 0) {
        perror("[ERROR] fseek failed");
        fclose(file);
        *size = 0;
        return NULL;
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        perror("[ERROR] ftell failed");
        fclose(file);
        *size = 0;
        return NULL;
    }
    rewind(file);

    // Allocate buffer
    char *buffer = malloc(file_size);
    if (!buffer) {
        perror("[ERROR] Memory allocation failed");
        fclose(file);
        *size = 0;
        return NULL;
    }

    // Read file content
    size_t read_size = fread(buffer, 1, file_size, file);
    if (read_size != file_size) {
        perror("[ERROR] fread did not read expected bytes");
        free(buffer);
        fclose(file);
        *size = 0;
        return NULL;
    }

    fclose(file);
    *size = file_size;
    return buffer;
}

// Function to receive all data
int recv_all(int sock, void *buffer, int length) {
    int total_received = 0;
    while (total_received < length) {
        int received_now = recv(sock, (char *)buffer + total_received, length - total_received, 0);
        if (received_now <= 0) return -1;
        total_received += received_now;
    }
    return total_received;
}

// Function to send file update to client
void send_update(int client_sock, const char *type, const char *src, const char *filename, int file_size, const char *file_data) {
    pthread_mutex_lock(&sock_mutex);
    
    // Send update type
    if (send(client_sock, type, 10, 0) <= 0) {
        pthread_mutex_unlock(&sock_mutex);
        return;
    }
    
    // Send source path
    if (send(client_sock, src, 256, 0) <= 0) {
        pthread_mutex_unlock(&sock_mutex);
        return;
    }
    
    // Send filename
    if (send(client_sock, filename, 256, 0) <= 0) {
        pthread_mutex_unlock(&sock_mutex);
        return;
    }
    
    // Send file size
    int file_size_n = htonl(file_size);
    if (send(client_sock, &file_size_n, sizeof(file_size_n), 0) <= 0) {
        pthread_mutex_unlock(&sock_mutex);
        return;
    }
    
    // Send file data if needed
    if (file_size > 0) {
        if (send(client_sock, file_data, file_size, 0) <= 0) {
            pthread_mutex_unlock(&sock_mutex);
            return;
        }
    }
    
    printf("Sent update to client %d: %s %s %s (size: %d)\n", client_sock, type, src, filename, file_size);
    pthread_mutex_unlock(&sock_mutex);
}

// Function to broadcast update to all clients
void broadcast_update(const char *type, const char *src, const char *filename, int file_size, const char *file_data) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket > 0 && !is_ignored(filename, clients[i].ignore_list, clients[i].ignore_count)) {
            send_update(clients[i].socket, type, src, filename, file_size, file_data);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_watches_recursive(int fd, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        perror("Failed to open directory");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char new_path[PATH_MAX];
        snprintf(new_path, sizeof(new_path), "%s/%s", dir_path, entry->d_name);

        struct stat statbuf;
        if (stat(new_path, &statbuf) == -1) {
            perror("stat failed");
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            // send dir
            broadcast_update("CREATD", ".", new_path, 0, NULL);
            send_watches_recursive(fd, new_path);
        }
        else {
            // send file
            int file_size;
            char *file_data = read_file(new_path, &file_size);
            puts(new_path);
            broadcast_update("CREATF", ".", new_path, file_size, file_data);
            free(file_data);
        }
    }

    closedir(dir);
}

void watch_directory(int fd, const char *dir_path, int *wd_count);
// Function to process inotify events
void process_event(int fd, struct inotify_event *event, const char *base_path) {
    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%s/%s", base_path, event->name);

    if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO) {
        sync();

        struct stat statbuf;
                
        if (stat(path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
            add_watches_recursive(fd, path);
            broadcast_update("CREATD", base_path, event->name, 0, NULL);
            send_watches_recursive(fd, path);
        } else {
            int file_size;
            char *file_data = read_file(path, &file_size);
            broadcast_update("CREATF", base_path, event->name, file_size, file_data);
            free(file_data);
        }
    }

    if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
        sync();
        printf("Deleted: %s\n", path);
        struct stat statbuf;
        if (stat(path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
            // If it's a directory, remove all watches for subdirectories
            for (int i = watch_count - 1; i >= 0; i--) {
                if (strncmp(watches[i].path, path, strlen(watches[i].path)) == 0) {
                    remove_watch(fd, watches[i].wd);
                }
            }
        }
        broadcast_update("DELETE", base_path, event->name, 0, NULL);
    }
}


// Function to watch directory recursively
void watch_directory(int fd, const char *dir_path, int *wd_count) {
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];

    int wd = inotify_add_watch(fd, dir_path, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd == -1) {
        perror("inotify_add_watch failed");
        return;
    }

    printf("Watching directory: %s (wd=%d)\n", dir_path, wd);
    (*wd_count)++;

    dir = opendir(dir_path);
    if (dir == NULL) {
        perror("Failed to open directory");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
            watch_directory(fd, path, wd_count);
        }
    }

    closedir(dir);
}


// Thread function to monitor directory changes
void *monitor_directory(void *arg) {
    int fd = inotify_init();
    if (fd == -1) {
        perror("inotify_init failed");
        return NULL;
    }

    const char *root_dir = ".";
    add_watches_recursive(fd, root_dir);

    char buffer[EVENT_BUF_LEN];
    fd_set rfds;
    struct timeval timeout;

    while (server_running) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeout.tv_sec = 1;  // 1-second timeout
        timeout.tv_usec = 0;

        int ret = select(fd + 1, &rfds, NULL, NULL, &timeout);
        if (ret < 0) {
            perror("select failed");
            break;
        } else if (ret == 0) {
            continue;  // Timeout, check shutting_down again
        }

        int length = read(fd, buffer, sizeof(buffer));
        if (length <= 0) {
            if (length < 0) perror("read failed");
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event*)&buffer[i];
            char *base_path = "."; // Default
            for (int j = 0; j < watch_count; j++) {
                if (watches[j].wd == event->wd) {
                    base_path = watches[j].path;
                    break;
                }
            }
            process_event(fd, event, base_path);
            i += EVENT_SIZE + event->len;
        }
    }

    close(fd);
    printf("Monitor thread exiting...\n");
    return NULL;
}


// Function to receive ignore list file
void receive_ignore_list(ClientInfo *client) {
    int sock = client->socket;
    
    char filename[256];
    int file_size;
    
    // Receive filename
    if (recv_all(sock, filename, sizeof(filename)) < 0) {
        perror("Filename receive error");
        return;
    }
    
    printf("Receiving ignore list: %s\n", filename);
    
    // Receive file size
    if (recv_all(sock, &file_size, sizeof(file_size)) < 0) {
        perror("File size receive error");
        return;
    }
    
    file_size = ntohl(file_size);
    printf("Ignore list size: %d bytes\n", file_size);
    
    // Receive file data
    char *file_data = malloc(file_size + 1);
    if (!file_data) {
        perror("Memory allocation failed");
        return;
    }
    
    if (recv_all(sock, file_data, file_size) < 0) {
        perror("File data receive error");
        free(file_data);
        return;
    }
    
    file_data[file_size] = '\0';  // Null-terminate for string operations
    
    // Parse CSV data into client's ignore_list
    client->ignore_count = 0;
    client->ignore_list = malloc(MAX_IGNORE_ENTRIES * sizeof(char*));
    
    char *token = strtok(file_data, ",\n");
    while (token != NULL && client->ignore_count < MAX_IGNORE_ENTRIES) {
        client->ignore_list[client->ignore_count] = strdup(token);
        client->ignore_count++;
        token = strtok(NULL, ",\n");
    }
    
    printf("Loaded %d entries into client's ignore list\n", client->ignore_count);
    free(file_data);
}

// Thread function to handle client
void *handle_client(void *arg) {
    int client_sock = *((int *)arg);
    free(arg);
    
    // Find available client slot
    int client_index = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket <= 0) {
            client_index = i;
            clients[i].socket = client_sock;
            clients[i].ignore_list = NULL;
            clients[i].ignore_count = 0;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    if (client_index == -1) {
        printf("No available client slots. Rejecting connection.\n");
        close(client_sock);
        return NULL;
    }
    
    ClientInfo *client = &clients[client_index];
    
    // Receive ignore list
    receive_ignore_list(client);
    
    // Keep the connection alive
    char buffer[BUFFER_SIZE];
    while (server_running) {
        // Use select to check if socket is still connected
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_sock, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;  // Check every second
        timeout.tv_usec = 0;
        
        int ready = select(client_sock + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready < 0) {
            perror("select failed");
            break;
        }
        
        if (ready > 0) {
            // Socket has data or has been closed
            int bytes = recv(client_sock, buffer, BUFFER_SIZE, MSG_PEEK);
            if (bytes <= 0) {
                // Client disconnected
                break;
            }
        }
    }
    
    // Client disconnected, clean up
    pthread_mutex_lock(&clients_mutex);
    // Remove client from client_sockets array
    for (int i = 0; i < client_count; i++) {
        if (client_sockets[i] == client_sock) {
            // Shift remaining clients
            for (int j = i; j < client_count - 1; j++) {
                client_sockets[j] = client_sockets[j + 1];
            }
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    // Free client's ignore list
    if (client->ignore_list != NULL) {
        for (int j = 0; j < client->ignore_count; j++) {
            free(client->ignore_list[j]);
        }
        free(client->ignore_list);
        client->ignore_list = NULL;
        client->ignore_count = 0;
    }
    
    printf("Client disconnected (socket: %d).\n", client_sock);
    close(client_sock);
    client->socket = -1;  // Mark slot as available
    
    return NULL;
}


// Signal handler for graceful shutdown
void handle_signal(int sig) {
    printf("\nShutting down server...\n");
    server_running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <path_to_local_directory> <port> <max_clients>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *local_directory = argv[1];
    PORT = atoi(argv[2]);
    MAX_CLIENTS = atoi(argv[3]);
    client_sockets = (int*) malloc(MAX_CLIENTS * sizeof(int));
    client_threads = (pthread_t*)  malloc(MAX_CLIENTS * sizeof(pthread_t));
    clients = (ClientInfo*) malloc(MAX_CLIENTS * sizeof(ClientInfo));

    if (chdir(local_directory) != 0) {
        perror("chdir failed");
        exit(EXIT_FAILURE);
    }

    int server_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size = sizeof(client_addr);
    pthread_t monitor_thread;
    
    // Initialize client sockets array
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_sockets[i] = -1;
    }
    
    // Create socket
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Configure server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    // Bind socket
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    printf("Server is listening on port %d...\n", PORT);
    
    // Start directory monitoring thread
    pthread_create(&monitor_thread, NULL, monitor_directory, NULL);
    
    // Set up for select
    fd_set read_fds;
    int max_fd;
    
    while (server_running) {
        FD_ZERO(&read_fds);
        FD_SET(server_sock, &read_fds);
        FD_SET(STDIN_FILENO, &read_fds); // Monitor standard input for server shutdown command
        
        max_fd = server_sock > STDIN_FILENO ? server_sock : STDIN_FILENO;
        
        // Use select to wait for activity with timeout
        struct timeval timeout;
        timeout.tv_sec = 1; // Check server_running every second
        timeout.tv_usec = 0;
        
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity < 0 && errno != EINTR) {
            perror("select failed");
            break;
        }
        
        // Check for command on standard input
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char cmd[20];
            fgets(cmd, sizeof(cmd), stdin);
            
            if (strncmp(cmd, "quit", 4) == 0 || strncmp(cmd, "exit", 4) == 0) {
                printf("Server shutdown initiated...\n");
                server_running = 0;
                break;
            }
        }
        
        // Check for new client connection
        if (FD_ISSET(server_sock, &read_fds)) {
            int new_client = accept(server_sock, (struct sockaddr*)&client_addr, &addr_size);
            if (new_client < 0) {
                perror("Accept failed");
                continue;
            }
            
            printf("New client connected (socket: %d)\n", new_client);
            
            // Add client to array
            pthread_mutex_lock(&clients_mutex);
            if (client_count < MAX_CLIENTS) {
                client_sockets[client_count] = new_client;
                
                // Create thread argument
                int *client_sock_ptr = malloc(sizeof(int));
                if (!client_sock_ptr) {
                    perror("Memory allocation failed");
                    close(new_client);
                    pthread_mutex_unlock(&clients_mutex);
                    continue;
                }
                
                *client_sock_ptr = new_client;
                
                // Create thread for client
                pthread_t client_thread;
                if (pthread_create(&client_thread, NULL, handle_client, client_sock_ptr) != 0) {
                    perror("Thread creation failed");
                    free(client_sock_ptr);
                    close(new_client);
                    pthread_mutex_unlock(&clients_mutex);
                    continue;
                }
                
                // Detach thread to automatically clean up resources
                pthread_detach(client_thread);
                client_count++;
            } else {
                printf("Maximum clients reached. Connection rejected.\n");
                close(new_client);
            }
            
            pthread_mutex_unlock(&clients_mutex);
        }
    }
    
    // Clean up
    printf("Shutting down server...\n");
    
    // Close all client connections
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (client_sockets[i] > 0) {
            close(client_sockets[i]);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    // Wait for monitor thread to finish
    pthread_join(monitor_thread, NULL);
    
    // Close server socket
    close(server_sock);

    free(client_sockets);
    free(client_threads);
    
    // Free client-specific ignore lists
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].ignore_list != NULL) {
            for (int j = 0; j < clients[i].ignore_count; j++) {
                free(clients[i].ignore_list[j]);
            }
            free(clients[i].ignore_list);
        }
    }
    
    printf("Server shutdown complete.\n");
    return 0;
}
