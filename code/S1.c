#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <libgen.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

// Buffer and path size definitions
#define BUFFER_SIZE 8192       // Size for data transfer buffers
#define MAX_PATH_LEN 1024      // Maximum path length allowed
#define MAX_FILES 1000         // Maximum number of files to handle in directory listings

// Port definitions for the distributed file system
#define S1_PORT 8000           // Main server port (handles .c files)
#define S2_PORT 8001           // Auxiliary server port (handles .pdf files)
#define S3_PORT 8002           // Auxiliary server port (handles .txt files)
#define S4_PORT 8003           // Auxiliary server port (handles .zip files)

 // Function prototypes
void prcclient(int client_fd);                    // Process client requests in child process
void setup_signal_handlers();                    // Configure signal handlers for process cleanup
void sigchld_handler(int sig);                   // Handle child process termination signals
void ensure_directory_exists(const char* path);  // Create directory structure if it doesn't exist
int string_compare(const void* a, const void* b); // Comparison function for qsort

// File handling functions
const char* get_file_extension(const char* filename);                                     // Extract file extension from filename
int get_server_for_file(const char* filepath);                                          // Determine which server should handle a file type
void convert_path_for_server(const char* s1_path, char* server_path, const char* server_name); // Convert S1 path format to auxiliary server path format

// Connection functions
int connect_to_aux_server(int port);             // Establish connection to auxiliary servers (S2, S3, S4)

// Download functions (downlf)
int s1_handle_downlf(int client_fd, char* command);                                     // Main handler for download file requests
int send_local_file_to_client(int client_fd, const char* filepath);                    // Send .c files stored locally on S1
int forward_file_from_aux_server(int client_fd, const char* filepath, int server_port, const char* server_name); // Forward files from auxiliary servers

// Download tar functions (downltar)
int s1_handle_downltar(int client_fd, char* command);                                   // Main handler for download tar archive requests
int send_local_tar_to_client(int client_fd, const char* filetype);                     // Create and send tar of local .c files
int forward_tar_from_aux_server(int client_fd, const char* filetype, int server_port, const char* server_name); // Forward tar archives from auxiliary servers

// Upload functions (uploadf)
int s1_handle_uploadf(int client_fd, char* command);                                    // Main handler for file upload requests
int send_to_server(const char* filepath, const char* dest_path, int server_port);      // Transfer files to appropriate auxiliary servers

// Remove functions (removef)
int s1_handle_removef(int client_fd, char* command);                                    // Main handler for file removal requests
int send_delete_request(const char* filepath, int server_port);                        // Send deletion requests to auxiliary servers

// Display functions (dispfnames)
int s1_handle_dispfnames(int client_fd, char* command);                                 // Main handler for directory listing requests
int get_files_from_server(int server_port, const char* server_path, char files[][256], int* count); // Get file lists from auxiliary servers
int get_local_c_files(const char* local_path, char files[][256], int* count);          // Get list of local .c files

int main() {
    int server_socket, client_socket;    // Socket file descriptors
    struct sockaddr_in server_addr, client_addr; // Socket address structures
    socklen_t client_len;               // Length of client address structure
    pid_t child_pid;                    // Process ID for forked child processes

    // Setup signal handler for zombie process cleanup
    setup_signal_handlers();

    // Create S1 directory if it doesn't exist - this is where .c files are stored locally
    char s1_dir[MAX_PATH_LEN];
    snprintf(s1_dir, sizeof(s1_dir), "%s/S1", getenv("HOME"));
    ensure_directory_exists(s1_dir);

    // Create server socket using TCP protocol
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        return 1;
    }

    // Allow socket reuse to avoid "Address already in use" errors
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Setup server address structure
    server_addr.sin_family = AF_INET;      // IPv4
    server_addr.sin_port = htons(S1_PORT); // Convert port to network byte order
    server_addr.sin_addr.s_addr = INADDR_ANY; // Accept connections from any interface

    // Bind socket to the specified address and port
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_socket);
        return 1;
    }

    // Listen for incoming connections (max 5 pending connections in queue)
    if (listen(server_socket, 5) < 0) {
        perror("Listen failed");
        close(server_socket);
        return 1;
    }

    // Server startup messages
    printf("S1 Server listening on port %d\n", S1_PORT);
    printf("Storage directory: %s\n", s1_dir);
    printf("Waiting for client connections...\n");

    // Main server loop - continuously accept and handle client connections
    while (1) {
        client_len = sizeof(client_addr);
        // Accept incoming client connection
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            perror("Accept failed");
            continue; // Continue accepting other connections
        }

        printf("New client connected from %s\n", inet_ntoa(client_addr.sin_addr));

        // Fork child process to handle client - allows concurrent client handling
        child_pid = fork();
        if (child_pid == 0) {
            // Child process code
            close(server_socket);  // Child doesn't need the listening socket
            prcclient(client_socket); // Process client requests
            exit(0); // Exit child process when client disconnects
        }
        else if (child_pid > 0) {
            // Parent process code
            close(client_socket);  // Parent doesn't need the client socket
            // Clean up zombie processes (non-blocking)
            while (waitpid(-1, NULL, WNOHANG) > 0);
        }
        else {
            // Fork failed
            perror("Fork failed");
            close(client_socket);
        }
    }

    close(server_socket);
    return 0;
}

