#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libgen.h>
#include <errno.h>

// Constants for buffer sizes and server port
#define BUFFER_SIZE 8192        // Size of data buffers for file transfers
#define MAX_PATH_LEN 1024       // Maximum length for file paths
#define S1_PORT 8000            // Port number for S1 server connection

// Function prototypes
int connect_to_s1();            // Establishes connection to S1 server
int parse_command(char* input, char* args[]);  // Parses user input into command arguments

// Upload functions
int validate_uploadf_command(const char* command);  // Validates uploadf command syntax
int client_uploadf(int socket_fd, int argc, char* argv[]);  // Handles file upload to server

// Download functions
int client_downlf(int socket_fd, int argc, char* argv[]);   // Handles single file download
int client_downltar(int socket_fd, int argc, char* argv[]); // Handles tar file download
int receive_file_from_server(int socket_fd, const char* filename);  // Receives file from server
int receive_tar_from_server(int socket_fd, const char* tarname);    // Receives tar file from server

// Remove and display functions
int validate_removef_command(const char* command);      // Validates removef command
int validate_dispfnames_command(const char* command);   // Validates dispfnames command
int client_removef(int socket_fd, const char* command); // Handles file removal on server
int client_dispfnames(int socket_fd, const char* command); // Handles directory listing

// Validation functions
int validate_file_path(const char* filepath);           // Validates server file path format
int validate_file_extension(const char* filepath);       // Validates supported file extensions
int validate_tar_filetype(const char* filetype);        // Validates supported tar file types
const char* get_file_extension(const char* filename);   // Extracts file extension from filename

int main() {
    char input[BUFFER_SIZE];    // Buffer for user input
    char* args[10];             // Array for command arguments (max 10)
    int argc;                   // Argument count
    
    // Display client interface and help information
    printf("=== S25 Distributed File System Client ===\n");
    printf("Available commands:\n");
    printf("  uploadf file1 [file2] [file3] ~S1/destination/path\n");
    printf("  downlf ~S1/path/file1 [~S1/path/file2]\n");
    printf("  downltar filetype (.c/.pdf/.txt)\n");
    printf("  removef ~S1/path/file1 [~S1/path/file2]\n");
    printf("  dispfnames ~S1/path/to/directory\n");
    printf("  quit - to exit\n");
    printf("Supported file types: .c, .pdf, .txt, .zip (upload only)\n");
    printf("==========================================\n\n");
    
    // Main command loop
    while (1) {
        printf("s25client$ ");
        if (!fgets(input, sizeof(input), stdin)) break;  // Read user input
        
        // Remove newline character from input
        input[strcspn(input, "\n")] = 0;
        
        // Skip empty input
        if (strlen(input) == 0) continue;
        
        // Exit commands
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("Exiting client...\n");
            break;
        }
        
        // Parse command into arguments
        argc = parse_command(input, args);
        if (argc == 0) continue;  // Skip if no valid command
        
        printf("\n=== Processing Command: '%s' ===\n", input);
        
        // Command routing based on first argument
        if (strcmp(args[0], "uploadf") == 0) {
            // Handle file upload command
            if (validate_uploadf_command(input)) {
                int socket_fd = connect_to_s1();
                if (socket_fd >= 0) {
                    client_uploadf(socket_fd, argc, args);
                    close(socket_fd);
                }
            }
        }
        else if (strcmp(args[0], "downlf") == 0) {
            // Handle file download command
            int socket_fd = connect_to_s1();
            if (socket_fd >= 0) {
                client_downlf(socket_fd, argc, args);
                close(socket_fd);
            }
        }
        else if (strcmp(args[0], "downltar") == 0) {
            // Handle tar file download command
            int socket_fd = connect_to_s1();
            if (socket_fd >= 0) {
                client_downltar(socket_fd, argc, args);
                close(socket_fd);
            }
        }
        else if (strcmp(args[0], "removef") == 0) {
            // Handle file removal command
            if (validate_removef_command(input)) {
                int socket_fd = connect_to_s1();
                if (socket_fd >= 0) {
                    client_removef(socket_fd, input);
                    close(socket_fd);
                }
            }
        }
        else if (strcmp(args[0], "dispfnames") == 0) {
            // Handle directory listing command
            if (validate_dispfnames_command(input)) {
                int socket_fd = connect_to_s1();
                if (socket_fd >= 0) {
                    client_dispfnames(socket_fd, input);
                    close(socket_fd);
                }
            }
        }
        else {
            // Unknown command handler
            printf("Unknown command '%s'. Available: uploadf, downlf, downltar, removef, dispfnames, quit\n", args[0]);
        }
        
        printf("=== Command Complete ===\n\n");
    }
    
    return 0;
}

