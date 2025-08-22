#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <errno.h>

// Define constants for buffer sizes and limits
#define BUFFER_SIZE 8192          // Standard buffer size for file operations
#define MAX_PATH_LEN 1024         // Maximum length for file paths
#define S2_PORT 8001              // Port number for S2 server to listen on
#define MAX_FILES 1000            // Maximum number of files to handle in listings

 // Function prototypes - declaring all functions used in the program
void handle_s1_request(int s1_socket);                                          // Main request handler from S1
void send_file_to_s1(int s1_socket, const char* filepath);                      // Handles GET_FILE requests
void create_and_send_tar_to_s1(int s1_socket, const char* filetype);           // Handles CREATE_TAR requests
void handle_delete_request(int s1_socket, const char* filepath);               // Handles DELETE requests
void handle_list_request(int s1_socket, const char* pathname);                 // Handles LIST requests
void handle_upload_request(int s1_socket, char* initial_buffer, int initial_bytes); // Handles file upload requests
void ensure_directory_exists(const char* path);                                // Utility to create directories
const char* get_file_extension(const char* filename);                          // Utility to get file extension

int main() {
    int server_socket, s1_socket;    // Socket file descriptors
    struct sockaddr_in server_addr;  // Server address structure

    printf("Starting Integrated S2 Server (PDF files)...\n");

    // Create S2 directory if it doesn't exist - this is where all PDF files will be stored
    char s2_dir[MAX_PATH_LEN];
    snprintf(s2_dir, sizeof(s2_dir), "%s/S2", getenv("HOME")); // Create path: ~/S2
    ensure_directory_exists(s2_dir);

    // Create server socket using TCP (SOCK_STREAM)
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("S2: Socket creation failed");
        return 1;
    }

    // Allow socket reuse to prevent "Address already in use" errors
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Setup server address structure for binding
    server_addr.sin_family = AF_INET;          // IPv4
    server_addr.sin_port = htons(S2_PORT);     // Convert port to network byte order
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all available interfaces

    // Bind socket to the specified address and port
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("S2: Bind failed");
        close(server_socket);
        return 1;
    }

    // Start listening for incoming connections (queue up to 5 connections)
    if (listen(server_socket, 5) < 0) {
        perror("S2: Listen failed");
        close(server_socket);
        return 1;
    }

    // Print server startup information
    printf("S2 Server (PDF files) listening on port %d\n", S2_PORT);
    printf("Storage directory: %s\n", s2_dir);
    printf("Supports: File uploads, GET_FILE, CREATE_TAR, DELETE, LIST commands\n");
    printf("Waiting for requests from S1...\n");

    // Main server loop - accept and handle connections
    while (1) {
        // Accept incoming connection from S1
        s1_socket = accept(server_socket, NULL, NULL);
        if (s1_socket < 0) {
            perror("S2: Accept failed");
            continue; // Continue accepting other connections
        }

        printf("S2: Received connection from S1\n");

        // Fork child process to handle request - allows concurrent handling
        pid_t pid = fork();
        if (pid == 0) {
            // Child process - handle the client request
            close(server_socket);     // Child doesn't need the listening socket
            handle_s1_request(s1_socket); // Process the request
            close(s1_socket);         // Close client socket when done
            exit(0);                  // Exit child process
        }
        else if (pid > 0) {
            // Parent process - continue accepting new connections
            close(s1_socket);         // Parent doesn't need this client socket
            // Clean up zombie processes (children that have finished)
            while (waitpid(-1, NULL, WNOHANG) > 0);
        }
        else {
            perror("S2: Fork failed"); // Fork failed, but continue server operation
        }
    }

    close(server_socket); // This line never reached in normal operation
    return 0;
}