// Process client requests in child process - handles all command parsing and routing
void prcclient(int client_fd) {
    char buffer[BUFFER_SIZE]; // Buffer to receive client commands
    int bytes_received;

    printf("Child process started for client (PID: %d)\n", getpid());

    // Main client processing loop - handle multiple commands per connection
    while (1) {
        // Receive command from client
        bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            printf("Client disconnected (PID: %d)\n", getpid());
            break; // Exit loop on client disconnect or error
        }

        buffer[bytes_received] = '\0'; // Null-terminate received data
        printf("Received command: %s (PID: %d)\n", buffer, getpid());

        // Parse and route commands to appropriate handlers
        if (strncmp(buffer, "downlf", 6) == 0) {
            s1_handle_downlf(client_fd, buffer);           // Handle file download requests
        }
        else if (strncmp(buffer, "downltar", 8) == 0) {
            s1_handle_downltar(client_fd, buffer);         // Handle tar archive download requests
        }
        else if (strncmp(buffer, "uploadf", 7) == 0) {
            s1_handle_uploadf(client_fd, buffer);          // Handle file upload requests
        }
        else if (strncmp(buffer, "removef", 7) == 0) {
            s1_handle_removef(client_fd, buffer);          // Handle file removal requests
        }
        else if (strncmp(buffer, "dispfnames", 10) == 0) {
            s1_handle_dispfnames(client_fd, buffer);       // Handle directory listing requests
        }
        else {
            // Handle unknown commands
            printf("Unknown command: %s\n", buffer);
            char error_response[] = "Error: Unknown command";
            send(client_fd, error_response, strlen(error_response), 0);
        }
    }

    close(client_fd); // Close client connection
}

// Configure signal handlers for proper process cleanup
void setup_signal_handlers() {
    signal(SIGCHLD, sigchld_handler); // Handle child process termination
}

// Signal handler for child process termination - prevents zombie processes
void sigchld_handler(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0); // Clean up all terminated child processes
}

// Create directory structure recursively if it doesn't exist
void ensure_directory_exists(const char* path) {
    char tmp[MAX_PATH_LEN]; // Temporary path buffer
    char* p = NULL;         // Pointer for path traversal
    size_t len;
    struct stat st = { 0 }; // File status structure

    // Check if directory already exists
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("Directory %s already exists\n", path);
        return;
    }

    // Copy path to temporary buffer for manipulation
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    // Remove trailing slash if present
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    // Create directories recursively by traversing path components
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0; // Temporarily null-terminate at this level
            // Create directory at this level if it doesn't exist
            if (stat(tmp, &st) == -1) {
                if (mkdir(tmp, 0755) == 0) {
                    printf("Created directory: %s\n", tmp);
                }
            }
            *p = '/'; // Restore slash
        }
    }

    // Create the final directory level
    if (stat(tmp, &st) == -1) {
        if (mkdir(tmp, 0755) == 0) {
            printf("Created directory: %s\n", tmp);
        }
        else {
            printf("Failed to create directory: %s (Error: %s)\n", tmp, strerror(errno));
        }
    }
}

// Comparison function for sorting strings alphabetically (used with qsort)
int string_compare(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

// Extract file extension from filename (returns pointer to extension including dot)
const char* get_file_extension(const char* filename) {
    const char* dot = strrchr(filename, '.'); // Find last occurrence of dot
    if (!dot || dot == filename) return "";   // Return empty string if no extension
    return dot; // Return pointer to extension including dot
}

// Determine which server should handle a file based on its extension
int get_server_for_file(const char* filepath) {
    const char* ext = get_file_extension(filepath);

    // File type to server mapping
    if (strcmp(ext, ".c") == 0) return 1;       // S1 (local) handles .c files
    if (strcmp(ext, ".pdf") == 0) return 2;     // S2 handles .pdf files
    if (strcmp(ext, ".txt") == 0) return 3;     // S3 handles .txt files
    if (strcmp(ext, ".zip") == 0) return 4;     // S4 handles .zip files

    return -1; // Invalid/unsupported extension
}

// Convert S1 path format (~S1/...) to auxiliary server path format (~S2/..., ~S3/..., etc.)
void convert_path_for_server(const char* s1_path, char* server_path, const char* server_name) {
    strcpy(server_path, "~");           // Start with tilde
    strcat(server_path, server_name);   // Add server name (S2, S3, or S4)
    strcat(server_path, s1_path + 3);   // Skip "~S1" and append the rest of the path
}

// Establish TCP connection to auxiliary servers (S2, S3, S4)
int connect_to_aux_server(int port) {
    int socket_fd;                    // Socket file descriptor
    struct sockaddr_in server_addr;   // Server address structure

    // Create socket
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    // Configure server address
    server_addr.sin_family = AF_INET;                      // IPv4
    server_addr.sin_port = htons(port);                    // Convert port to network byte order
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // Localhost (all servers on same machine)

    // Attempt connection to auxiliary server
    if (connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Warning: Connection to auxiliary server on port %d failed\n", port);
        close(socket_fd);
        return -1;
    }

    return socket_fd; // Return connected socket
}

//downlf

// Send a local .c file directly to the client
int send_local_file_to_client(int client_fd, const char* filepath) {
    FILE* file;           // File pointer for reading
    char buffer[BUFFER_SIZE]; // Data transfer buffer
    char local_path[MAX_PATH_LEN]; // Converted local file path
    long file_size;       // Size of file to send
    int bytes_read;       // Number of bytes read from file

    // Convert ~S1 path to actual local filesystem path
    snprintf(local_path, sizeof(local_path), "%s/S1%s", getenv("HOME"), filepath + 3);

    // Attempt to open file in binary mode
    file = fopen(local_path, "rb");
    if (!file) {
        // File not found - send error indicator to client
        file_size = -1;
        send(client_fd, &file_size, sizeof(file_size), 0);
        printf("File not found: %s\n", local_path);
        return 0;
    }

    // Get file size by seeking to end and getting position
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET); // Reset to beginning

    // Send file size to client first (protocol requirement)
    if (send(client_fd, &file_size, sizeof(file_size), 0) < 0) {
        fclose(file);
        return 0;
    }

    printf("Sending local file: %s (%ld bytes)\n", local_path, file_size);

    // Send file data in chunks
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (send(client_fd, buffer, bytes_read, 0) < 0) {
            fclose(file);
            return 0; // Send failed
        }
    }

    fclose(file);
    return 1; // Success
}

