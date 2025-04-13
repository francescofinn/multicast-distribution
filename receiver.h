#ifndef RECEIVER_H
#define RECEIVER_H

#include <stdint.h>
#include<pthread.h>

typedef struct {
    int file_id;          
    int total_chunks;     
    int chunks_received;  
    unsigned char **chunks;
    int fin_sent;
    int *chunk_sizes;        
    // 0: not received, 1: received (in memory), 2: flushed to disk
    int *received;           
    FILE *fp;               // file pointer for output (random access)
    pthread_mutex_t flush_mutex;  // to protect flush operations
    int flush_in_progress;        // 0 = no flush in progress, 1 = flush running
} FileBuffer;

void store_chunk(chunk_header_t header, unsigned char *data);
void try_reassemble_file(int file_id);
void free_file_buffer(FileBuffer *fb);
void flush_all_chunks(FileBuffer *fb);
int get_pending_flush_count(FileBuffer *fb);

#endif
