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

// Server configuration constants
#define S4_PORT 8003           // S4 server port for communication with S1
#define BUFFER_SIZE 8192       // Buffer size for file operations (8KB)
#define MAX_PATH 1024          // Maximum allowed path length
#define MAX_FILE_SIZE (500 * 1024 * 1024)  // Maximum ZIP file size: 500MB
#define MAX_FILES 1000         // Maximum number of files for directory listings

// Recursively create directory structure to ensure path exists before storing ZIP files (path)

void ensure_directory_exists(const char *path) {
    char tmp[MAX_PATH];
    char *p = NULL;
    size_t len;
    struct stat st = {0};  // Initialize stat structure to zeros

    // Check if directory already exists
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("[S4] Directory %s already exists\n", path);
        return;
    }

    // Copy path to temporary buffer for safe manipulation
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    // Remove trailing slash if present to normalize path
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    
    // Create parent directories recursively
    // Start from position 1 to skip root slash
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {  // Found a directory separator
            *p = 0;  // Temporarily terminate string at this component
            
            // Check if this part of the path exists
            if (stat(tmp, &st) == -1) {
                // Directory doesn't exist, create it with proper permissions (rwxr-xr-x)
                if (mkdir(tmp, 0755) == 0) {
                    printf("[S4] Created directory: %s\n", tmp);
                } else {
                    printf("[S4] Failed to create directory: %s (Error: %s)\n", 
                           tmp, strerror(errno));
                }
            }
            *p = '/';  // Restore the slash and continue
        }
    }
    
    // Create the final directory component
    if (stat(tmp, &st) == -1) {
        if (mkdir(tmp, 0755) == 0) {
            printf("[S4] Created directory: %s\n", tmp);
        } else {
            printf("[S4] Failed to create directory: %s (Error: %s)\n", 
                   tmp, strerror(errno));
        }
    }
}

// Get file extension - returns pointer to last '.' in filename or empty string if none found

const char* get_file_extension(const char* filename) {
    const char* dot = strrchr(filename, '.');  // Find last occurrence of '.'
    if (!dot || dot == filename) return "";    // No extension or starts with dot
    return dot;  // Return extension including the dot
}
// Validate ZIP file format - checks signature and structural integrity, returns 1 if valid, 0 if invalid (filepath)

int validate_zip_file(const char *filepath) {
    FILE *file = fopen(filepath, "rb");  // Open in binary read mode
    if (!file) {
        printf("[S4] Error: Cannot open file for validation: %s\n", strerror(errno));
        return 0;
    }
    
    // Check file size first - ZIP files need minimum structure
    fseek(file, 0, SEEK_END);  // Move to end of file
    long file_size = ftell(file);  // Get current position (file size)
    fseek(file, 0, SEEK_SET);  // Return to beginning
    
    if (file_size < 22) {  // Minimum ZIP file size (central directory record)
        printf("[S4] Error: File too small to be a valid ZIP file (%ld bytes)\n", file_size);
        fclose(file);
        return 0;
    }
    
    // Read and verify ZIP file signature (magic bytes)
    unsigned char signature[4];
    if (fread(signature, 1, 4, file) != 4) {
        printf("[S4] Error: Cannot read ZIP signature\n");
        fclose(file);
        return 0;
    }
    
    // Check for valid ZIP signatures (ZIP format magic numbers)
    // First two bytes are always 0x504B for ZIP files
    if (signature[0] == 0x50 && signature[1] == 0x4B) {
        if ((signature[2] == 0x03 && signature[3] == 0x04) ||  // Standard ZIP (local file header)
            (signature[2] == 0x05 && signature[3] == 0x06) ||  // Empty ZIP (end of central dir)
            (signature[2] == 0x07 && signature[3] == 0x08)) {  // Spanned ZIP (data descriptor)
            
            printf("[S4] ZIP signature validation: PASSED\n");
            fclose(file);
            return 1;
        }
    }
    
    printf("[S4] Error: Invalid ZIP signature\n");
    fclose(file);
    return 0;
}

// Send file to S1 for downlf command - reads from S4 storage and transmits over socket (s1_socket, filepath)