// Forward a file from auxiliary server to client (proxy functionality)
int forward_file_from_aux_server(int client_fd, const char* filepath, int server_port, const char* server_name) {
    int aux_socket;              // Socket to auxiliary server
    char aux_path[MAX_PATH_LEN]; // Path format for auxiliary server
    char command[BUFFER_SIZE];   // Command to send to auxiliary server
    char buffer[BUFFER_SIZE];    // Data transfer buffer
    long file_size;              // File size received from auxiliary server
    int bytes_received;          // Bytes received from auxiliary server

    // Connect to the appropriate auxiliary server
    aux_socket = connect_to_aux_server(server_port);
    if (aux_socket < 0) {
        // Connection failed - send error to client
        file_size = -1;
        send(client_fd, &file_size, sizeof(file_size), 0);
        return 0;
    }

    // Convert path format for auxiliary server
    convert_path_for_server(filepath, aux_path, server_name);

    // Send file request to auxiliary server
    snprintf(command, sizeof(command), "GET_FILE %s", aux_path);
    send(aux_socket, command, strlen(command), 0);

    // Receive file size from auxiliary server
    if (recv(aux_socket, &file_size, sizeof(file_size), 0) <= 0) {
        close(aux_socket);
        file_size = -1;
        send(client_fd, &file_size, sizeof(file_size), 0);
        return 0;
    }

    // Forward file size to client
    send(client_fd, &file_size, sizeof(file_size), 0);

    // Check if file was found on auxiliary server
    if (file_size == -1) {
        close(aux_socket);
        printf("File not found on %s server: %s\n", server_name, aux_path);
        return 0;
    }

    printf("Forwarding file from %s: %s (%ld bytes)\n", server_name, aux_path, file_size);

    // Forward file data from auxiliary server to client
    long bytes_remaining = file_size;
    while (bytes_remaining > 0) {
        // Calculate how much to read in this iteration
        int bytes_to_receive = (bytes_remaining < BUFFER_SIZE) ? bytes_remaining : BUFFER_SIZE;
        bytes_received = recv(aux_socket, buffer, bytes_to_receive, 0);

        if (bytes_received <= 0) break; // Error or connection closed

        // Forward received data to client
        send(client_fd, buffer, bytes_received, 0);
        bytes_remaining -= bytes_received;
    }

    close(aux_socket);
    return bytes_remaining == 0; // Return success if all bytes transferred
}

// Main handler for download file requests (downlf command)
int s1_handle_downlf(int client_fd, char* command) {
    char* token;        // Token for command parsing
    char* files[2];     // Array to store up to 2 file paths
    int file_count = 0; // Number of files to process

    // Parse command to extract filenames (skip "downlf")
    token = strtok(command, " ");  // Skip "downlf"
    token = strtok(NULL, " ");     // Get first filename

    // Extract all file paths from command (maximum 2 for downlf)
    while (token != NULL && file_count < 2) {
        files[file_count] = malloc(strlen(token) + 1); // Allocate memory for filename
        strcpy(files[file_count], token);              // Copy filename
        file_count++;
        token = strtok(NULL, " "); // Get next filename
    }

    printf("Processing downlf request for %d files\n", file_count);

    // Process each requested file
    for (int i = 0; i < file_count; i++) {
        int server_num = get_server_for_file(files[i]); // Determine which server handles this file type
        int success = 0;

        printf("Processing file: %s (server: %d)\n", files[i], server_num);

        // Route file request to appropriate server based on file extension
        switch (server_num) {
        case 1: // .c file - handle locally on S1
            success = send_local_file_to_client(client_fd, files[i]);
            break;
        case 2: // .pdf file - get from S2
            success = forward_file_from_aux_server(client_fd, files[i], S2_PORT, "S2");
            break;
        case 3: // .txt file - get from S3
            success = forward_file_from_aux_server(client_fd, files[i], S3_PORT, "S3");
            break;
        case 4: // .zip file - get from S4
            success = forward_file_from_aux_server(client_fd, files[i], S4_PORT, "S4");
            break;
        default:
            // Invalid file type - send error indicator
            long file_size = -1;
            send(client_fd, &file_size, sizeof(file_size), 0);
            printf("Invalid file type for: %s\n", files[i]);
            break;
        }
    }

    // Clean up allocated memory for filenames
    for (int i = 0; i < file_count; i++) {
        free(files[i]);
    }

    return 1;
}

//downltar

// Create and send a tar archive of local .c files
int send_local_tar_to_client(int client_fd, const char* filetype) {
    char tar_command[MAX_PATH_LEN]; // Command to create tar archive
    char tar_path[MAX_PATH_LEN];    // Path to created tar file
    FILE* tar_file;                 // File pointer for reading tar file
    char buffer[BUFFER_SIZE];       // Data transfer buffer
    long file_size;                 // Size of created tar file
    int bytes_read;                 // Bytes read from tar file

    // Define tar filename and full path
    const char* tar_filename = "cfiles.tar";
    snprintf(tar_path, sizeof(tar_path), "%s/S1/%s", getenv("HOME"), tar_filename);

    // Create tar command to find all .c files in S1 directory tree and archive them
    snprintf(tar_command, sizeof(tar_command),
        "cd %s/S1 && find . -name '*.c' -type f | tar -cf %s -T - 2>/dev/null",
        getenv("HOME"), tar_filename);

    printf("Creating tar file with command: %s\n", tar_command);

    // Execute tar command using system call
    int result = system(tar_command);
    if (result != 0) {
        printf("Warning: tar command returned %d\n", result);
    }

    // Open the created tar file for reading
    tar_file = fopen(tar_path, "rb");
    if (!tar_file) {
        // Failed to create or open tar file - send error to client
        file_size = -1;
        send(client_fd, &file_size, sizeof(file_size), 0);
        printf("Failed to create or open tar file: %s\n", tar_path);
        return 0;
    }

    // Get tar file size
    fseek(tar_file, 0, SEEK_END);
    file_size = ftell(tar_file);
    fseek(tar_file, 0, SEEK_SET);

    // Send file size to client
    if (send(client_fd, &file_size, sizeof(file_size), 0) < 0) {
        fclose(tar_file);
        return 0;
    }

    printf("Sending tar file: %s (%ld bytes)\n", tar_path, file_size);

    // Send tar file data to client
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, tar_file)) > 0) {
        if (send(client_fd, buffer, bytes_read, 0) < 0) {
            fclose(tar_file);
            return 0;
        }
    }

    fclose(tar_file);

    // Clean up - remove temporary tar file
    remove(tar_path);

    return 1;
}