// Establishes TCP connection to S1 server
int connect_to_s1() {
    int socket_fd;
    struct sockaddr_in server_addr;
    
    // Create TCP socket
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    // Configure server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(S1_PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");  // Connect to localhost
    
    printf("Connecting to S1 server on port %d...\n", S1_PORT);
    if (connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Error: Cannot connect to S1 server. Make sure S1 server is running.\n");
        close(socket_fd);
        return -1;
    }
    
    printf("Connected to S1 server successfully\n");
    return socket_fd;
}

// Parses user input into command arguments
int parse_command(char* input, char* args[]) {
    int argc = 0;
    char* input_copy = malloc(strlen(input) + 1);
    strcpy(input_copy, input);
    
    // Tokenize input string by spaces
    char* token = strtok(input_copy, " ");
    while (token != NULL && argc < 10) {
        args[argc] = malloc(strlen(token) + 1);
        strcpy(args[argc], token);
        argc++;
        token = strtok(NULL, " ");
    }
    
    free(input_copy);
    return argc;
}

// Extracts file extension from filename
const char* get_file_extension(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "";
    return dot;
}

// Validates server file path format (~S1/...)
int validate_file_path(const char* filepath) {
    if (strncmp(filepath, "~S1/", 4) != 0) {
        printf("Error: File path must start with ~S1/\n");
        return 0;
    }
    return 1;
}

// Validates supported file extensions for upload/download
int validate_file_extension(const char* filepath) {
    const char* ext = get_file_extension(filepath);
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".pdf") == 0 || 
        strcmp(ext, ".txt") == 0 || strcmp(ext, ".zip") == 0) {
        return 1;
    }
    printf("Error: Unsupported file type '%s'. Only .c, .pdf, .txt, .zip files allowed.\n", ext);
    return 0;
}

// Validates supported file types for tar download
int validate_tar_filetype(const char* filetype) {
    if (strcmp(filetype, ".c") == 0 || strcmp(filetype, ".pdf") == 0 || strcmp(filetype, ".txt") == 0) {
        return 1;
    }
    printf("Error: Unsupported tar filetype '%s'. Only .c, .pdf, .txt allowed.\n", filetype);
    return 0;
}