// Main function to handle incoming requests from S1 server
void handle_s1_request(int s1_socket) {
    char buffer[BUFFER_SIZE];           // Buffer to receive command data
    char command[256], parameter[MAX_PATH_LEN]; // Command parsing variables
    int bytes_received;

    // Receive command from S1
    bytes_received = recv(s1_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received <= 0) {
        printf("S2: Failed to receive command from S1\n");
        return;
    }

    buffer[bytes_received] = '\0'; // Null-terminate the received string
    printf("S2: Received command: %s\n", buffer);

    // Parse and handle different command types
    if (strncmp(buffer, "DELETE ", 7) == 0) {
        // DELETE command format: "DELETE /path/to/file"
        char filepath[MAX_PATH_LEN];
        strcpy(filepath, buffer + 7); // Skip "DELETE " prefix
        handle_delete_request(s1_socket, filepath);
    }
    else if (strncmp(buffer, "LIST ", 5) == 0) {
        // LIST command format: "LIST /path/to/directory"
        char pathname[MAX_PATH_LEN];
        strcpy(pathname, buffer + 5); // Skip "LIST " prefix
        handle_list_request(s1_socket, pathname);
    }
    else if (sscanf(buffer, "%s %s", command, parameter) == 2) {
        // Two-parameter commands (command + parameter)
        if (strcmp(command, "GET_FILE") == 0) {
            // GET_FILE command - send requested file to S1
            send_file_to_s1(s1_socket, parameter);
        }
        else if (strcmp(command, "CREATE_TAR") == 0) {
            // CREATE_TAR command - create tar archive and send to S1
            create_and_send_tar_to_s1(s1_socket, parameter);
        }
        else {
            printf("S2: Unknown command: %s\n", command);
        }
    }
    else {
        // Check if this is a file upload (no command prefix)
        // File upload starts with destination path length as binary data
        int dest_len;
        if (bytes_received >= sizeof(dest_len)) {
            memcpy(&dest_len, buffer, sizeof(dest_len)); // Extract length from binary data
            if (dest_len > 0 && dest_len < MAX_PATH_LEN) {
                // This looks like a file upload, handle it
                printf("S2: Detected file upload request\n");
                handle_upload_request(s1_socket, buffer, bytes_received);
                return;
            }
        }
        printf("S2: Invalid command format: %s\n", buffer);
    }
}

// Handle GET_FILE requests - send requested file back to S1
void send_file_to_s1(int s1_socket, const char* filepath) {
    FILE* file;                    // File pointer for reading
    char buffer[BUFFER_SIZE];      // Buffer for file data transfer
    char local_path[MAX_PATH_LEN]; // Actual local file system path
    long file_size;                // Size of file to send
    int bytes_read;                // Bytes read from file

    // Convert server path to actual local path
    // Example: ~S2/folder/file.pdf -> /home/user/S2/folder/file.pdf
    snprintf(local_path, sizeof(local_path), "%s/S2%s", getenv("HOME"), filepath + 3);

    printf("S2: Looking for file: %s\n", local_path);

    // Try to open the requested file
    file = fopen(local_path, "rb"); // Open in binary read mode
    if (!file) {
        // Send -1 to indicate file not found
        file_size = -1;
        send(s1_socket, &file_size, sizeof(file_size), 0);
        printf("S2: File not found: %s\n", local_path);
        return;
    }

    // Get file size by seeking to end and getting position
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET); // Reset to beginning of file

    // Send file size to S1 first (protocol requirement)
    if (send(s1_socket, &file_size, sizeof(file_size), 0) < 0) {
        printf("S2: Failed to send file size\n");
        fclose(file);
        return;
    }

    printf("S2: Sending file: %s (%ld bytes)\n", local_path, file_size);

    // Send file data to S1 in chunks
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (send(s1_socket, buffer, bytes_read, 0) < 0) {
            printf("S2: Failed to send file data\n");
            break;
        }
    }

    fclose(file);
    printf("S2: File sent successfully\n");
}

// Handle CREATE_TAR requests - create tar archive of all PDF files and send to S1
void create_and_send_tar_to_s1(int s1_socket, const char* filetype) {
    char tar_command[MAX_PATH_LEN];  // Command to create tar file
    char tar_path[MAX_PATH_LEN];     // Path to created tar file
    FILE* tar_file;                  // File pointer for reading tar
    char buffer[BUFFER_SIZE];        // Buffer for data transfer
    long file_size;                  // Size of tar file
    int bytes_read;                  // Bytes read from tar file

    // Create tar file name - always "pdf.tar" for PDF files
    const char* tar_filename = "pdf.tar";
    snprintf(tar_path, sizeof(tar_path), "%s/S2/%s", getenv("HOME"), tar_filename);

    // Create tar command to find all .pdf files in S2 directory tree
    // Uses find to locate all .pdf files, then pipes to tar command
    snprintf(tar_command, sizeof(tar_command),
        "cd %s/S2 && find . -name '*.pdf' -type f | tar -cf %s -T - 2>/dev/null",
        getenv("HOME"), tar_filename);

    printf("S2: Creating tar file with command: %s\n", tar_command);

    // Execute tar command using system() call
    int result = system(tar_command);
    if (result != 0) {
        printf("S2: Warning: tar command returned %d\n", result);
    }

    // Open created tar file for reading
    tar_file = fopen(tar_path, "rb");
    if (!tar_file) {
        // Send -1 to indicate failure
        file_size = -1;
        send(s1_socket, &file_size, sizeof(file_size), 0);
        printf("S2: Failed to create or open tar file: %s\n", tar_path);
        return;
    }

    // Get tar file size
    fseek(tar_file, 0, SEEK_END);
    file_size = ftell(tar_file);
    fseek(tar_file, 0, SEEK_SET);

    // Send file size to S1 first (protocol requirement)
    if (send(s1_socket, &file_size, sizeof(file_size), 0) < 0) {
        printf("S2: Failed to send tar file size\n");
        fclose(tar_file);
        return;
    }

    printf("S2: Sending tar file: %s (%ld bytes)\n", tar_path, file_size);

    // Send tar file data to S1 in chunks
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, tar_file)) > 0) {
        if (send(s1_socket, buffer, bytes_read, 0) < 0) {
            printf("S2: Failed to send tar file data\n");
            break;
        }
    }

    fclose(tar_file);

    // Clean up tar file - remove temporary file
    remove(tar_path);

    printf("S2: Tar file sent successfully\n");
}