void send_file_to_s1(int s1_socket, const char* filepath) {
    FILE* file;
    char buffer[BUFFER_SIZE];
    char local_path[MAX_PATH];
    long file_size;
    int bytes_read;
    
    // Convert server path to actual local path
    // Example: ~S4/folder/file.zip -> /home/user/S4/folder/file.zip
    if (strncmp(filepath, "~S4", 3) == 0) {
        // Replace ~S4 with actual S4 directory path
        snprintf(local_path, sizeof(local_path), "%s/S4%s", getenv("HOME"), filepath + 3);
    } else {
        // Use path as-is
        strncpy(local_path, filepath, sizeof(local_path) - 1);
        local_path[sizeof(local_path) - 1] = '\0';  // Ensure null termination
    }
    
    printf("[S4] Looking for file: %s\n", local_path);
    
    // Attempt to open the requested file
    file = fopen(local_path, "rb");  // Binary read mode for ZIP files
    if (!file) {
        // Send -1 to indicate file not found
        file_size = -1;
        send(s1_socket, &file_size, sizeof(file_size), 0);
        printf("[S4] File not found: %s\n", local_path);
        return;
    }
    
    // Get file size by seeking to end and getting position
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);  // Return to beginning for reading
    
    // Send file size to S1 first (protocol requirement)
    if (send(s1_socket, &file_size, sizeof(file_size), 0) < 0) {
        printf("[S4] Failed to send file size\n");
        fclose(file);
        return;
    }
    
    printf("[S4] Sending file: %s (%ld bytes)\n", local_path, file_size);
    
    // Send file data to S1 in chunks
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (send(s1_socket, buffer, bytes_read, 0) < 0) {
            printf("[S4] Failed to send file data\n");
            break;  // Exit loop on send error
        }
    }
    
    fclose(file);
    printf("[S4] File sent successfully to S1\n");
}

//Handle file upload from S1: receives ZIP files, stores in S4 directories using length-prefixed protocol (client_sock, protocol_buffer, total_header_received)