// Validates uploadf command syntax and arguments
int validate_uploadf_command(const char* command) {
    char cmd[256], arg1[256], arg2[256], arg3[256], dest[MAX_PATH_LEN];
    
    memset(cmd, 0, sizeof(cmd));
    memset(arg1, 0, sizeof(arg1));
    memset(arg2, 0, sizeof(arg2));
    memset(arg3, 0, sizeof(arg3));
    memset(dest, 0, sizeof(dest));
    
    // Parse command into components
    int args = sscanf(command, "%s %s %s %s %s", cmd, arg1, arg2, arg3, dest);
    
    if (strcmp(cmd, "uploadf") != 0) {
        printf("Error: Invalid command '%s'. Use 'uploadf'\n", cmd);
        return 0;
    }
    
    if (args < 3) {
        printf("Error: Usage: uploadf file1 [file2] [file3] ~S1/destination/path\n");
        return 0;
    }
    
    // Determine files and destination from parsed arguments
    char files[3][256];
    int file_count = 0;
    char* destination = NULL;
    
    if (args == 3 && arg2[0] == '~') {
        strcpy(files[0], arg1);
        file_count = 1;
        destination = arg2;
    }
    else if (args == 4 && arg3[0] == '~') {
        strcpy(files[0], arg1);
        strcpy(files[1], arg2);
        file_count = 2;
        destination = arg3;
    }
    else if (args == 5 && dest[0] == '~') {
        strcpy(files[0], arg1);
        strcpy(files[1], arg2);
        strcpy(files[2], arg3);
        file_count = 3;
        destination = dest;
    }
    
    if (!destination || file_count == 0) {
        printf("Error: Could not parse command. Ensure destination starts with ~S1\n");
        return 0;
    }
    
    if (file_count < 1 || file_count > 3) {
        printf("Error: Must specify 1-3 files\n");
        return 0;
    }
    
    // Validate destination path format
    if (strncmp(destination, "~S1", 3) != 0) {
        printf("Error: Destination must start with ~S1\n");
        return 0;
    }
    
    // Validate each file's extension and accessibility
    for (int i = 0; i < file_count; i++) {
        const char* ext = get_file_extension(files[i]);
        if (strcmp(ext, ".c") != 0 && strcmp(ext, ".pdf") != 0 &&
            strcmp(ext, ".txt") != 0 && strcmp(ext, ".zip") != 0) {
            printf("Error: Invalid file type '%s' for file %s. Supported: .c, .pdf, .txt, .zip\n",
                ext, files[i]);
            return 0;
        }
        
        if (access(files[i], F_OK) != 0) {
            printf("Error: File %s not found in current directory\n", files[i]);
            return 0;
        }
        
        if (access(files[i], R_OK) != 0) {
            printf("Error: Cannot read file %s\n", files[i]);
            return 0;
        }
    }
    
    printf("uploadf command validation successful\n");
    return 1;
}

// Handles file upload to server
int client_uploadf(int socket_fd, int argc, char* argv[]) {
    char response[BUFFER_SIZE * 4];
    
    // Construct full command string to send to server
    char command[BUFFER_SIZE];
    strcpy(command, argv[0]);
    for (int i = 1; i < argc; i++) {
        strcat(command, " ");
        strcat(command, argv[i]);
    }
    
    printf("Sending command to server...\n");
    int cmd_len = strlen(command) + 1;
    if (send(socket_fd, command, cmd_len, 0) < 0) {
        perror("Failed to send command");
        return 0;
    }
    
    // Small delay to ensure command is processed
    usleep(100000); // 100ms
    
    // Determine which files to send based on arguments
    char files[3][256];
    int file_count = 0;
    
    if (argc == 3 && argv[2][0] == '~') {
        strcpy(files[0], argv[1]);
        file_count = 1;
    }
    else if (argc == 4 && argv[3][0] == '~') {
        strcpy(files[0], argv[1]);
        strcpy(files[1], argv[2]);
        file_count = 2;
    }
    else if (argc == 5 && argv[4][0] == '~') {
        strcpy(files[0], argv[1]);
        strcpy(files[1], argv[2]);
        strcpy(files[2], argv[3]);
        file_count = 3;
    }
    
    printf("Preparing to send %d file(s)\n", file_count);
    
    // Send each file to server
    int files_sent = 0;
    for (int i = 0; i < file_count; i++) {
        printf("\n--- Sending file %d: %s ---\n", i + 1, files[i]);
        
        FILE* file = fopen(files[i], "rb");
        if (!file) {
            printf("Error: Could not open file %s - %s\n", files[i], strerror(errno));
            continue;
        }
        
        // Get file size
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        fseek(file, 0, SEEK_SET);
        
        printf("File size: %ld bytes\n", file_size);
        
        // Send file size first
        printf("Sending file size...\n");
        if (send(socket_fd, &file_size, sizeof(file_size), MSG_NOSIGNAL) < 0) {
            printf("Error sending file size: %s\n", strerror(errno));
            fclose(file);
            continue;
        }
        
        usleep(50000); // 50ms delay
        
        // Send file content in chunks
        printf("Sending file content...\n");
        char buffer[BUFFER_SIZE];
        size_t bytes_read;
        long total_sent = 0;
        
        while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
            int bytes_sent = send(socket_fd, buffer, bytes_read, MSG_NOSIGNAL);
            if (bytes_sent < 0) {
                printf("Error sending file data: %s\n", strerror(errno));
                break;
            }
            else {
                total_sent += bytes_sent;
            }
        }
        fclose(file);
        
        if (total_sent == file_size) {
            printf("Successfully sent file: %s (%ld bytes)\n", files[i], total_sent);
            files_sent++;
        }
        else {
            printf("Error: File transfer incomplete for %s\n", files[i]);
        }
    }
    
    printf("\nFiles sent: %d out of %d\n", files_sent, file_count);
    
    // Receive and display server response
    printf("Waiting for server response...\n");
    memset(response, 0, sizeof(response));
    int bytes = recv(socket_fd, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        printf("Server response: %s\n", response);
    }
    else {
        printf("No response received from server\n");
    }
    
    return files_sent > 0;
}