// Forward a tar archive from auxiliary server to client
int forward_tar_from_aux_server(int client_fd, const char* filetype, int server_port, const char* server_name) {
    int aux_socket;           // Socket to auxiliary server
    char command[BUFFER_SIZE]; // Command to send to auxiliary server
    char buffer[BUFFER_SIZE];  // Data transfer buffer
    long file_size;           // Size of tar file from auxiliary server
    int bytes_received;       // Bytes received from auxiliary server

    // Connect to auxiliary server
    aux_socket = connect_to_aux_server(server_port);
    if (aux_socket < 0) {
        // Connection failed - send error to client
        file_size = -1;
        send(client_fd, &file_size, sizeof(file_size), 0);
        return 0;
    }

    // Send tar creation request to auxiliary server
    snprintf(command, sizeof(command), "CREATE_TAR %s", filetype);
    send(aux_socket, command, strlen(command), 0);

    // Receive tar file size from auxiliary server
    if (recv(aux_socket, &file_size, sizeof(file_size), 0) <= 0) {
        close(aux_socket);
        file_size = -1;
        send(client_fd, &file_size, sizeof(file_size), 0);
        return 0;
    }

    // Forward file size to client
    send(client_fd, &file_size, sizeof(file_size), 0);

    // Check if tar creation was successful
    if (file_size == -1) {
        close(aux_socket);
        printf("Failed to create tar on %s server\n", server_name);
        return 0;
    }

    printf("Forwarding tar file from %s: %s (%ld bytes)\n", server_name, filetype, file_size);

    // Forward tar file data from auxiliary server to client
    long bytes_remaining = file_size;
    while (bytes_remaining > 0) {
        int bytes_to_receive = (bytes_remaining < BUFFER_SIZE) ? bytes_remaining : BUFFER_SIZE;
        bytes_received = recv(aux_socket, buffer, bytes_to_receive, 0);

        if (bytes_received <= 0) break;

        // Forward data to client
        send(client_fd, buffer, bytes_received, 0);
        bytes_remaining -= bytes_received;
    }

    close(aux_socket);
    return bytes_remaining == 0;
}

// Main handler for download tar archive requests (downltar command)
int s1_handle_downltar(int client_fd, char* command) {
    char* token;    // Token for command parsing
    char* filetype; // File type for tar archive (.c, .pdf, .txt)

    // Parse command to extract filetype
    token = strtok(command, " ");  // Skip "downltar"
    filetype = strtok(NULL, " ");  // Get filetype

    if (!filetype) {
        printf("Invalid downltar command: missing filetype\n");
        long file_size = -1;
        send(client_fd, &file_size, sizeof(file_size), 0);
        return 0;
    }

    printf("Processing downltar request for filetype: %s\n", filetype);

    int success = 0;

    // Route tar request based on file type
    if (strcmp(filetype, ".c") == 0) {
        // Create tar of local .c files
        success = send_local_tar_to_client(client_fd, filetype);
    }
    else if (strcmp(filetype, ".pdf") == 0) {
        // Request tar from S2 server
        success = forward_tar_from_aux_server(client_fd, filetype, S2_PORT, "S2");
    }
    else if (strcmp(filetype, ".txt") == 0) {
        // Request tar from S3 server
        success = forward_tar_from_aux_server(client_fd, filetype, S3_PORT, "S3");
    }
    else {
        // Invalid filetype for tar operation
        printf("Invalid filetype for downltar: %s\n", filetype);
        long file_size = -1;
        send(client_fd, &file_size, sizeof(file_size), 0);
    }

    return success;
}

// uploadf

// Send a file to the appropriate auxiliary server based on file type
int send_to_server(const char* filepath, const char* dest_path, int server_port) {
    int sock;                         // Socket to auxiliary server
    struct sockaddr_in server_addr;   // Server address structure
    FILE* file;                       // File pointer for reading file to send
    char buffer[BUFFER_SIZE];         // Data transfer buffer
    size_t bytes_read;                // Bytes read from file

    printf("Attempting to connect to server on port %d\n", server_port);

    // Create socket for connection to auxiliary server
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    // Setup auxiliary server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Connect to auxiliary server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Warning: Cannot connect to server on port %d (server may not be running)\n", server_port);
        close(sock);
        return -1;
    }

    printf("Connected to server on port %d\n", server_port);

    // Send destination path with length prefix (protocol requirement)
    int dest_len = strlen(dest_path);
    if (send(sock, &dest_len, sizeof(dest_len), 0) != sizeof(dest_len)) {
        printf("Error: Failed to send destination path length\n");
        close(sock);
        return -1;
    }

    if (send(sock, dest_path, dest_len, 0) != dest_len) {
        printf("Error: Failed to send complete destination path\n");
        close(sock);
        return -1;
    }
    printf("Sent destination path: '%s' (%d bytes)\n", dest_path, dest_len);

    // Extract filename properly and send with length prefix
    char filepath_copy[MAX_PATH_LEN];
    strncpy(filepath_copy, filepath, sizeof(filepath_copy) - 1);
    filepath_copy[sizeof(filepath_copy) - 1] = '\0';

    // Use basename to extract just the filename from full path
    char* filename = basename(filepath_copy);
    if (!filename || strlen(filename) == 0) {
        printf("Error: Could not extract filename from path '%s'\n", filepath);
        close(sock);
        return -1;
    }

    // Send filename length and filename to auxiliary server
    int filename_len = strlen(filename);
    if (send(sock, &filename_len, sizeof(filename_len), 0) != sizeof(filename_len)) {
        printf("Error: Failed to send filename length\n");
        close(sock);
        return -1;
    }

    if (send(sock, filename, filename_len, 0) != filename_len) {
        printf("Error: Failed to send complete filename\n");
        close(sock);
        return -1;
    }
    printf("Sent filename: '%s' (%d bytes)\n", filename, filename_len);

    // Open file and get size for transfer
    file = fopen(filepath, "rb");
    if (!file) {
        perror("File open failed");
        close(sock);
        return -1;
    }

    // Calculate file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    printf("File size: %ld bytes\n", file_size);

    // Send file size to auxiliary server
    if (send(sock, &file_size, sizeof(file_size), 0) != sizeof(file_size)) {
        printf("Error: Failed to send file size\n");
        fclose(file);
        close(sock);
        return -1;
    }
    printf("Sent file size: %ld bytes\n", file_size);

    // Send file content in chunks
    long total_sent = 0;
    printf("Starting file transfer...\n");

    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        int bytes_sent = send(sock, buffer, bytes_read, 0);
        if (bytes_sent != bytes_read) {
            printf("Error: Failed to send file data\n");
            fclose(file);
            close(sock);
            return -1;
        }
        total_sent += bytes_sent;

        // Show progress for large files (> 1MB)
        if (file_size > 1024 * 1024) {
            printf("Sent: %.1f%% (%ld/%ld bytes)\r",
                (double)total_sent / file_size * 100, total_sent, file_size);
            fflush(stdout);
        }
    }

    fclose(file);
    close(sock);

    // Verify complete transfer
    if (total_sent == file_size) {
        printf("\nSUCCESS: File '%s' sent completely (%ld bytes)\n", filename, total_sent);
        return 0;
    }
    else {
        printf("\nERROR: File transfer incomplete (sent %ld/%ld bytes)\n", total_sent, file_size);
        return -1;
    }
}

