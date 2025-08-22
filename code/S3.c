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

// Define constants for buffer sizes and operational limits
#define BUFFER_SIZE 8192          // Standard buffer size for file I/O operations
#define MAX_PATH_LEN 1024         // Maximum length for file and directory paths
#define S3_PORT 8002              // Port number for S3 server to listen on
#define MAX_FILES 1000            // Maximum number of files to handle in directory listings

 // Function prototypes - declaring all functions used in the program
void handle_s1_request(int s1_socket);                                          // Main request handler for S1 connections
void send_file_to_s1(int s1_socket, const char* filepath);                      // Handles GET_FILE requests
void create_and_send_tar_to_s1(int s1_socket, const char* filetype);           // Handles CREATE_TAR requests
void handle_delete_request(int s1_socket, const char* filepath);               // Handles DELETE requests
void handle_list_request(int s1_socket, const char* pathname);                 // Handles LIST requests
void handle_upload_request(int s1_socket, char* initial_buffer, int initial_bytes); // Handles file upload requests
void ensure_directory_exists(const char* path);                                // Utility to create directories recursively
const char* get_file_extension(const char* filename);                          // Utility to extract file extensions

int main() {
    int server_socket, s1_socket;    // Socket file descriptors for server and client connections
    struct sockaddr_in server_addr;  // Server address structure for socket binding

    printf("Starting Integrated S3 Server (TXT files)...\n");

    // Create S3 directory if it doesn't exist - this is the root storage directory for all TXT files
    char s3_dir[MAX_PATH_LEN];
    snprintf(s3_dir, sizeof(s3_dir), "%s/S3", getenv("HOME")); // Create path: ~/S3
    ensure_directory_exists(s3_dir);

    // Create server socket using TCP protocol (SOCK_STREAM)
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("S3: Socket creation failed");
        return 1;
    }

    // Allow socket reuse to prevent "Address already in use" errors when restarting server
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Setup server address structure for socket binding
    server_addr.sin_family = AF_INET;          // Use IPv4 addressing
    server_addr.sin_port = htons(S3_PORT);     // Convert port to network byte order
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all available network interfaces

    // Bind socket to the specified address and port
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("S3: Bind failed");
        close(server_socket);
        return 1;
    }

    // Start listening for incoming connections with a backlog queue of 5 connections
    if (listen(server_socket, 5) < 0) {
        perror("S3: Listen failed");
        close(server_socket);
        return 1;
    }

    // Print server startup information and status
    printf("S3 Server (TXT files) listening on port %d\n", S3_PORT);
    printf("Storage directory: %s\n", s3_dir);
    printf("Supports: File uploads, GET_FILE, CREATE_TAR, DELETE, LIST commands\n");
    printf("Waiting for requests from S1...\n");

    // Main server loop - continuously accept and handle client connections
    while (1) {
        // Accept incoming connection from S1 server
        s1_socket = accept(server_socket, NULL, NULL);
        if (s1_socket < 0) {
            perror("S3: Accept failed");
            continue; // Continue accepting other connections despite this failure
        }

        printf("S3: Received connection from S1\n");

        // Fork child process to handle request - enables concurrent request handling
        pid_t pid = fork();
        if (pid == 0) {
            // Child process - handles the specific client request
            close(server_socket);     // Child doesn't need the listening socket
            handle_s1_request(s1_socket); // Process the incoming request
            close(s1_socket);         // Close client socket when processing is complete
            exit(0);                  // Exit child process cleanly
        }
        else if (pid > 0) {
            // Parent process - continues accepting new connections
            close(s1_socket);         // Parent doesn't need this specific client socket
            // Clean up zombie child processes that have completed execution
            while (waitpid(-1, NULL, WNOHANG) > 0);
        }
        else {
            perror("S3: Fork failed"); // Fork failed, but server continues operation
        }
    }

    close(server_socket); // This line never reached in normal server operation
    return 0;
}

