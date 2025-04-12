#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>         
#include <sys/stat.h>       
#include "multicast.h"
#include "sender.h"         // For chunk_header_t and CHUNK_SIZE
#include "receiver.h"


#define MAX_FILES 10

FileBuffer *file_buffers[MAX_FILES] = {0};

uint32_t compute_checksum(const unsigned char *data, size_t length) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum;
}

// get or create a FileBuffer for a given file_id
FileBuffer *get_file_buffer(int file_id, int total_chunks) {
    if (file_id < 0 || file_id >= MAX_FILES) {
        fprintf(stderr, "File id %d out of bounds (max %d).\n", file_id, MAX_FILES);
        return NULL;
    }
    if (file_buffers[file_id] == NULL) {
        FileBuffer *fb = (FileBuffer *)malloc(sizeof(FileBuffer));
        if (!fb) {
            perror("malloc");
            return NULL;
        }
        fb->file_id = file_id;
        fb->total_chunks = total_chunks;
        fb->chunks_received = 0;
        fb->chunks = (unsigned char **)calloc(total_chunks, sizeof(unsigned char *));
        fb->chunk_sizes = (int *)calloc(total_chunks, sizeof(int));
        fb->received = (int *)calloc(total_chunks, sizeof(int));
        if (!fb->chunks || !fb->chunk_sizes || !fb->received) {
            perror("calloc");
            free(fb);
            return NULL;
        }
        file_buffers[file_id] = fb;
    }
    return file_buffers[file_id];
}

// store chunk in correct file
void store_chunk(chunk_header_t header, unsigned char *data) {
    FileBuffer *fb = get_file_buffer(header.file_id, header.total_chunks);
    if (!fb)
        return;
    // Make sur the chunk isn't already stored
    if (header.seq_num < 0 || header.seq_num >= fb->total_chunks) {
        fprintf(stderr, "Invalid sequence number %d for file %d\n", header.seq_num, header.file_id);
        return;
    }
    if (fb->received[header.seq_num] == 0) {
        // copy chunk to buffer
        fb->chunks[header.seq_num] = (unsigned char *)malloc(header.data_size);
        if (!fb->chunks[header.seq_num]) {
            perror("malloc");
            return;
        }
        memcpy(fb->chunks[header.seq_num], data, header.data_size);
        fb->chunk_sizes[header.seq_num] = header.data_size;
        fb->received[header.seq_num] = 1;
        fb->chunks_received++;
        printf("Received file %d, chunk %d (%d/%d)\n", header.file_id, header.seq_num,
               fb->chunks_received, fb->total_chunks);
        
        // All chunks are received
        if (fb->chunks_received == fb->total_chunks) {
            printf("All chunks for file %d received. Reassembling file...\n", header.file_id);
            try_reassemble_file(header.file_id);
        }
    }
}

void try_reassemble_file(int file_id) {
    if (file_id < 0 || file_id >= MAX_FILES)
        return;
    FileBuffer *fb = file_buffers[file_id];
    if (!fb)
        return;
    if (fb->chunks_received != fb->total_chunks)
        return;
    
    #ifdef _WIN32
        _mkdir("received_files");
    #else
        mkdir("received_files", 0777);
    #endif

    //filename
    char filename[256];
    snprintf(filename, sizeof(filename), "received_files/file_%d", file_id);
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("fopen");
        return;
    }
    
    // write chunks in order.
    for (int i = 0; i < fb->total_chunks; i++) {
        if (fb->received[i])
            fwrite(fb->chunks[i], 1, fb->chunk_sizes[i], fp);
    }
    fclose(fp);
    printf("File %d reassembled and saved as %s\n", file_id, filename);
    
    free_file_buffer(fb);
    file_buffers[file_id] = NULL;
}

void free_file_buffer(FileBuffer *fb) {
    if (!fb)
        return;
    for (int i = 0; i < fb->total_chunks; i++) {
        if (fb->received[i] && fb->chunks[i]) {
            free(fb->chunks[i]);
        }
    }
    free(fb->chunks);
    free(fb->chunk_sizes);
    free(fb->received);
    free(fb);
}

// The main receiver loop.
int main() {
    // Initialize multicast receiver using an example multicast address and ports.
    mcast_t *m = multicast_init("239.0.0.1", 5000, 5000);
    multicast_setup_recv(m);
    printf("Receiver started. Listening for multicast data...\n");
    
    while (1) {
        int ready = multicast_check_receive(m);
        if (ready > 0) {
            // Allocate a buffer to hold a packet (header + data).
            unsigned char packet[sizeof(chunk_header_t) + CHUNK_SIZE];
            int n = multicast_receive(m, packet, sizeof(packet));
            if (n < (int)sizeof(chunk_header_t)) {
                continue;
            }
            // extract the header
            chunk_header_t header;
            memcpy(&header, packet, sizeof(chunk_header_t));
            unsigned char *data = packet + sizeof(chunk_header_t);
            
            //checksum.
            uint32_t chk = compute_checksum(data, header.data_size);
            if (chk != header.checksum) {
                fprintf(stderr, "Checksum mismatch for file %d, chunk %d\n", header.file_id, header.seq_num);
                continue;
            }
            
            store_chunk(header, data);
        }
        // Small sleep to avoid busy-looping.
        usleep(10000);
    }
    
    multicast_destroy(m);
    return 0;
}