// Handles single file download from server
int client_downlf(int socket_fd, int argc, char* argv[]) {
    // Validate number of arguments (command + 1-2 filenames)
    if (argc < 2 || argc > 3) {
        printf("Usage: downlf filename1 [filename2]\n");
        printf("Example: downlf ~S1/folder/file.pdf\n");
        printf("Example: downlf ~S1/test.txt ~S1/doc.c\n");
        return 0;
    }

    // Validate each file path and extension
    for (int i = 1; i < argc; i++) {
        if (!validate_file_path(argv[i])) {
            return 0;
        }

        const char* ext = get_file_extension(argv[i]);
        // FIXED: Added .zip support for download
        if (strcmp(ext, ".c") != 0 && strcmp(ext, ".pdf") != 0 &&
            strcmp(ext, ".txt") != 0 && strcmp(ext, ".zip") != 0) {
            printf("Error: Unsupported file type '%s' for download. Only .c, .pdf, .txt, .zip files allowed.\n", ext);
            return 0;
        }
    }

    // Construct and send command to server
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "downlf");
    for (int i = 1; i < argc; i++) {
        strcat(command, " ");
        strcat(command, argv[i]);
    }

    if (send(socket_fd, command, strlen(command), 0) < 0) {
        printf("Error: Failed to send command to server\n");
        return 0;
    }

    // Receive each requested file from server
    int success_count = 0;
    for (int i = 1; i < argc; i++) {
        if (receive_file_from_server(socket_fd, argv[i])) {
            success_count++;
        }
    }

    printf("Downloaded %d out of %d files successfully\n", success_count, argc - 1);
    return success_count > 0;
}

// Receives a single file from server and saves to local directory
int receive_file_from_server(int socket_fd, const char* filename) {
    char buffer[BUFFER_SIZE];
    FILE* file;
    long file_size;
    int bytes_received;
    
    // Extract just the filename from the full path
    char* filepath_copy = malloc(strlen(filename) + 1);
    strcpy(filepath_copy, filename);
    const char* base_filename = basename(filepath_copy);
    
    // Receive file size first
    if (recv(socket_fd, &file_size, sizeof(file_size), 0) <= 0) {
        printf("Error: Failed to receive file size for %s\n", base_filename);
        free(filepath_copy);
        return 0;
    }
    
    if (file_size == -1) {
        printf("Error: File %s not found on server\n", base_filename);
        free(filepath_copy);
        return 0;
    }
    
    // Open local file for writing
    file = fopen(base_filename, "wb");
    if (!file) {
        printf("Error: Cannot create file %s in current directory\n", base_filename);
        free(filepath_copy);
        return 0;
    }
    
    printf("Downloading %s (%ld bytes)...\n", base_filename, file_size);
    
    // Receive file data in chunks
    long bytes_remaining = file_size;
    while (bytes_remaining > 0) {
        int bytes_to_receive = (bytes_remaining < BUFFER_SIZE) ? bytes_remaining : BUFFER_SIZE;
        bytes_received = recv(socket_fd, buffer, bytes_to_receive, 0);
        
        if (bytes_received <= 0) {
            printf("Error: Connection lost while downloading %s\n", base_filename);
            fclose(file);
            remove(base_filename);
            free(filepath_copy);
            return 0;
        }
        
        fwrite(buffer, 1, bytes_received, file);
        bytes_remaining -= bytes_received;
    }
    
    fclose(file);
    printf("Successfully downloaded %s\n", base_filename);
    free(filepath_copy);
    return 1;
}