// Main function to handle and dispatch incoming requests from S1 server
void handle_s1_request(int s1_socket) {
    char buffer[BUFFER_SIZE];           // Buffer to receive command and initial data
    char command[256], parameter[MAX_PATH_LEN]; // Variables for command parsing
    int bytes_received;                 // Number of bytes received from socket

    // Receive command data from S1 server
    bytes_received = recv(s1_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received <= 0) {
        printf("S3: Failed to receive command from S1\n");
        return;
    }

    buffer[bytes_received] = '\0'; // Null-terminate the received string for safety
    printf("S3: Received command: %s\n", buffer);

    // Parse and dispatch different command types based on command prefix
    if (strncmp(buffer, "DELETE ", 7) == 0) {
        // DELETE command format: "DELETE /path/to/file.txt"
        char filepath[MAX_PATH_LEN];
        strcpy(filepath, buffer + 7); // Extract filepath by skipping "DELETE " prefix
        handle_delete_request(s1_socket, filepath);
    }
    else if (strncmp(buffer, "LIST ", 5) == 0) {
        // LIST command format: "LIST /path/to/directory"
        char pathname[MAX_PATH_LEN];
        strcpy(pathname, buffer + 5); // Extract pathname by skipping "LIST " prefix
        handle_list_request(s1_socket, pathname);
    }
    else if (sscanf(buffer, "%s %s", command, parameter) == 2) {
        // Two-parameter commands (command word + parameter)
        if (strcmp(command, "GET_FILE") == 0) {
            // GET_FILE command - retrieve and send specified file to S1
            send_file_to_s1(s1_socket, parameter);
        }
        else if (strcmp(command, "CREATE_TAR") == 0) {
            // CREATE_TAR command - create tar archive of all TXT files and send to S1
            create_and_send_tar_to_s1(s1_socket, parameter);
        }
        else {
            printf("S3: Unknown command: %s\n", command);
        }
    }
    else {
        // Check if this is a file upload request (binary data, no text command prefix)
        // File upload protocol starts with destination path length as 4-byte integer
        int dest_len;
        if (bytes_received >= sizeof(dest_len)) {
            memcpy(&dest_len, buffer, sizeof(dest_len)); // Extract length from binary data
            if (dest_len > 0 && dest_len < MAX_PATH_LEN) {
                // Valid destination length detected - this appears to be a file upload
                printf("S3: Detected file upload request\n");
                handle_upload_request(s1_socket, buffer, bytes_received);
                return;
            }
        }
        printf("S3: Invalid command format: %s\n", buffer);
    }
}

// Handle GET_FILE requests - retrieve and send requested file back to S1
void send_file_to_s1(int s1_socket, const char* filepath) {
    FILE* file;                    // File pointer for reading the requested file
    char buffer[BUFFER_SIZE];      // Buffer for reading and transferring file data
    char local_path[MAX_PATH_LEN]; // Converted local filesystem path
    long file_size;                // Size of the file to be sent
    int bytes_read;                // Bytes read from file in each iteration

    // Convert server-side path notation to actual local filesystem path
    // Example: ~S3/folder/file.txt -> /home/user/S3/folder/file.txt
    snprintf(local_path, sizeof(local_path), "%s/S3%s", getenv("HOME"), filepath + 3);

    printf("S3: Looking for file: %s\n", local_path);

    // Attempt to open the requested file in binary read mode
    file = fopen(local_path, "rb");
    if (!file) {
        // File not found - send -1 as file size to indicate error to S1
        file_size = -1;
        send(s1_socket, &file_size, sizeof(file_size), 0);
        printf("S3: File not found: %s\n", local_path);
        return;
    }

    // Determine file size by seeking to end and getting current position
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET); // Reset file pointer to beginning

    // Send file size to S1 first (required by protocol)
    if (send(s1_socket, &file_size, sizeof(file_size), 0) < 0) {
        printf("S3: Failed to send file size\n");
        fclose(file);
        return;
    }

    printf("S3: Sending file: %s (%ld bytes)\n", local_path, file_size);

    // Send file contents to S1 in chunks of BUFFER_SIZE
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (send(s1_socket, buffer, bytes_read, 0) < 0) {
            printf("S3: Failed to send file data\n");
            break;
        }
    }

    fclose(file);
    printf("S3: File sent successfully\n");
}