// Handle DELETE requests - delete specified file from S2 storage
void handle_delete_request(int s1_socket, const char* filepath) {
    printf("S2: Processing DELETE request for file: '%s'\n", filepath);

    // Check if file exists using access() system call
    if (access(filepath, F_OK) == 0) {
        // File exists, try to delete it using unlink()
        if (unlink(filepath) == 0) {
            printf("S2: SUCCESS - Deleted file: %s\n", filepath);
            char success_response[] = "SUCCESS: File deleted from S2";
            send(s1_socket, success_response, strlen(success_response), 0);
        }
        else {
            // Failed to delete file - send error response
            printf("S2: ERROR - Failed to delete file: %s - %s\n", filepath, strerror(errno));
            char error_response[] = "ERROR: Failed to delete file from S2";
            send(s1_socket, error_response, strlen(error_response), 0);
        }
    }
    else {
        // File not found - send warning response
        printf("S2: WARNING - File not found: %s\n", filepath);
        char not_found_response[] = "WARNING: File not found in S2";
        send(s1_socket, not_found_response, strlen(not_found_response), 0);
    }
}

// Handle LIST requests - list all PDF files in specified directory
void handle_list_request(int s1_socket, const char* pathname) {
    printf("S2: Processing LIST request for path: '%s'\n", pathname);

    DIR* dir;                           // Directory stream pointer
    struct dirent* entry;               // Directory entry structure
    char response[BUFFER_SIZE * 4] = ""; // Large buffer for file list response

    // Try to open the requested directory
    dir = opendir(pathname);
    if (!dir) {
        printf("S2: Warning: Cannot open directory %s - %s\n", pathname, strerror(errno));
        char error_response[] = "ERROR: Directory not found in S2";
        send(s1_socket, error_response, strlen(error_response), 0);
        return;
    }

    // Arrays to collect .pdf files found
    char pdf_files[MAX_FILES][256];  // Array to store filenames
    int pdf_count = 0;               // Counter for PDF files found

    // Read directory entries and collect .pdf files
    while ((entry = readdir(dir)) != NULL && pdf_count < MAX_FILES) {
        if (entry->d_type == DT_REG) { // Regular file (not directory or special file)
            const char* ext = get_file_extension(entry->d_name);
            if (strcmp(ext, ".pdf") == 0) {
                // This is a PDF file - add to our list
                strncpy(pdf_files[pdf_count], entry->d_name, 255);
                pdf_files[pdf_count][255] = '\0'; // Ensure null termination
                pdf_count++;
                printf("S2: Found .pdf file: %s\n", entry->d_name);
            }
        }
    }
    closedir(dir);

    // Build response string with file list
    if (pdf_count == 0) {
        strcpy(response, "No .pdf files found in S2");
    }
    else {
        char temp[256];
        snprintf(temp, sizeof(temp), "Files found in S2: %d\n", pdf_count);
        strcat(response, temp);

        // Add all .pdf files to response
        for (int i = 0; i < pdf_count; i++) {
            strcat(response, pdf_files[i]);
            strcat(response, "\n");
        }
    }

    // Send the complete response back to S1
    printf("S2: Sending LIST response (%zu bytes)\n", strlen(response));
    send(s1_socket, response, strlen(response), 0);
}