// Handles tar file download from server
int client_downltar(int socket_fd, int argc, char* argv[]) {
    // Validate number of arguments (command + filetype)
    if (argc != 2) {
        printf("Usage: downltar filetype\n");
        printf("Example: downltar .c\n");
        printf("Example: downltar .pdf\n");
        printf("Example: downltar .txt\n");
        return 0;
    }
    
    // Validate requested file type
    if (!validate_tar_filetype(argv[1])) {
        return 0;
    }
    
    // Construct and send command to server
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "downltar %s", argv[1]);
    
    if (send(socket_fd, command, strlen(command), 0) < 0) {
        printf("Error: Failed to send command to server\n");
        return 0;
    }
    
    // Determine tar filename based on file type
    const char* tarname;
    if (strcmp(argv[1], ".c") == 0) {
        tarname = "cfiles.tar";
    } else if (strcmp(argv[1], ".pdf") == 0) {
        tarname = "pdf.tar";
    } else if (strcmp(argv[1], ".txt") == 0) {
        tarname = "text.tar";
    } else {
        printf("Error: Invalid filetype\n");
        return 0;
    }
    
    // Receive tar file from server
    if (receive_tar_from_server(socket_fd, tarname)) {
        printf("Tar file downloaded successfully\n");
        return 1;
    } else {
        printf("Failed to download tar file\n");
        return 0;
    }
}

// Receives tar file from server and saves to local directory
int receive_tar_from_server(int socket_fd, const char* tarname) {
    char buffer[BUFFER_SIZE];
    FILE* file;
    long file_size;
    int bytes_received;
    
    // Receive file size first
    if (recv(socket_fd, &file_size, sizeof(file_size), 0) <= 0) {
        printf("Error: Failed to receive tar file size for %s\n", tarname);
        return 0;
    }
    
    if (file_size == -1) {
        printf("Error: Tar file %s could not be created on server\n", tarname);
        return 0;
    }
    
    // Open local file for writing
    file = fopen(tarname, "wb");
    if (!file) {
        printf("Error: Cannot create tar file %s in current directory\n", tarname);
        return 0;
    }
    
    printf("Downloading %s (%ld bytes)...\n", tarname, file_size);
    
    // Receive file data in chunks
    long bytes_remaining = file_size;
    while (bytes_remaining > 0) {
        int bytes_to_receive = (bytes_remaining < BUFFER_SIZE) ? bytes_remaining : BUFFER_SIZE;
        bytes_received = recv(socket_fd, buffer, bytes_to_receive, 0);
        
        if (bytes_received <= 0) {
            printf("Error: Connection lost while downloading %s\n", tarname);
            fclose(file);
            remove(tarname);
            return 0;
        }
        
        fwrite(buffer, 1, bytes_received, file);
        bytes_remaining -= bytes_received;
    }
    
    fclose(file);
    printf("Successfully downloaded %s\n", tarname);
    return 1;
}