void handle_file_upload(int client_sock, char* protocol_buffer, int total_header_received) {
    char dest_path[MAX_PATH];
    char filename[MAX_PATH];
    char full_path[MAX_PATH];
    char s4_base_path[MAX_PATH];
    
    printf("[S4] Processing file upload request\n");
    
    // Get S4 base directory path
    snprintf(s4_base_path, sizeof(s4_base_path), "%s/S4", getenv("HOME"));
    
    // Parse the protocol buffer using length-prefixed format
    // Protocol format: [4-byte path length][path string][4-byte filename length][filename string][8-byte file size]
    int pos = 0;  // Current position in buffer
    
    // Extract destination path (4-byte length + string)
    if (pos + 4 > total_header_received) {
        printf("[S4] Error: Not enough data for path length\n");
        return;
    }
    
    int path_len;
    memcpy(&path_len, protocol_buffer + pos, 4);  // Read path length
    pos += 4;
    
    printf("[S4] Path length: %d bytes\n", path_len);
    
    // Verify we have enough data for the path string
    if (pos + path_len > total_header_received) {
        printf("[S4] Error: Not enough data for path string (need %d bytes, have %d)\n", 
               path_len, total_header_received - pos);
        return;
    }
    
    // Extract path string if valid
    if (path_len > 0 && path_len < MAX_PATH) {
        memcpy(dest_path, protocol_buffer + pos, path_len);
        dest_path[path_len] = '\0';  // Null terminate
        pos += path_len;
    } else {
        dest_path[0] = '\0';  // Empty path
        pos += path_len; // Skip invalid path data
    }
    
    printf("[S4] Extracted destination path: '%s'\n", dest_path);
    
    // Extract filename (4-byte length + string)
    if (pos + 4 > total_header_received) {
        printf("[S4] Error: Not enough data for filename length\n");
        return;
    }
    
    int name_len;
    memcpy(&name_len, protocol_buffer + pos, 4);  // Read filename length
    pos += 4;
    
    printf("[S4] Filename length: %d bytes\n", name_len);
    
    // Verify we have enough data for the filename string
    if (pos + name_len > total_header_received) {
        printf("[S4] Error: Not enough data for filename string (need %d bytes, have %d)\n", 
               name_len, total_header_received - pos);
        return;
    }
    
    // Extract filename string if valid
    if (name_len > 0 && name_len < MAX_PATH) {
        memcpy(filename, protocol_buffer + pos, name_len);
        filename[name_len] = '\0';  // Null terminate
        pos += name_len;
    } else {
        printf("[S4] Error: Invalid filename length: %d\n", name_len);
        return;
    }
    
    printf("[S4] Extracted filename: '%s'\n", filename);
    
    // Extract file size (8 bytes - long integer)
    long file_size = 0;
    if (pos + 8 <= total_header_received) {
        memcpy(&file_size, protocol_buffer + pos, 8);  // Read file size
        pos += 8;
        printf("[S4] File size from header: %ld bytes\n", file_size);
    } else {
        printf("[S4] File size not in header, will receive separately\n");
    }
    
    // Handle destination path - convert S1 paths to S4 paths
    if (strlen(dest_path) <= 1) {
        // No specific path provided, use S4 base directory
        strcpy(dest_path, s4_base_path);
        printf("[S4] Using S4 base directory: %s\n", dest_path);
    } else {
        // Convert ~S1 path to ~S4 path for storage in S4 server
        if (strncmp(dest_path, "~S1", 3) == 0) {
            char temp_path[MAX_PATH];
            snprintf(temp_path, sizeof(temp_path), "%s/S4%s", getenv("HOME"), dest_path + 3);
            strcpy(dest_path, temp_path);
        }
        printf("[S4] Using converted path: %s\n", dest_path);
    }
    
    // Validate that this is indeed a ZIP file by extension
    const char *ext = strrchr(filename, '.');  // Find last dot
    if (!ext || (strcmp(ext, ".zip") != 0 && strcmp(ext, ".ZIP") != 0)) {
        printf("[S4] Error: File '%s' is not a ZIP file (extension: %s)\n", 
               filename, ext ? ext : "none");
        return;
    }
    
    // Ensure destination directory structure exists
    printf("[S4] Creating directory structure if needed...\n");
    ensure_directory_exists(dest_path);
    
    // Create complete file path in S4 directory
    snprintf(full_path, sizeof(full_path), "%s/%s", dest_path, filename);
    printf("[S4] Full file path: '%s'\n", full_path);
    
    // Validate filename is not empty
    if (strlen(filename) == 0) {
        printf("[S4] Error: Empty filename extracted\n");
        return;
    }
    
    // Handle file size - receive separately if not in header
    if (file_size == 0) {
        // Receive file size separately if not in header
        printf("[S4] Waiting for file size...\n");
        int size_received = recv(client_sock, &file_size, sizeof(file_size), MSG_WAITALL);
        if (size_received != sizeof(file_size)) {
            printf("[S4] Error: Failed to receive file size information (received %d bytes)\n", size_received);
            return;
        }
        printf("[S4] File size received separately: %ld bytes\n", file_size);
    }
    
    printf("[S4] File size: %ld bytes (%.2f MB)\n", file_size, file_size / (1024.0 * 1024.0));
    
    // Validate file size for ZIP files
    if (file_size <= 0) {
        printf("[S4] Error: Invalid file size %ld\n", file_size);
        return;
    }
    if (file_size > MAX_FILE_SIZE) {
        printf("[S4] Error: File size %ld exceeds maximum allowed size\n", file_size);
        return;
    }
    
    // Create and open file for writing (binary mode essential for ZIP)
    FILE *file = fopen(full_path, "wb");
    if (!file) {
        printf("[S4] Error: Cannot create file '%s' - %s\n", full_path, strerror(errno));
        
        // Skip the incoming file data to prevent socket blocking
        char skip_buffer[BUFFER_SIZE];
        long remaining = file_size;
        printf("[S4] Skipping %ld bytes of incoming ZIP data...\n", file_size);
        
        // Drain the socket to prevent blocking S1
        while (remaining > 0) {
            int to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
            int received = recv(client_sock, skip_buffer, to_read, 0);
            if (received > 0) {
                remaining -= received;
            } else {
                break;  // Connection closed or error
            }
        }
        return;
    }
    
    // Initialize file transfer variables
    char file_buffer[BUFFER_SIZE];
    long remaining = file_size;
    long total_received = 0;
    
    // Check if we already have some file content in the buffer
    int content_start_pos = pos;  // Position where file content starts
    int content_in_buffer = total_header_received - content_start_pos;
    
    // Handle any file content that was already received with the header
    if (content_in_buffer > 0) {
        printf("[S4] Found %d bytes of file content in protocol buffer\n", content_in_buffer);
        
        // Write the content that was already received
        size_t written = fwrite(protocol_buffer + content_start_pos, 1, content_in_buffer, file);
        if (written != content_in_buffer) {
            printf("[S4] Error: Failed to write buffered content to file\n");
            fclose(file);
            return;
        }
        
        // Update counters
        total_received += content_in_buffer;
        remaining -= content_in_buffer;
        
        printf("[S4] Wrote %d bytes from buffer, remaining: %ld bytes\n", 
               content_in_buffer, remaining);
    }
    
    // Receive remaining ZIP file content in chunks
    printf("[S4] Starting ZIP file reception (remaining: %ld bytes)...\n", remaining);
    
    while (remaining > 0) {
        // Determine chunk size for this iteration
        int to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        
        // Receive data chunk from S1 server
        int received = recv(client_sock, file_buffer, to_read, 0);
        
        if (received > 0) {
            // Write received data to file
            size_t written = fwrite(file_buffer, 1, received, file);
            if (written != received) {
                printf("[S4] Error: Failed to write complete data to file\n");
                break;
            }
            
            // Update remaining bytes and total received
            remaining -= received;
            total_received += received;
            
            // Progress indicator for large files (> 1MB)
            if (file_size > 1024 * 1024) {
                printf("[S4] Progress: %.1f%% (%ld/%ld bytes)\r", 
                       (double)total_received / file_size * 100, 
                       total_received, file_size);
                fflush(stdout);  // Force output to appear immediately
            }
            
        } else if (received == 0) {
            printf("[S4] Connection closed by S1 during ZIP transfer\n");
            break;
        } else {
            printf("[S4] Error receiving ZIP data: %s\n", strerror(errno));
            break;
        }
    }
    
    // Close file and verify transfer completion
    fclose(file);
    
    // Clear progress line for large files
    if (file_size > 1024 * 1024) {
        printf("\n");  // Clear progress line
    }
    
    // Check if transfer was successful
    if (total_received == file_size) {
        printf("[S4] SUCCESS: ZIP file '%s' stored successfully (%ld bytes)\n", 
               filename, total_received);
        printf("[S4] File location: %s\n", full_path);
        
        // Validate the received ZIP file integrity
        if (validate_zip_file(full_path)) {
            printf("[S4] ZIP validation: PASSED - Valid ZIP file format\n");
        } else {
            printf("[S4] ZIP validation: FAILED - File may be corrupted\n");
        }
        
    } else {
        printf("[S4] ERROR: ZIP file transfer incomplete\n");
        printf("[S4] Expected: %ld bytes, Received: %ld bytes\n", file_size, total_received);
        
        // Remove incomplete ZIP file to prevent corruption
        if (unlink(full_path) == 0) {
            printf("[S4] Removed incomplete ZIP file\n");
        } else {
            printf("[S4] Warning: Could not remove incomplete file\n");
        }
    }
}