// Main handler for file upload requests (uploadf command)
int s1_handle_uploadf(int client_fd, char* command) {
    char command_copy[BUFFER_SIZE]; // Copy of command for parsing
    strcpy(command_copy, command);

    char* token;         // Token for command parsing
    char files[3][256];  // Array to store up to 3 filenames
    int file_count = 0;  // Number of files to upload
    char* actual_dest = NULL; // Destination directory path

    // Parse command: uploadf file1 [file2] [file3] destination
    token = strtok(command_copy, " "); // Skip "uploadf"

    // Collect arguments (files and destination)
    char args[4][256];  // Maximum 4 arguments (3 files + 1 destination)
    int arg_count = 0;

    while ((token = strtok(NULL, " ")) != NULL && arg_count < 4) {
        strcpy(args[arg_count], token);
        arg_count++;
    }

    // Determine files and destination based on number of arguments
    // Last argument starting with ~ is the destination path
    if (arg_count >= 2) {
        if (args[arg_count - 1][0] == '~') {  // Last argument is destination
            actual_dest = args[arg_count - 1];
            file_count = arg_count - 1;       // All other arguments are files
            for (int i = 0; i < file_count; i++) {
                strcpy(files[i], args[i]);
            }
        }
    }

    // Validate command format
    if (!actual_dest || file_count == 0 || file_count > 3) {
        printf("Error: Invalid uploadf command format\n");
        char error_response[] = "Error: Invalid command format";
        send(client_fd, error_response, strlen(error_response), 0);
        return 0;
    }

    printf("Files to process: %d\n", file_count);
    printf("Destination: '%s'\n", actual_dest);

    // Convert ~S1 path to absolute filesystem path
    char final_dest[MAX_PATH_LEN];
    if (strncmp(actual_dest, "~S1", 3) == 0) {
        snprintf(final_dest, sizeof(final_dest), "%s/S1%s", getenv("HOME"), actual_dest + 3);
    }
    else {
        strcpy(final_dest, actual_dest);
    }

    printf("Final destination path: '%s'\n", final_dest);

    // Ensure destination directory exists before receiving files
    ensure_directory_exists(final_dest);

    // Process each uploaded file
    int files_processed = 0;
    for (int i = 0; i < file_count; i++) {
        char filename[256];
        strcpy(filename, files[i]);

        printf("\n=== Processing file %d: '%s' ===\n", i + 1, filename);

        // Receive file size from client
        long file_size;
        int size_received = recv(client_fd, &file_size, sizeof(file_size), MSG_WAITALL);
        if (size_received != sizeof(file_size)) {
            printf("Error receiving file size for %s\n", filename);
            continue; // Skip this file
        }

        printf("Expecting file size: %ld bytes\n", file_size);

        // Create full file path for saving
        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", final_dest, filename);
        printf("Full path: '%s'\n", full_path);

        // Create and open file for writing
        FILE* file = fopen(full_path, "wb");
        if (!file) {
            printf("ERROR: Cannot create file '%s' - %s\n", full_path, strerror(errno));

            // Skip this file's data to keep protocol in sync
            char skip_buffer[BUFFER_SIZE];
            long remaining = file_size;
            while (remaining > 0) {
                int to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
                int received = recv(client_fd, skip_buffer, to_read, 0);
                if (received > 0) remaining -= received;
                else break;
            }
            continue;
        }

        // Receive file content from client
        char file_buffer[BUFFER_SIZE];
        long remaining = file_size;
        long total_received = 0;

        printf("Starting to receive file data...\n");
        while (remaining > 0) {
            // Receive data in chunks
            int to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
            int received = recv(client_fd, file_buffer, to_read, 0);

            if (received > 0) {
                // Write received data to file
                size_t written = fwrite(file_buffer, 1, received, file);
                if (written != received) {
                    printf("Error writing to file\n");
                    break;
                }
                remaining -= received;
                total_received += received;
            }
            else if (received == 0) {
                printf("Connection closed while receiving file\n");
                break;
            }
            else {
                printf("Error receiving file data: %s\n", strerror(errno));
                break;
            }
        }
        fclose(file);

        printf("File received: %ld bytes total\n", total_received);

        // Verify complete file transfer
        if (total_received == file_size) {
            printf("SUCCESS: File '%s' saved successfully\n", filename);
            files_processed++;

            // Route file to appropriate server based on file extension
            const char* ext = get_file_extension(filename);
            printf("File extension: '%s'\n", ext);

            if (strcmp(ext, ".pdf") == 0) {
                // Transfer .pdf files to S2 server
                char s2_dest[MAX_PATH_LEN];
                snprintf(s2_dest, sizeof(s2_dest), "%s/S2%s", getenv("HOME"), actual_dest + 3);
                if (send_to_server(full_path, s2_dest, S2_PORT) == 0) {
                    unlink(full_path); // Remove from S1 after successful transfer
                    printf("Transferred %s to S2 and removed from S1\n", filename);
                }
                else {
                    printf("Failed to transfer %s to S2, keeping in S1\n", filename);
                }
            }
            else if (strcmp(ext, ".txt") == 0) {
                // Transfer .txt files to S3 server
                char s3_dest[MAX_PATH_LEN];
                snprintf(s3_dest, sizeof(s3_dest), "%s/S3%s", getenv("HOME"), actual_dest + 3);
                if (send_to_server(full_path, s3_dest, S3_PORT) == 0) {
                    unlink(full_path); // Remove from S1 after successful transfer
                    printf("Transferred %s to S3 and removed from S1\n", filename);
                }
                else {
                    printf("Failed to transfer %s to S3, keeping in S1\n", filename);
                }
            }
            else if (strcmp(ext, ".zip") == 0) {
                // Transfer .zip files to S4 server
                char s4_dest[MAX_PATH_LEN];
                snprintf(s4_dest, sizeof(s4_dest), "%s/S4%s", getenv("HOME"), actual_dest + 3);
                if (send_to_server(full_path, s4_dest, S4_PORT) == 0) {
                    unlink(full_path); // Remove from S1 after successful transfer
                    printf("Transferred %s to S4 and removed from S1\n", filename);
                }
                else {
                    printf("Failed to transfer %s to S4, keeping in S1\n", filename);
                }
            }
            else if (strcmp(ext, ".c") == 0) {
                // .c files remain on S1 server
                printf("SUCCESS: C file '%s' stored in S1\n", filename);
            }
            else {
                // Unknown file types default to S1 storage
                printf("Unknown file type '%s', stored in S1\n", filename);
            }
        }
        else {
            printf("ERROR: File transfer incomplete\n");
        }

        printf("=== End processing file %d ===\n\n", i + 1);
    }

    // Send response to client with upload results
    char response[256];
    snprintf(response, sizeof(response), "Successfully processed %d out of %d files", files_processed, file_count);
    send(client_fd, response, strlen(response), 0);

    return 1;
}

