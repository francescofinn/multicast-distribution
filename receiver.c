#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "multicast.h"
#include "sender.h"
#include "receiver.h"

#define MAX_FILES 100

// Global array to keep track of file buffers (one per file_id)
FileBuffer *file_buffers[MAX_FILES] = {0};

uint32_t compute_checksum(const unsigned char *data, size_t length) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum;
}

// Get or create a FileBuffer for a given file_id; open the output file for random-access writes
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
        fb->received = (int *)calloc(total_chunks, sizeof(int)); // initially all 0
        if (!fb->chunks || !fb->chunk_sizes || !fb->received) {
            perror("calloc");
            free(fb);
            return NULL;
        }
    #ifdef _WIN32
        _mkdir("received_files");
    #else
        mkdir("received_files", 0777);
    #endif
        char filename[256];
        snprintf(filename, sizeof(filename), "received_files/file_%d", file_id);
        fb->fp = fopen(filename, "wb+");  // random-access writing
        if (!fb->fp) {
            perror("fopen");
            free(fb->chunks);
            free(fb->chunk_sizes);
            free(fb->received);
            free(fb);
            return NULL;
        }
        file_buffers[file_id] = fb;
    }
    return file_buffers[file_id];
}

int get_pending_flush_count(FileBuffer *fb) {
    int count = 0;
    for (int i = 0; i < fb->total_chunks; i++) {
        if (fb->received[i] == 1)
            count++;
    }
    return count;
}

void flush_all_chunks(FileBuffer *fb) {
    if (!fb || !fb->fp)
        return;
    int flushedCount = 0;
    for (int i = 0; i < fb->total_chunks; i++) {
        if (fb->received[i] == 1 && fb->chunks[i] != NULL) {
            long offset = i * CHUNK_SIZE;
            if (fseek(fb->fp, offset, SEEK_SET) != 0) {
                perror("fseek");
                continue;
            }
            if (fwrite(fb->chunks[i], 1, fb->chunk_sizes[i], fb->fp) != (size_t)fb->chunk_sizes[i]) {
                perror("fwrite");
                continue;
            }
            free(fb->chunks[i]);
            fb->chunks[i] = NULL;
            fb->received[i] = 2;  // mark as flushed
            flushedCount++;
        }
    }
    if (flushedCount > 0)
        printf("Flushed %d non-contiguous chunks for file %d\n", flushedCount, fb->file_id);
    fflush(fb->fp);
}

// Store an incoming chunk in RAM, flush to disk if cache is full
void store_chunk(chunk_header_t header, unsigned char *data) {
    FileBuffer *fb = get_file_buffer(header.file_id, header.total_chunks);
    if (!fb)
        return;
    // Validate sequence number (header.seq_num is unsigned, so no negative check necessary)
    if (header.seq_num >= fb->total_chunks) {
        fprintf(stderr, "Invalid sequence number %d for file %d\n", header.seq_num, header.file_id);
        return;
    }
    // If already received (flag 1 or 2) -> ignore duplicate retransmission
    if (fb->received[header.seq_num] != 0)
        return;
    
    fb->chunks[header.seq_num] = (unsigned char *)malloc(header.data_size);
    if (!fb->chunks[header.seq_num]) {
        perror("malloc");
        return;
    }
    memcpy(fb->chunks[header.seq_num], data, header.data_size);
    fb->chunk_sizes[header.seq_num] = header.data_size;
    fb->received[header.seq_num] = 1;  // mark as buffered
    fb->chunks_received++;
    printf("Received file %d, chunk %d (%d/%d)\n", 
           header.file_id, header.seq_num, fb->chunks_received, fb->total_chunks);
    
    if (get_pending_flush_count(fb) >= CACHE_LIMIT) {
        flush_all_chunks(fb);
    }
    
    // If all chunks have been received, flush any remaining chunks and finalize the file
    if (fb->chunks_received == fb->total_chunks) {
        flush_all_chunks(fb);
        printf("File %d fully received and flushed to disk.\n", fb->file_id);
        fclose(fb->fp);
        free_file_buffer(fb);
        file_buffers[header.file_id] = NULL;
    }
}

void free_file_buffer(FileBuffer *fb) {
    if (!fb)
        return;
    for (int i = 0; i < fb->total_chunks; i++) {
        if ((fb->received[i] == 0 || fb->received[i] == 1) && fb->chunks[i]) {
            free(fb->chunks[i]);
        }
    }
    free(fb->chunks);
    free(fb->chunk_sizes);
    free(fb->received);
    free(fb);
}

// Send individual NAK packets (one per missing chunk). Each NAK is 8 bytes: file_id and missing chunk seq number.
void send_individual_naks(FileBuffer *fb, mcast_t *m) {
    for (int i = 0; i < fb->total_chunks; i++) {
        if (fb->received[i] == 0) { // missing chunk
            uint32_t nak_packet[2];
            nak_packet[0] = fb->file_id;
            nak_packet[1] = i;
            multicast_send(m, (unsigned char *)nak_packet, sizeof(nak_packet));
            //printf("Sent NAK for file %u, missing chunk %d\n", fb->file_id, i);
        }
    }
}

int main() {
    mcast_t *m = multicast_init("239.0.0.1", 5000, 5000);
    multicast_setup_recv(m);
    printf("Receiver started. Listening for multicast data...\n");

    // Track the last time NAKs were sent.
    time_t last_nak_time = time(NULL);
    const int NAK_INTERVAL = 10;  // seconds

    while (1) {
        int ready = multicast_check_receive(m);
        if (ready > 0) {
            unsigned char packet[sizeof(chunk_header_t) + CHUNK_SIZE];
            int n = multicast_receive(m, packet, sizeof(packet));
            if (n < (int)sizeof(chunk_header_t)) {
                continue;
            }
            chunk_header_t header;
            memcpy(&header, packet, sizeof(chunk_header_t));
            unsigned char *data = packet + sizeof(chunk_header_t);
            
            uint32_t chk = compute_checksum(data, header.data_size);
            if (chk != header.checksum) {
                fprintf(stderr, "Checksum mismatch for file %d, chunk %d\n", header.file_id, header.seq_num);
                continue;
            }
            
            store_chunk(header, data);
        }
        usleep(10000);  // sleep for 10ms

        // Every NAK_INTERVAL seconds, send individual NAKs for incomplete files.
        time_t now = time(NULL);
        if (now - last_nak_time >= NAK_INTERVAL) {
            for (int id = 0; id < MAX_FILES; id++) {
                if (file_buffers[id] != NULL) {
                    FileBuffer *fb = file_buffers[id];
                    if (fb->chunks_received < fb->total_chunks) {
                        send_individual_naks(fb, m);
                    }
                }
            }
            last_nak_time = now;
        }
    }
    
    multicast_destroy(m);
    return 0;
}
