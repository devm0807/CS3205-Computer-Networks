#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define SERVER_IP "127.0.0.1"

int sock;

// Function to send a file to the server
void send_file(int sock, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("File open error");
        return;
    }
    
    // Send filename
    send(sock, filename, 256, 0);
    
    // Get file size
    fseek(file, 0, SEEK_END);
    int file_size = ftell(file);
    rewind(file);
    
    // Send file size
    int file_size_n = htonl(file_size);
    send(sock, &file_size_n, sizeof(file_size_n), 0);
    printf("Sent file size: %d bytes\n", file_size);
    
    // Send file data in chunks
    char buffer[BUFFER_SIZE];
    int bytes_read, total_bytes_sent = 0;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        send(sock, buffer, bytes_read, 0);
        total_bytes_sent += bytes_read;
        printf("Sent chunk: %d bytes (Total: %d/%d)\n", bytes_read, total_bytes_sent, file_size);
    }
    
    fclose(file);
    printf("File sent completely!\n");
}

// Function to ensure all data is received
int recv_all(int sock, void *buffer, int length) {
    int total_received = 0;
    while (total_received < length) {
        int received_now = recv(sock, (char *)buffer + total_received, length - total_received, 0);
        if (received_now <= 0) return -1;
        total_received += received_now;
    }
    return total_received;
}

// Function to create all directories in a path
void create_directories(const char *path) {
    char temp[512];
    char *p = NULL;
    
    // Copy path to avoid modifying the original
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    // Remove trailing slash if present
    size_t len = strlen(temp);
    if (len > 0 && temp[len - 1] == '/') {
        temp[len - 1] = '\0';
    }
    
    // Create each directory in the path
    for (p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(temp, 0700) != 0 && errno != EEXIST) {
                printf("Failed to create directory: %s (errno: %d)\n", temp, errno);
            }
            *p = '/';
        }
    }
    
    // Create the final directory
    if (mkdir(temp, 0700) != 0 && errno != EEXIST) {
        printf("Failed to create final directory: %s (errno: %d)\n", temp, errno);
    }
}

// Function to handle updates from server
int handle_update() {
    char type[10];
    char src[256];
    char filename[256];
    int file_size;
    
    // Receive update type
    if (recv_all(sock, type, 10) < 0) {
        perror("Error receiving update type");
        return -1;  // Return error code to indicate disconnection
    }
    
    // Receive source path
    if (recv_all(sock, src, 256) < 0) {
        perror("Error receiving source path");
        return -1;
    }
    
    // Receive filename
    if (recv_all(sock, filename, 256) < 0) {
        perror("Error receiving filename");
        return -1;
    }
    
    // Receive file size
    if (recv_all(sock, &file_size, sizeof(file_size)) < 0) {
        perror("Error receiving file size");
        return -1;
    }
    
    file_size = ntohl(file_size);
    
    printf("Received update: %s %s %s (size: %d)\n", type, src, filename, file_size);
    
    // Create full path
    char full_path[512];
    char dir_path[512];
    
    // Handle the case where filename contains directory structure
    char *last_slash = strrchr(filename, '/');
    if (last_slash) {
        // Extract directory part from filename
        strncpy(dir_path, filename, last_slash - filename);
        dir_path[last_slash - filename] = '\0';
        
        // Create the full directory path
        char full_dir_path[512];
        if (strcmp(src, ".") == 0) {
            snprintf(full_dir_path, sizeof(full_dir_path), "%s", dir_path);
        } else {
            snprintf(full_dir_path, sizeof(full_dir_path), "%s/%s", src, dir_path);
        }
        
        // Create all directories in the path
        create_directories(full_dir_path);
        
        // Set the full path for the file
        if (strcmp(src, ".") == 0) {
            snprintf(full_path, sizeof(full_path), "%s", filename);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", src, filename);
        }
    } else {
        // No directory structure in filename
        if (strcmp(src, ".") == 0) {
            snprintf(full_path, sizeof(full_path), "%s", filename);
        } else {
            // Create the directory if it doesn't exist
            create_directories(src);
            snprintf(full_path, sizeof(full_path), "%s/%s", src, filename);
        }
    }
    
    // Handle different update types
    if (strncmp(type, "CREAT", 5) == 0) {
        struct stat st = {0};
    
        // Check if the path already exists
        if (stat(full_path, &st) == 0) {
            printf("Path already exists: %s\n", full_path);
        } else {
            if (type[5] == 'D') {
                if (mkdir(full_path, 0777) == -1) {
                    perror("Error creating directory");
                    return -1;
                }
                printf("Created directory: %s\n", full_path);
            } else {
                // Ensure parent directories exist
                char parent_dir[512];
                strncpy(parent_dir, full_path, sizeof(parent_dir));
                char *last_slash = strrchr(parent_dir, '/');
                if (last_slash) {
                    *last_slash = '\0';
                    create_directories(parent_dir);
                }
    
                // Create the file
                FILE *file = fopen(full_path, "wb");
                if (!file) {
                    perror("Error creating file");
                    printf("Failed to create file: %s\n", full_path);
                    return -1;
                }
    
                // Receive and write file data
                if (file_size > 0) {
                    char *buffer = malloc(file_size);
                    if (!buffer) {
                        perror("Memory allocation failed");
                        fclose(file);
                        return -1;
                    }
    
                    if (recv_all(sock, buffer, file_size) < 0) {
                        perror("Error receiving file data");
                        free(buffer);
                        fclose(file);
                        return -1;
                    }
    
                    fwrite(buffer, 1, file_size, file);
                    free(buffer);
                }
    
                fclose(file);
                printf("Created file: %s (%d bytes)\n", full_path, file_size);
            }
        }
    } else if (strcmp(type, "DELETE") == 0) {
        struct stat st = {0};
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (rmdir(full_path) == -1) {
                    perror("Error removing directory");
                } else {
                    printf("Removed directory: %s\n", full_path);
                }
            } else {
                if (unlink(full_path) == -1) {
                    perror("Error removing file");
                } else {
                    printf("Removed file: %s\n", full_path);
                }
            }
        } else {
            perror("Path does not exist");
        }
    }
    
    
    return 0;  // Success
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <path_to_local_directory> <path_to_ignore_list_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *local_directory = argv[1];
    char *ignore_list_file = argv[2];

    struct sockaddr_in server_addr;
    
    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Connect to server
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        exit(EXIT_FAILURE);
    }
    
    printf("Connected to server.\n");
    
    // Send ignore list file first
    send_file(sock, ignore_list_file);

    if (chdir(local_directory) != 0) {
        perror("Failed to change directory");
        exit(EXIT_FAILURE);
    }
    
    // Enter persistent mode to receive updates
    printf("Entering persistent mode to receive updates...\n");
    
    // Loop to receive updates from server
    while (1) {
        // Use select to check if socket is still connected
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;  // Check every second
        timeout.tv_usec = 0;
        
        int ready = select(sock + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready < 0) {
            perror("select failed");
            break;
        }
        
        if (ready > 0) {
            // Socket has data, try to handle the update
            if (handle_update() < 0) {
                printf("Server disconnected.\n");
                break;
            }
        }
    }
    
    close(sock);
    return 0;
}