// Handle CREATE_TAR requests - create tar archive of all TXT files and send to S1
void create_and_send_tar_to_s1(int s1_socket, const char* filetype) {
    char tar_command[MAX_PATH_LEN];  // Shell command to create tar archive
    char tar_path[MAX_PATH_LEN];     // Full path to the created tar file
    FILE* tar_file;                  // File pointer for reading the created tar
    char buffer[BUFFER_SIZE];        // Buffer for reading and sending tar data
    long file_size;                  // Size of the created tar file
    int bytes_read;                  // Bytes read from tar file in each iteration

    // Define tar filename - always "text.tar" for TXT files
    const char* tar_filename = "text.tar";
    snprintf(tar_path, sizeof(tar_path), "%s/S3/%s", getenv("HOME"), tar_filename);

    // Create shell command to find all .txt files recursively and create tar archive
    // Uses find command to locate files, pipes list to tar for archiving
    snprintf(tar_command, sizeof(tar_command),
        "cd %s/S3 && find . -name '*.txt' -type f | tar -cf %s -T - 2>/dev/null",
        getenv("HOME"), tar_filename);

    printf("S3: Creating tar file with command: %s\n", tar_command);

    // Execute the tar creation command using system() call
    int result = system(tar_command);
    if (result != 0) {
        printf("S3: Warning: tar command returned %d\n", result);
    }

    // Open the created tar file for reading in binary mode
    tar_file = fopen(tar_path, "rb");
    if (!tar_file) {
        // Failed to create or open tar file - send -1 to indicate failure
        file_size = -1;
        send(s1_socket, &file_size, sizeof(file_size), 0);
        printf("S3: Failed to create or open tar file: %s\n", tar_path);
        return;
    }

    // Determine tar file size
    fseek(tar_file, 0, SEEK_END);
    file_size = ftell(tar_file);
    fseek(tar_file, 0, SEEK_SET);

    // Send tar file size to S1 first (protocol requirement)
    if (send(s1_socket, &file_size, sizeof(file_size), 0) < 0) {
        printf("S3: Failed to send tar file size\n");
        fclose(tar_file);
        return;
    }

    printf("S3: Sending tar file: %s (%ld bytes)\n", tar_path, file_size);

    // Send tar file data to S1 in chunks
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, tar_file)) > 0) {
        if (send(s1_socket, buffer, bytes_read, 0) < 0) {
            printf("S3: Failed to send tar file data\n");
            break;
        }
    }

    fclose(tar_file);

    // Clean up temporary tar file from filesystem
    remove(tar_path);

    printf("S3: Tar file sent successfully\n");
}

// Handle DELETE requests - delete specified file from S3 storage
void handle_delete_request(int s1_socket, const char* filepath) {
    printf("S3: Processing DELETE request for file: '%s'\n", filepath);

    // Check if the specified file exists using access() system call
    if (access(filepath, F_OK) == 0) {
        // File exists - attempt to delete it using unlink() system call
        if (unlink(filepath) == 0) {
            printf("S3: SUCCESS - Deleted file: %s\n", filepath);
            char success_response[] = "SUCCESS: File deleted from S3";
            send(s1_socket, success_response, strlen(success_response), 0);
        }
        else {
            // Delete operation failed - send error response to S1
            printf("S3: ERROR - Failed to delete file: %s - %s\n", filepath, strerror(errno));
            char error_response[] = "ERROR: Failed to delete file from S3";
            send(s1_socket, error_response, strlen(error_response), 0);
        }
    }
    else {
        // File doesn't exist - send warning response to S1
        printf("S3: WARNING - File not found: %s\n", filepath);
        char not_found_response[] = "WARNING: File not found in S3";
        send(s1_socket, not_found_response, strlen(not_found_response), 0);
    }
}

