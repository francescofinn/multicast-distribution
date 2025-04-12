#ifndef RECEIVER_H
#define RECEIVER_H

#include <stdint.h>

// buffering chunks from 1 file
typedef struct {
    int file_id;          
    int total_chunks;     
    int chunks_received;  
    unsigned char **chunks;
    int *chunk_sizes;        
    int *received;           
} FileBuffer;

void store_chunk(chunk_header_t header, unsigned char *data);
void try_reassemble_file(int file_id);
void free_file_buffer(FileBuffer *fb);

#endif