// Handle file upload requests - complex function that handles binary protocol
void handle_upload_request(int s1_socket, char* initial_buffer, int initial_bytes) {
    char dest_path[MAX_PATH_LEN];  // Destination directory path
    char filename[256];            // Name of file being uploaded
    char full_path[MAX_PATH_LEN];  // Complete path where file will be saved
    int dest_len, filename_len;    // Lengths of destination path and filename
    long file_size;                // Size of file being uploaded
    char* buffer_ptr = initial_buffer;  // Pointer to current position in buffer
    int remaining_bytes = initial_bytes; // Bytes remaining in initial buffer

    printf("S2: Handling file upload request\n");

    // Parse destination path length from initial buffer (binary format)
    if (remaining_bytes < sizeof(dest_len)) {
        printf("S2: Error: Insufficient data for destination length\n");
        return;
    }

    memcpy(&dest_len, buffer_ptr, sizeof(dest_len)); // Extract 4-byte integer
    buffer_ptr += sizeof(dest_len);
    remaining_bytes -= sizeof(dest_len);

    printf("S2: Destination path length: %d\n", dest_len);

    // Validate destination path length
    if (dest_len <= 0 || dest_len >= MAX_PATH_LEN) {
        printf("S2: Error: Invalid destination path length\n");
        return;
    }

    // Parse destination path string
    if (remaining_bytes < dest_len) {
        printf("S2: Error: Insufficient data for destination path\n");
        return;
    }

    memset(dest_path, 0, sizeof(dest_path)); // Clear destination path buffer
    memcpy(dest_path, buffer_ptr, dest_len); // Copy path string
    buffer_ptr += dest_len;
    remaining_bytes -= dest_len;

    printf("S2: Destination path: '%s'\n", dest_path);

    // Ensure destination directory exists (create if needed)
    ensure_directory_exists(dest_path);

    // Parse filename length - may need to receive more data
    if (remaining_bytes < sizeof(filename_len)) {
        printf("S2: Need to receive more data for filename length\n");
        char temp_buffer[BUFFER_SIZE];
        int needed = sizeof(filename_len) - remaining_bytes;
        int received = recv(s1_socket, temp_buffer, needed, MSG_WAITALL);
        if (received <= 0) {
            printf("S2: Error receiving filename length\n");
            return;
        }

        // Combine partial data from initial buffer with new data
        char combined_buffer[sizeof(filename_len)];
        if (remaining_bytes > 0) {
            memcpy(combined_buffer, buffer_ptr, remaining_bytes);
        }
        memcpy(combined_buffer + remaining_bytes, temp_buffer, received);
        memcpy(&filename_len, combined_buffer, sizeof(filename_len));
        remaining_bytes = 0; // Used up all initial buffer data
    }
    else {
        // Filename length fits in remaining buffer
        memcpy(&filename_len, buffer_ptr, sizeof(filename_len));
        buffer_ptr += sizeof(filename_len);
        remaining_bytes -= sizeof(filename_len);
    }

    printf("S2: Filename length: %d\n", filename_len);

    // Validate filename length
    if (filename_len <= 0 || filename_len >= 256) {
        printf("S2: Error: Invalid filename length\n");
        return;
    }

    // Parse filename string - may span across multiple receives
    memset(filename, 0, sizeof(filename));
    if (remaining_bytes >= filename_len) {
        // Filename fits completely in remaining buffer
        memcpy(filename, buffer_ptr, filename_len);
        buffer_ptr += filename_len;
        remaining_bytes -= filename_len;
    }
    else {
        // Copy partial filename from initial buffer
        if (remaining_bytes > 0) {
            memcpy(filename, buffer_ptr, remaining_bytes);
        }

        // Receive rest of filename
        int still_needed = filename_len - remaining_bytes;
        int received = recv(s1_socket, filename + remaining_bytes, still_needed, MSG_WAITALL);
        if (received <= 0) {
            printf("S2: Error receiving filename\n");
            return;
        }
        remaining_bytes = 0;
    }

    printf("S2: Filename: '%s'\n", filename);

    // Parse file size - may need to receive more data
    if (remaining_bytes >= sizeof(file_size)) {
        // File size fits in remaining buffer
        memcpy(&file_size, buffer_ptr, sizeof(file_size));
        buffer_ptr += sizeof(file_size);
        remaining_bytes -= sizeof(file_size);
    }
    else {
        // Copy partial file size from initial buffer and receive rest
        char file_size_buffer[sizeof(file_size)];
        if (remaining_bytes > 0) {
            memcpy(file_size_buffer, buffer_ptr, remaining_bytes);
        }

        int still_needed = sizeof(file_size) - remaining_bytes;
        int received = recv(s1_socket, file_size_buffer + remaining_bytes, still_needed, MSG_WAITALL);
        if (received <= 0) {
            printf("S2: Error receiving file size\n");
            return;
        }

        memcpy(&file_size, file_size_buffer, sizeof(file_size));
        remaining_bytes = 0;
    }

    printf("S2: File size: %ld bytes\n", file_size);

    // Create full file path by combining destination path and filename
    snprintf(full_path, sizeof(full_path), "%s/%s", dest_path, filename);
    printf("S2: Full file path: '%s'\n", full_path);

    // Create and open file for writing in binary mode
    FILE* file = fopen(full_path, "wb");
    if (!file) {
        printf("S2: ERROR: Cannot create file '%s' - %s\n", full_path, strerror(errno));
        return;
    }

    // Handle any remaining file data from the initial buffer
    long total_received = 0; // Track total bytes received
    if (remaining_bytes > 0) {
        size_t written = fwrite(buffer_ptr, 1, remaining_bytes, file);
        if (written != remaining_bytes) {
            printf("S2: Error writing initial file data\n");
            fclose(file);
            return;
        }
        total_received += remaining_bytes;
        printf("S2: Wrote initial %d bytes from buffer\n", remaining_bytes);
    }

    // Receive remaining file content in chunks
    char file_buffer[BUFFER_SIZE];
    long remaining_file_bytes = file_size - total_received;

    printf("S2: Starting to receive remaining file data (%ld bytes)...\n", remaining_file_bytes);
    while (remaining_file_bytes > 0) {
        // Calculate how many bytes to request this iteration
        int to_read = (remaining_file_bytes < BUFFER_SIZE) ? remaining_file_bytes : BUFFER_SIZE;
        int received = recv(s1_socket, file_buffer, to_read, 0);

        if (received > 0) {
            // Write received data to file
            size_t written = fwrite(file_buffer, 1, received, file);
            if (written != received) {
                printf("S2: Error writing to file\n");
                break;
            }
            remaining_file_bytes -= received;
            total_received += received;

            // Progress indicator for large files (>1MB)
            if (file_size > 1024 * 1024) {
                printf("S2: Received: %.1f%% (%ld/%ld bytes)\r",
                    (double)total_received / file_size * 100, total_received, file_size);
                fflush(stdout); // Force output to display immediately
            }
        }
        else if (received == 0) {
            // Connection closed by sender
            printf("S2: Connection closed while receiving file\n");
            break;
        }
        else {
            // Error occurred during receive
            printf("S2: Error receiving file data: %s\n", strerror(errno));
            break;
        }
    }

    fclose(file);

    // Check if file transfer was successful
    if (total_received == file_size) {
        printf("\nS2: SUCCESS: File '%s' received completely (%ld bytes)\n", filename, total_received);
        printf("S2: File saved at: %s\n", full_path);
    }
    else {
        printf("\nS2: ERROR: File transfer incomplete (received %ld/%ld bytes)\n", total_received, file_size);
        // Remove incomplete file to avoid corrupted files
        unlink(full_path);
    }
}