// removef

// Send delete request to auxiliary server
int send_delete_request(const char* filepath, int server_port) {
    int sock;                         // Socket to auxiliary server
    struct sockaddr_in server_addr;   // Server address structure
    char response[BUFFER_SIZE];       // Response buffer from server

    printf("Sending delete request to server on port %d for file: %s\n", server_port, filepath);

    // Create socket for connection to auxiliary server
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    // Setup auxiliary server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Connect to auxiliary server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Warning: Cannot connect to server on port %d (server may not be running)\n", server_port);
        close(sock);
        return -1;
    }

    printf("Connected to server on port %d\n", server_port);

    // Send delete command to auxiliary server
    char delete_cmd[MAX_PATH_LEN + 20];
    snprintf(delete_cmd, sizeof(delete_cmd), "DELETE %s", filepath);

    if (send(sock, delete_cmd, strlen(delete_cmd), 0) < 0) {
        printf("Error: Failed to send delete command\n");
        close(sock);
        return -1;
    }

    printf("Sent delete command: %s\n", delete_cmd);

    // Wait for response from auxiliary server
    memset(response, 0, sizeof(response));
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        printf("Server response: %s\n", response);
    }

    close(sock);
    return 0;
}

// Main handler for file removal requests (removef command)
int s1_handle_removef(int client_fd, char* command) {
    char command_copy[BUFFER_SIZE]; // Copy of command for parsing
    strcpy(command_copy, command);

    char* token;                    // Token for command parsing
    char files[2][MAX_PATH_LEN];    // Array to store up to 2 file paths
    int file_count = 0;             // Number of files to remove

    // Parse command to extract filenames (skip "removef")
    token = strtok(command_copy, " ");  // Skip "removef"

    while ((token = strtok(NULL, " ")) != NULL && file_count < 2) {
        strcpy(files[file_count], token);
        file_count++;
    }

    printf("Files to remove: %d\n", file_count);

    int files_removed = 0;               // Counter for successfully processed files
    char response_msg[BUFFER_SIZE] = ""; // Response message to build

    // Process each file removal request
    for (int i = 0; i < file_count; i++) {
        printf("\n=== Processing remove request for file: '%s' ===\n", files[i]);

        // Convert ~S1 path to absolute filesystem path
        char absolute_path[MAX_PATH_LEN];
        if (strncmp(files[i], "~S1", 3) == 0) {
            snprintf(absolute_path, sizeof(absolute_path), "%s/S1%s", getenv("HOME"), files[i] + 3);
        }
        else {
            strcpy(absolute_path, files[i]);
        }

        printf("Absolute path: '%s'\n", absolute_path);

        // Get file extension to determine which server handles this file type
        const char* ext = get_file_extension(files[i]);
        printf("File extension: '%s'\n", ext);

        if (strcmp(ext, ".c") == 0) {
            // For .c files, check if file exists locally in S1 and delete it
            if (access(absolute_path, F_OK) != 0) {
                printf("Error: File '%s' not found in S1\n", files[i]);
                strcat(response_msg, "File not found in S1: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
                continue;
            }
            // Delete .c file locally in S1
            if (unlink(absolute_path) == 0) {
                printf("SUCCESS: Deleted .c file '%s' from S1\n", files[i]);
                files_removed++;
                strcat(response_msg, "Deleted from S1: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
            }
            else {
                printf("Error: Failed to delete file '%s' - %s\n", files[i], strerror(errno));
                strcat(response_msg, "Failed to delete: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
            }
        }
        else if (strcmp(ext, ".pdf") == 0) {
            // For .pdf files, send delete request to S2 server
            char s2_path[MAX_PATH_LEN];
            snprintf(s2_path, sizeof(s2_path), "%s/S2%s", getenv("HOME"), files[i] + 3);

            printf("Sending delete request to S2 for: %s\n", s2_path);
            if (send_delete_request(s2_path, S2_PORT) == 0) {
                printf("SUCCESS: Delete request sent to S2 for file '%s'\n", files[i]);
                files_removed++;
                strcat(response_msg, "Delete request sent to S2: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
            }
            else {
                printf("Error: Failed to send delete request to S2 for file '%s'\n", files[i]);
                strcat(response_msg, "Failed to contact S2 for: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
            }
        }
        else if (strcmp(ext, ".txt") == 0) {
            // For .txt files, send delete request to S3 server
            char s3_path[MAX_PATH_LEN];
            snprintf(s3_path, sizeof(s3_path), "%s/S3%s", getenv("HOME"), files[i] + 3);

            printf("Sending delete request to S3 for: %s\n", s3_path);
            if (send_delete_request(s3_path, S3_PORT) == 0) {
                printf("SUCCESS: Delete request sent to S3 for file '%s'\n", files[i]);
                files_removed++;
                strcat(response_msg, "Delete request sent to S3: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
            }
            else {
                printf("Error: Failed to send delete request to S3 for file '%s'\n", files[i]);
                strcat(response_msg, "Failed to contact S3 for: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
            }
        }
        else if (strcmp(ext, ".zip") == 0) {
            // For .zip files, send delete request to S4 server
            char s4_path[MAX_PATH_LEN];
            snprintf(s4_path, sizeof(s4_path), "%s/S4%s", getenv("HOME"), files[i] + 3);

            if (send_delete_request(s4_path, S4_PORT) == 0) {
                printf("SUCCESS: Delete request sent to S4 for file '%s'\n", files[i]);
                files_removed++;
                strcat(response_msg, "Delete request sent to S4: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
            }
            else {
                printf("Error: Failed to send delete request to S4 for file '%s'\n", files[i]);
                strcat(response_msg, "Failed to contact S4 for: ");
                strcat(response_msg, files[i]);
                strcat(response_msg, "; ");
            }
        }
        else {
            // Handle unsupported file types
            printf("Error: Unsupported file type '%s' for file '%s'\n", ext, files[i]);
            strcat(response_msg, "Unsupported file type: ");
            strcat(response_msg, files[i]);
            strcat(response_msg, "; ");
        }

        printf("=== End processing remove request for file ===\n\n");
    }

    // Send response to client with removal results
    char final_response[BUFFER_SIZE];
    snprintf(final_response, sizeof(final_response), "Remove operation completed. Processed %d out of %d files. %s",
        files_removed, file_count, response_msg);
    send(client_fd, final_response, strlen(final_response), 0);

    return 1;
}

// dispfnames

// Get file list from auxiliary server
int get_files_from_server(int server_port, const char* server_path, char files[][256], int* count) {
    int sock;                         // Socket to auxiliary server
    struct sockaddr_in server_addr;   // Server address structure
    char list_cmd[MAX_PATH_LEN + 20]; // Command to send to server
    char response[BUFFER_SIZE * 4];   // Response buffer (large for file lists)

    printf("Requesting file list from server on port %d for path: %s\n", server_port, server_path);

    *count = 0; // Initialize file count

    // Create socket for connection to auxiliary server
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Warning: Cannot create socket for server on port %d\n", server_port);
        return -1;
    }

    // Setup auxiliary server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Connect to auxiliary server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Warning: Cannot connect to server on port %d (server may not be running)\n", server_port);
        close(sock);
        return -1;
    }

    printf("Connected to server on port %d\n", server_port);

    // Send list command to auxiliary server
    snprintf(list_cmd, sizeof(list_cmd), "LIST %s", server_path);

    if (send(sock, list_cmd, strlen(list_cmd), 0) < 0) {
        printf("Error: Failed to send list command\n");
        close(sock);
        return -1;
    }

    printf("Sent list command: %s\n", list_cmd);

    // Wait for response containing file list
    memset(response, 0, sizeof(response));
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        printf("Server response (%d bytes): %s\n", bytes, response);

        // Parse the response to extract filenames
        char* token = strtok(response, "\n");
        while (token != NULL && *count < MAX_FILES) {
            // Skip empty lines and status messages
            if (strlen(token) > 0 && strstr(token, "SUCCESS") == NULL &&
                strstr(token, "ERROR") == NULL && strstr(token, "Files found") == NULL) {
                strncpy(files[*count], token, 255);
                files[*count][255] = '\0';
                (*count)++;
                printf("Added file to list: %s\n", token);
            }
            token = strtok(NULL, "\n");
        }
    }

    close(sock);
    return 0;
}

// Get list of local .c files from S1 directory
int get_local_c_files(const char* local_path, char files[][256], int* count) {
    DIR* dir;           // Directory pointer
    struct dirent* entry; // Directory entry structure

    *count = 0; // Initialize file count

    printf("Scanning local directory: %s\n", local_path);

    // Open local directory
    dir = opendir(local_path);
    if (!dir) {
        printf("Warning: Cannot open local directory %s - %s\n", local_path, strerror(errno));
        return -1;
    }

    // Read directory entries and filter for .c files
    while ((entry = readdir(dir)) != NULL && *count < MAX_FILES) {
        if (entry->d_type == DT_REG) { // Regular file (not directory)
            const char* ext = get_file_extension(entry->d_name);
            if (strcmp(ext, ".c") == 0) { // Only include .c files
                strncpy(files[*count], entry->d_name, 255);
                files[*count][255] = '\0';
                (*count)++;
                printf("Found local .c file: %s\n", entry->d_name);
            }
        }
    }

    closedir(dir);
    return 0;
}

// Main handler for directory listing requests (dispfnames command)
int s1_handle_dispfnames(int client_fd, char* command) {
    char command_copy[BUFFER_SIZE]; // Copy of command for parsing
    strcpy(command_copy, command);

    char* token;    // Token for command parsing
    char* pathname; // Directory path to list

    // Parse command to extract pathname (skip "dispfnames")
    token = strtok(command_copy, " ");  // Skip "dispfnames"
    pathname = strtok(NULL, " ");       // Get pathname

    if (!pathname) {
        printf("Invalid dispfnames command: missing pathname\n");
        char error_response[] = "Error: Path must be specified";
        send(client_fd, error_response, strlen(error_response), 0);
        return 0;
    }

    printf("\n=== Processing dispfnames command for path: '%s' ===\n", pathname);

    // Validate path format (must start with ~S1)
    if (strncmp(pathname, "~S1", 3) != 0) {
        char error_response[] = "Error: Path must start with ~S1";
        send(client_fd, error_response, strlen(error_response), 0);
        return 0;
    }

    // Convert ~S1 path to absolute filesystem path
    char absolute_path[MAX_PATH_LEN];
    snprintf(absolute_path, sizeof(absolute_path), "%s/S1%s", getenv("HOME"), pathname + 3);
    printf("Absolute local path: '%s'\n", absolute_path);

    // Check if directory exists locally
    struct stat st;
    if (stat(absolute_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char error_response[] = "Error: Directory not found in S1";
        send(client_fd, error_response, strlen(error_response), 0);
        return 0;
    }

    // Arrays to store filenames by file type
    char c_files[MAX_FILES][256];   // Local .c files
    char pdf_files[MAX_FILES][256]; // .pdf files from S2
    char txt_files[MAX_FILES][256]; // .txt files from S3
    char zip_files[MAX_FILES][256]; // .zip files from S4
    int c_count = 0, pdf_count = 0, txt_count = 0, zip_count = 0;

    // Get local .c files from S1
    printf("Getting local .c files...\n");
    get_local_c_files(absolute_path, c_files, &c_count);

    // Get .pdf files from S2 server
    printf("Getting .pdf files from S2...\n");
    char s2_path[MAX_PATH_LEN];
    snprintf(s2_path, sizeof(s2_path), "%s/S2%s", getenv("HOME"), pathname + 3);
    get_files_from_server(S2_PORT, s2_path, pdf_files, &pdf_count);

    // Get .txt files from S3 server
    printf("Getting .txt files from S3...\n");
    char s3_path[MAX_PATH_LEN];
    snprintf(s3_path, sizeof(s3_path), "%s/S3%s", getenv("HOME"), pathname + 3);
    get_files_from_server(S3_PORT, s3_path, txt_files, &txt_count);

    // Get .zip files from S4 server
    printf("Getting .zip files from S4...\n");
    char s4_path[MAX_PATH_LEN];
    snprintf(s4_path, sizeof(s4_path), "%s/S4%s", getenv("HOME"), pathname + 3);
    get_files_from_server(S4_PORT, s4_path, zip_files, &zip_count);

    // Sort files alphabetically within each type using qsort
    if (c_count > 0) {
        char* c_ptrs[MAX_FILES];
        for (int i = 0; i < c_count; i++) c_ptrs[i] = c_files[i];
        qsort(c_ptrs, c_count, sizeof(char*), string_compare);
        for (int i = 0; i < c_count; i++) strcpy(c_files[i], c_ptrs[i]);
    }

    if (pdf_count > 0) {
        char* pdf_ptrs[MAX_FILES];
        for (int i = 0; i < pdf_count; i++) pdf_ptrs[i] = pdf_files[i];
        qsort(pdf_ptrs, pdf_count, sizeof(char*), string_compare);
        for (int i = 0; i < pdf_count; i++) strcpy(pdf_files[i], pdf_ptrs[i]);
    }

    if (txt_count > 0) {
        char* txt_ptrs[MAX_FILES];
        for (int i = 0; i < txt_count; i++) txt_ptrs[i] = txt_files[i];
        qsort(txt_ptrs, txt_count, sizeof(char*), string_compare);
        for (int i = 0; i < txt_count; i++) strcpy(txt_files[i], txt_ptrs[i]);
    }

    if (zip_count > 0) {
        char* zip_ptrs[MAX_FILES];
        for (int i = 0; i < zip_count; i++) zip_ptrs[i] = zip_files[i];
        qsort(zip_ptrs, zip_count, sizeof(char*), string_compare);
        for (int i = 0; i < zip_count; i++) strcpy(zip_files[i], zip_ptrs[i]);
    }

    // Build consolidated response with file listing
    char response[BUFFER_SIZE * 4] = ""; // Large buffer for file list
    int total_files = c_count + pdf_count + txt_count + zip_count;

    if (total_files == 0) {
        // No files found in any server
        strcpy(response, "No files found in the specified directory");
    }
    else {
        // Build response with file count summary
        char temp[256];
        snprintf(temp, sizeof(temp), "Files found: %d (.c: %d, .pdf: %d, .txt: %d, .zip: %d)\n",
            total_files, c_count, pdf_count, txt_count, zip_count);
        strcat(response, temp);

        // Add .c files first (as per requirement - local files have priority)
        for (int i = 0; i < c_count; i++) {
            strcat(response, c_files[i]);
            strcat(response, "\n");
        }

        // Add .pdf files from S2
        for (int i = 0; i < pdf_count; i++) {
            strcat(response, pdf_files[i]);
            strcat(response, "\n");
        }

        // Add .txt files from S3
        for (int i = 0; i < txt_count; i++) {
            strcat(response, txt_files[i]);
            strcat(response, "\n");
        }

        // Add .zip files from S4
        for (int i = 0; i < zip_count; i++) {
            strcat(response, zip_files[i]);
            strcat(response, "\n");
        }
    }

    // Send complete file listing response to client
    printf("Sending file list response (%zu bytes)\n", strlen(response));
    send(client_fd, response, strlen(response), 0);

    printf("=== dispfnames command completed ===\n\n");

    return 1;
}