// Main request dispatcher for S1 server - handles GET_FILE, DELETE, LIST, and file upload requests (s1_socket)
void handle_s1_request(int s1_socket) {
    char buffer[BUFFER_SIZE * 2]; // Larger buffer for protocol data
    char command[256], filepath[MAX_PATH];
    int bytes_received;
    
    printf("[S4] New connection session started with S1\n");
    
    // Receive initial data from S1 to determine request type
    bytes_received = recv(s1_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
        printf("[S4] Failed to receive data from S1\n");
        return;
    }
    
    buffer[bytes_received] = '\0';  // Null terminate received data
    printf("[S4] Received %d bytes from S1\n", bytes_received);
    printf("[S4] Initial data: '%s'\n", buffer);
    
    // Check if it's a GET_FILE command (for downlf operation)
    if (sscanf(buffer, "%s %s", command, filepath) == 2 && strcmp(command, "GET_FILE") == 0) {
        printf("[S4] Processing GET_FILE request for: %s\n", filepath);
        send_file_to_s1(s1_socket, filepath);
        return;
    }
    
    // Check if it's a DELETE command (for removef operation)
    if (strncmp(buffer, "DELETE ", 7) == 0) {
        char file_to_delete[MAX_PATH];
        char local_path[MAX_PATH];
        
        strcpy(file_to_delete, buffer + 7); // Skip "DELETE " prefix
        
        // Convert ~S4 path to local path if needed
        if (strncmp(file_to_delete, "~S4", 3) == 0) {
            snprintf(local_path, sizeof(local_path), "%s/S4%s", getenv("HOME"), file_to_delete + 3);
        } else {
            strncpy(local_path, file_to_delete, sizeof(local_path) - 1);
            local_path[sizeof(local_path) - 1] = '\0';
        }
        
        printf("[S4] Processing DELETE request for file: '%s'\n", local_path);
        
        // Check if file exists before attempting deletion
        if (access(local_path, F_OK) == 0) {
            // File exists, try to delete it
            if (unlink(local_path) == 0) {
                printf("[S4] SUCCESS - Deleted file: %s\n", local_path);
                char success_response[] = "SUCCESS: File deleted from S4";
                send(s1_socket, success_response, strlen(success_response), 0);
            } else {
                printf("[S4] ERROR - Failed to delete file: %s - %s\n", local_path, strerror(errno));
                char error_response[] = "ERROR: Failed to delete file from S4";
                send(s1_socket, error_response, strlen(error_response), 0);
            }
        } else {
            printf("[S4] WARNING - File not found: %s\n", local_path);
            char not_found_response[] = "WARNING: File not found in S4";
            send(s1_socket, not_found_response, strlen(not_found_response), 0);
        }
        
        return;
    }
    
    // Check if it's a LIST command (for dispfnames operation)
    if (strncmp(buffer, "LIST ", 5) == 0) {
        char list_path[MAX_PATH];
        char local_path[MAX_PATH];
        
        strcpy(list_path, buffer + 5); // Skip "LIST " prefix
        
        // Convert ~S4 path to local path if needed
        if (strncmp(list_path, "~S4", 3) == 0) {
            snprintf(local_path, sizeof(local_path), "%s/S4%s", getenv("HOME"), list_path + 3);
        } else {
            strncpy(local_path, list_path, sizeof(local_path) - 1);
            local_path[sizeof(local_path) - 1] = '\0';
        }
        
        printf("[S4] Processing LIST request for path: '%s'\n", local_path);
        
        DIR* dir;
        struct dirent* entry;
        char response[BUFFER_SIZE * 4] = ""; // Large buffer for file list
        
        // Attempt to open the directory
        dir = opendir(local_path);
        if (!dir) {
            printf("[S4] Warning: Cannot open directory %s - %s\n", local_path, strerror(errno));
            char error_response[] = "ERROR: Directory not found in S4";
            send(s1_socket, error_response, strlen(error_response), 0);
        } else {
            // Collect .zip files from the directory
            char zip_files[MAX_FILES][256];  // Array to store ZIP filenames
            int zip_count = 0;
            
            // Scan directory for ZIP files
            while ((entry = readdir(dir)) != NULL && zip_count < MAX_FILES) {
                if (entry->d_type == DT_REG) { // Regular file (not directory)
                    const char* ext = get_file_extension(entry->d_name);
                    // Check for ZIP extension (case insensitive)
                    if (strcmp(ext, ".zip") == 0 || strcmp(ext, ".ZIP") == 0) {
                        strncpy(zip_files[zip_count], entry->d_name, 255);
                        zip_files[zip_count][255] = '\0';  // Ensure null termination
                        zip_count++;
                        printf("[S4] Found .zip file: %s\n", entry->d_name);
                    }
                }
            }
            closedir(dir);
            
            // Build response string
            if (zip_count == 0) {
                strcpy(response, "No .zip files found in S4");
            } else {
                char temp[256];
                snprintf(temp, sizeof(temp), "Files found in S4: %d\n", zip_count);
                strcat(response, temp);
                
                // Sort ZIP files alphabetically using simple bubble sort
                for (int i = 0; i < zip_count - 1; i++) {
                    for (int j = 0; j < zip_count - i - 1; j++) {
                        if (strcmp(zip_files[j], zip_files[j + 1]) > 0) {
                            char temp_name[256];
                            strcpy(temp_name, zip_files[j]);
                            strcpy(zip_files[j], zip_files[j + 1]);
                            strcpy(zip_files[j + 1], temp_name);
                        }
                    }
                }
                
                // Add all sorted ZIP files to response
                for (int i = 0; i < zip_count; i++) {
                    strcat(response, zip_files[i]);
                    strcat(response, "\n");
                }
            }
            
            printf("[S4] Sending LIST response (%zu bytes)\n", strlen(response));
            send(s1_socket, response, strlen(response), 0);
        }
        
        return;
    }
    
    // If not a command, treat as file upload (uploadf operation)
    printf("[S4] Treating as file upload request\n");
    handle_file_upload(s1_socket, buffer, bytes_received);
}