// Utility function to create directory recursively (like mkdir -p)
void ensure_directory_exists(const char* path) {
    char tmp[MAX_PATH_LEN];  // Temporary path buffer
    char* p = NULL;          // Pointer for path traversal
    size_t len;              // Length of path
    struct stat st = { 0 };  // File status structure

    // Check if directory already exists
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("S2: Directory %s already exists\n", path);
        return;
    }

    // Copy path to temporary buffer for modification
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')  // Remove trailing slash if present
        tmp[len - 1] = 0;

    // Create parent directories by traversing path
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0; // Temporarily terminate string at this directory level
            if (stat(tmp, &st) == -1) { // Check if this level exists
                if (mkdir(tmp, 0755) == 0) { // Create directory with rwxr-xr-x permissions
                    printf("S2: Created directory: %s\n", tmp);
                }
            }
            *p = '/'; // Restore the slash
        }
    }

    // Create final directory if it doesn't exist
    if (stat(tmp, &st) == -1) {
        if (mkdir(tmp, 0755) == 0) {
            printf("S2: Created directory: %s\n", tmp);
        }
        else {
            printf("S2: Failed to create directory: %s (Error: %s)\n", tmp, strerror(errno));
        }
    }
}

// Utility function to get file extension from filename
const char* get_file_extension(const char* filename) {
    const char* dot = strrchr(filename, '.'); // Find last occurrence of '.'
    if (!dot || dot == filename) return "";   // No extension or filename starts with '.'
    return dot; // Return pointer to extension (including the dot)
}