// Handle LIST requests - list all TXT files in specified directory
void handle_list_request(int s1_socket, const char* pathname) {
    printf("S3: Processing LIST request for path: '%s'\n", pathname);

    DIR* dir;                           // Directory stream for reading directory entries
    struct dirent* entry;               // Structure representing directory entries
    char response[BUFFER_SIZE * 4] = ""; // Large buffer to build response string

    // Attempt to open the specified directory
    dir = opendir(pathname);
    if (!dir) {
        printf("S3: Warning: Cannot open directory %s - %s\n", pathname, strerror(errno));
        char error_response[] = "ERROR: Directory not found in S3";
        send(s1_socket, error_response, strlen(error_response), 0);
        return;
    }

    // Arrays to collect found .txt files
    char txt_files[MAX_FILES][256];  // Array to store TXT filenames
    int txt_count = 0;               // Counter for number of TXT files found

    // Read directory entries and filter for .txt files
    while ((entry = readdir(dir)) != NULL && txt_count < MAX_FILES) {
        if (entry->d_type == DT_REG) { // Check if entry is a regular file (not directory)
            const char* ext = get_file_extension(entry->d_name);
            if (strcmp(ext, ".txt") == 0) {
                // This is a TXT file - add it to our collection
                strncpy(txt_files[txt_count], entry->d_name, 255);
                txt_files[txt_count][255] = '\0'; // Ensure null termination
                txt_count++;
                printf("S3: Found .txt file: %s\n", entry->d_name);
            }
        }
    }
    closedir(dir);

    // Build response string containing file list
    if (txt_count == 0) {
        strcpy(response, "No .txt files found in S3");
    }
    else {
        char temp[256];
        snprintf(temp, sizeof(temp), "Files found in S3: %d\n", txt_count);
        strcat(response, temp);

        // Add all found .txt files to the response
        for (int i = 0; i < txt_count; i++) {
            strcat(response, txt_files[i]);
            strcat(response, "\n");
        }
    }

    // Send the complete file list response back to S1
    printf("S3: Sending LIST response (%zu bytes)\n", strlen(response));
    send(s1_socket, response, strlen(response), 0);
}