// Main server function - sets up S4 server to handle S1 operations, creates socket, binds to port, enters accept loop

int main() {
    int server_socket, s1_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    printf("=== Starting S4 Server (ZIP File Handler) - INTEGRATED VERSION ===\n");
    printf("[S4] Server handles ZIP files for distributed file system\n");
    printf("[S4] Operations: uploadf, downlf, removef, dispfnames\n");
    printf("[S4] Port: %d\n", S4_PORT);
    
    // Step 1: Create S4 base directory if it doesn't exist
    char s4_dir[MAX_PATH];
    snprintf(s4_dir, sizeof(s4_dir), "%s/S4", getenv("HOME"));
    printf("[S4] Initializing base directory: %s\n", s4_dir);
    ensure_directory_exists(s4_dir);
    
    // Step 2: Create server socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);  // TCP socket
    if (server_socket < 0) {
        perror("[S4] Socket creation failed");
        exit(1);
    }
    
    // Step 3: Set socket options to allow address reuse
    // Prevents "Address already in use" error when restarting server
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[S4] Setsockopt failed");
        close(server_socket);
        exit(1);
    }
    
    // Step 4: Configure server address structure
    memset(&server_addr, 0, sizeof(server_addr));  // Zero out structure
    server_addr.sin_family = AF_INET;              // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;      // Accept connections from any interface
    server_addr.sin_port = htons(S4_PORT);         // Convert port to network byte order
    
    // Step 5: Bind socket to the specified port
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[S4] Bind failed");
        printf("[S4] Make sure port %d is not already in use\n", S4_PORT);
        close(server_socket);
        exit(1);
    }
    
    // Step 6: Start listening for incoming connections
    // Queue up to 10 pending connections
    if (listen(server_socket, 10) < 0) {
        perror("[S4] Listen failed");
        close(server_socket);
        exit(1);
    }
    
    printf("[S4] Server listening on port %d\n", S4_PORT);
    printf("[S4] Storage directory: %s\n", s4_dir);
    printf("[S4] Ready to handle requests from S1...\n\n");
    
    // Step 7: Main server loop - continuously accept and handle connections
    while (1) {
        // Accept connection from S1 server
        printf("[S4] Waiting for connection from S1...\n");
        s1_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (s1_socket < 0) {
            perror("[S4] Accept failed");
            continue;  // Try again on next iteration
        }
        
        printf("[S4] Connection accepted from %s\n", inet_ntoa(client_addr.sin_addr));
        
        // Fork child process to handle the request concurrently
        pid_t pid = fork();
        
        if (pid == 0) {
            // Child process - handle this specific client request
            close(server_socket);  // Child doesn't need the listening socket
            handle_s1_request(s1_socket);  // Process the request
            close(s1_socket);  // Close client connection
            printf("[S4] Session completed\n");
            exit(0);  // Child process exits after handling request
            
        } else if (pid > 0) {
            // Parent process - continue accepting new connections
            close(s1_socket);  // Parent doesn't need the client socket
            
            // Clean up zombie processes (finished child processes)
            // WNOHANG means don't block if no zombies are ready
            while (waitpid(-1, NULL, WNOHANG) > 0);
            
        } else {
            // Fork failed
            perror("[S4] Fork failed");
            close(s1_socket);  // Close the client socket and continue
        }
    }
    
    // Clean up (this code is never reached in normal operation)
    close(server_socket);
    return 0;
}
