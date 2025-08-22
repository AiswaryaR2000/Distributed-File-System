# Distributed File System

## Overview
A distributed file system with 4 servers (S1, S2, S3, S4) that automatically distributes files by type. Clients connect only to S1.

## Tech Stack
- **Language**: C
- **Communication**: TCP Socket Programming
- **Process Management**: Fork() for multi-client handling
- **File Operations**: Standard C file I/O
- **Platform**: Linux/Unix

## File Distribution
- **S1**: Stores `.c` files in `~/S1`
- **S2**: Stores `.pdf` files in `~/S2` 
- **S3**: Stores `.txt` files in `~/S3`
- **S4**: Stores `.zip` files in `~/S4`

## Setup
1. Create directories: `mkdir ~/S1 ~/S2 ~/S3 ~/S4` on respective machines
2. Compile: `gcc -o server_name server_name.c` and `gcc -o s25client s25client.c`
3. Start servers: S2, S3, S4 first, then S1, then client

## Commands

### Upload (1-3 files)
```
uploadf file1 [file2] [file3] destination_path
```
Example: `uploadf sample.c test.pdf ~/S1/folder1`

### Download (1-2 files)
```
downlf file1 [file2]
```
Example: `downlf ~/S1/folder1/sample.c`

### Remove (1-2 files)
```
removef file1 [file2]
```
Example: `removef ~/S1/folder1/sample.pdf`

### Download Tar
```
downltar filetype
```
Example: `downltar .c` (creates cfiles.tar)

### List Files
```
dispfnames pathname
```
Example: `dispfnames ~/S1/folder1`

## Notes
- Clients only communicate with S1
- Files are automatically routed to correct servers
- Supports `.c`, `.pdf`, `.txt`, `.zip` files only