// Handle file upload requests - complex function implementing binary upload protocol
void handle_upload_request(int s1_socket, char* initial_buffer, int initial_bytes) {
    char dest_path[MAX_PATH_LEN];  // Destination directory path for uploaded file
    char filename[256];            // Name of the file being uploaded
    char full_path[MAX_PATH_LEN];  // Complete filesystem path where file will be saved
    int dest_len, filename_len;    // Lengths of destination path and filename strings
    long file_size;                // Size of the file being uploaded
    char* buffer_ptr = initial_buffer;  // Pointer to current position in buffer
    int remaining_bytes = initial_bytes; // Bytes remaining to be processed in buffer

    printf("S3: Handling file upload request\n");

    // Parse destination path length from initial buffer (stored as binary integer)
    if (remaining_bytes < sizeof(dest_len)) {
        printf("S3: Error: Insufficient data for destination length\n");
        return;
    }

    memcpy(&dest_len, buffer_ptr, sizeof(dest_len)); // Extract 4-byte integer
    buffer_ptr += sizeof(dest_len);
    remaining_bytes -= sizeof(dest_len);

    printf("S3: Destination path length: %d\n", dest_len);

    // Validate destination path length for sanity
    if (dest_len <= 0 || dest_len >= MAX_PATH_LEN) {
        printf("S3: Error: Invalid destination path length\n");
        return;
    }

    // Parse destination path string from buffer
    if (remaining_bytes < dest_len) {
        printf("S3: Error: Insufficient data for destination path\n");
        return;
    }

    memset(dest_path, 0, sizeof(dest_path)); // Clear destination buffer
    memcpy(dest_path, buffer_ptr, dest_len); // Copy path string
    buffer_ptr += dest_len;
    remaining_bytes -= dest_len;

    printf("S3: Destination path: '%s'\n", dest_path);

    // Ensure the destination directory structure exists (create if necessary)
    ensure_directory_exists(dest_path);

    // Parse filename length - may require additional data from socket
    if (remaining_bytes < sizeof(filename_len)) {
        printf("S3: Need to receive more data for filename length\n");
        char temp_buffer[BUFFER_SIZE];
        int needed = sizeof(filename_len) - remaining_bytes;
        int received = recv(s1_socket, temp_buffer, needed, MSG_WAITALL);
        if (received <= 0) {
            printf("S3: Error receiving filename length\n");
            return;
        }

        // Combine partial data from initial buffer with newly received data
        char combined_buffer[sizeof(filename_len)];
        if (remaining_bytes > 0) {
            memcpy(combined_buffer, buffer_ptr, remaining_bytes);
        }
        memcpy(combined_buffer + remaining_bytes, temp_buffer, received);
        memcpy(&filename_len, combined_buffer, sizeof(filename_len));
        remaining_bytes = 0; // All initial buffer data has been consumed
    }
    else {
        // Filename length completely contained in remaining buffer
        memcpy(&filename_len, buffer_ptr, sizeof(filename_len));
        buffer_ptr += sizeof(filename_len);
        remaining_bytes -= sizeof(filename_len);
    }

    printf("S3: Filename length: %d\n", filename_len);

    // Validate filename length for sanity
    if (filename_len <= 0 || filename_len >= 256) {
        printf("S3: Error: Invalid filename length\n");
        return;
    }

    // Parse filename string - may span across multiple socket receives
    memset(filename, 0, sizeof(filename));
    if (remaining_bytes >= filename_len) {
        // Complete filename available in remaining buffer
        memcpy(filename, buffer_ptr, filename_len);
        buffer_ptr += filename_len;
        remaining_bytes -= filename_len;
    }
    else {
        // Copy partial filename from initial buffer
        if (remaining_bytes > 0) {
            memcpy(filename, buffer_ptr, remaining_bytes);
        }

        // Receive remaining part of filename from socket
        int still_needed = filename_len - remaining_bytes;
        int received = recv(s1_socket, filename + remaining_bytes, still_needed, MSG_WAITALL);
        if (received <= 0) {
            printf("S3: Error receiving filename\n");
            return;
        }
        remaining_bytes = 0;
    }

    printf("S3: Filename: '%s'\n", filename);

    // Parse file size - may require additional socket reads
    if (remaining_bytes >= sizeof(file_size)) {
        // File size completely available in remaining buffer
        memcpy(&file_size, buffer_ptr, sizeof(file_size));
        buffer_ptr += sizeof(file_size);
        remaining_bytes -= sizeof(file_size);
    }
    else {
        // Assemble file size from partial buffer data and socket read
        char file_size_buffer[sizeof(file_size)];
        if (remaining_bytes > 0) {
            memcpy(file_size_buffer, buffer_ptr, remaining_bytes);
        }

        int still_needed = sizeof(file_size) - remaining_bytes;
        int received = recv(s1_socket, file_size_buffer + remaining_bytes, still_needed, MSG_WAITALL);
        if (received <= 0) {
            printf("S3: Error receiving file size\n");
            return;
        }

        memcpy(&file_size, file_size_buffer, sizeof(file_size));
        remaining_bytes = 0;
    }

    printf("S3: File size: %ld bytes\n", file_size);

    // Construct complete file path by combining destination directory and filename
    snprintf(full_path, sizeof(full_path), "%s/%s", dest_path, filename);
    printf("S3: Full file path: '%s'\n", full_path);

    // Create and open destination file for writing in binary mode
    FILE* file = fopen(full_path, "wb");
    if (!file) {
        printf("S3: ERROR: Cannot create file '%s' - %s\n", full_path, strerror(errno));
        return;
    }

    // Write any remaining file data that was included in the initial buffer
    long total_received = 0; // Track total bytes of file content received
    if (remaining_bytes > 0) {
        size_t written = fwrite(buffer_ptr, 1, remaining_bytes, file);
        if (written != remaining_bytes) {
            printf("S3: Error writing initial file data\n");
            fclose(file);
            return;
        }
        total_received += remaining_bytes;
        printf("S3: Wrote initial %d bytes from buffer\n", remaining_bytes);
    }

    // Receive and write remaining file content in chunks
    char file_buffer[BUFFER_SIZE];
    long remaining_file_bytes = file_size - total_received;

    printf("S3: Starting to receive remaining file data (%ld bytes)...\n", remaining_file_bytes);
    while (remaining_file_bytes > 0) {
        // Calculate optimal read size for this iteration
        int to_read = (remaining_file_bytes < BUFFER_SIZE) ? remaining_file_bytes : BUFFER_SIZE;
        int received = recv(s1_socket, file_buffer, to_read, 0);

        if (received > 0) {
            // Write received data chunk to file
            size_t written = fwrite(file_buffer, 1, received, file);
            if (written != received) {
                printf("S3: Error writing to file\n");
                break;
            }
            remaining_file_bytes -= received;
            total_received += received;

            // Display progress indicator for large file transfers (>1MB)
            if (file_size > 1024 * 1024) {
                printf("S3: Received: %.1f%% (%ld/%ld bytes)\r",
                    (double)total_received / file_size * 100, total_received, file_size);
                fflush(stdout); // Force immediate display of progress
            }
        }
        else if (received == 0) {
            // Connection closed by sender before transfer completion
            printf("S3: Connection closed while receiving file\n");
            break;
        }
        else {
            // Error occurred during socket receive operation
            printf("S3: Error receiving file data: %s\n", strerror(errno));
            break;
        }
    }

    fclose(file);

    // Verify successful file transfer completion
    if (total_received == file_size) {
        printf("\nS3: SUCCESS: File '%s' received completely (%ld bytes)\n", filename, total_received);
        printf("S3: File saved at: %s\n", full_path);
    }
    else {
        printf("\nS3: ERROR: File transfer incomplete (received %ld/%ld bytes)\n", total_received, file_size);
        // Remove incomplete file to prevent corruption issues
        unlink(full_path);
    }
}