// Validates removef command syntax and arguments
int validate_removef_command(const char* command) {
    char cmd[256], arg1[MAX_PATH_LEN], arg2[MAX_PATH_LEN];

    memset(cmd, 0, sizeof(cmd));
    memset(arg1, 0, sizeof(arg1));
    memset(arg2, 0, sizeof(arg2));

    // Parse command into components
    int args = sscanf(command, "%s %s %s", cmd, arg1, arg2);

    if (strcmp(cmd, "removef") != 0) {
        printf("Error: Invalid command '%s'. Use 'removef'\n", cmd);
        return 0;
    }

    if (args < 2 || args > 3) {
        printf("Error: Usage: removef ~S1/path/file1 [~S1/path/file2]\n");
        printf("       Min: 1 file, Max: 2 files\n");
        return 0;
    }

    // Validate first file path and extension
    if (strncmp(arg1, "~S1", 3) != 0) {
        printf("Error: File path must start with ~S1\n");
        return 0;
    }

    const char* ext1 = get_file_extension(arg1);
    // FIXED: Added .zip support to match upload/download functionality
    if (strcmp(ext1, ".c") != 0 && strcmp(ext1, ".pdf") != 0 &&
        strcmp(ext1, ".txt") != 0 && strcmp(ext1, ".zip") != 0) {
        printf("Error: Invalid file type '%s' for file %s. Supported: .c, .pdf, .txt, .zip\n", ext1, arg1);
        return 0;
    }

    // Validate second file if provided
    if (args == 3 && strlen(arg2) > 0) {
        if (strncmp(arg2, "~S1", 3) != 0) {
            printf("Error: File path must start with ~S1\n");
            return 0;
        }

        const char* ext2 = get_file_extension(arg2);
        // FIXED: Added .zip support to match upload/download functionality
        if (strcmp(ext2, ".c") != 0 && strcmp(ext2, ".pdf") != 0 &&
            strcmp(ext2, ".txt") != 0 && strcmp(ext2, ".zip") != 0) {
            printf("Error: Invalid file type '%s' for file %s. Supported: .c, .pdf, .txt, .zip\n", ext2, arg2);
            return 0;
        }
    }

    printf("removef command validation successful\n");
    return 1;
}

// Handles file removal request to server
int client_removef(int socket_fd, const char* command) {
    char response[BUFFER_SIZE * 4];
    
    // Send command to server
    printf("Sending removef command to server...\n");
    int cmd_len = strlen(command) + 1;
    if (send(socket_fd, command, cmd_len, 0) < 0) {
        perror("Failed to send command");
        return 0;
    }
    
    // Wait for and display server response
    printf("Waiting for server response...\n");
    memset(response, 0, sizeof(response));
    int bytes = recv(socket_fd, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        printf("Server response: %s\n", response);
        return 1;
    } else {
        printf("No response received from server\n");
        return 0;
    }
}

// Validates dispfnames command syntax
int validate_dispfnames_command(const char* command) {
    char cmd[256], path[MAX_PATH_LEN];
    
    memset(cmd, 0, sizeof(cmd));
    memset(path, 0, sizeof(path));
    
    // Parse command into components
    int args = sscanf(command, "%s %s", cmd, path);
    
    if (strcmp(cmd, "dispfnames") != 0) {
        printf("Error: Invalid command '%s'. Use 'dispfnames'\n", cmd);
        return 0;
    }
    
    if (args != 2) {
        printf("Error: Usage: dispfnames ~S1/path/to/directory\n");
        return 0;
    }
    
    // Validate path format
    if (strncmp(path, "~S1", 3) != 0) {
        printf("Error: Path must start with ~S1\n");
        return 0;
    }
    
    printf("dispfnames command validation successful\n");
    return 1;
}

// Handles directory listing request to server
int client_dispfnames(int socket_fd, const char* command) {
    char response[BUFFER_SIZE * 4];
    
    // Send command to server
    printf("Sending dispfnames command to server...\n");
    int cmd_len = strlen(command) + 1;
    if (send(socket_fd, command, cmd_len, 0) < 0) {
        perror("Failed to send command");
        return 0;
    }
    
    // Wait for and display server response
    printf("Waiting for server response...\n");
    memset(response, 0, sizeof(response));
    int bytes = recv(socket_fd, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        
        printf("\n=== File List ===\n");
        // Parse and display the file list nicely
        char* token = strtok(response, "\n");
        while (token != NULL) {
            if (strlen(token) > 0) {
                // Skip status messages, display only filenames
                if (strstr(token, "Files found") != NULL || 
                    strstr(token, "No files found") != NULL) {
                    printf("%s\n", token);
                } else if (strstr(token, "Error:") == NULL && 
                          strstr(token, "SUCCESS") == NULL) {
                    printf("%s\n", token);
                }
            }
            token = strtok(NULL, "\n");
        }
        printf("=================\n");
        return 1;
    } else {
        printf("No response received from server\n");
        return 0;
    }
}