// Utility function to create directory structure recursively (similar to mkdir -p)
void ensure_directory_exists(const char* path) {
    char tmp[MAX_PATH_LEN];  // Temporary buffer for path manipulation
    char* p = NULL;          // Pointer for traversing path components
    size_t len;              // Length of the input path
    struct stat st = { 0 };  // File status structure for existence checking

    // Check if directory already exists before attempting creation
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("S3: Directory %s already exists\n", path);
        return;
    }

    // Copy path to temporary buffer for safe modification
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')  // Remove trailing slash if present
        tmp[len - 1] = 0;

    // Create parent directories by traversing path components
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0; // Temporarily terminate string at current directory level
            if (stat(tmp, &st) == -1) { // Check if this directory level exists
                if (mkdir(tmp, 0755) == 0) { // Create directory with standard permissions
                    printf("S3: Created directory: %s\n", tmp);
                }
            }
            *p = '/'; // Restore the path separator
        }
    }

    // Create the final directory level if it doesn't exist
    if (stat(tmp, &st) == -1) {
        if (mkdir(tmp, 0755) == 0) {
            printf("S3: Created directory: %s\n", tmp);
        }
        else {
            printf("S3: Failed to create directory: %s (Error: %s)\n", tmp, strerror(errno));
        }
    }
}

// Utility function to extract file extension from filename string
const char* get_file_extension(const char* filename) {
    const char* dot = strrchr(filename, '.'); // Find last occurrence of dot character
    if (!dot || dot == filename) return "";   // No extension found or filename starts with dot
    return dot; // Return pointer to extension string (including the dot)
}
