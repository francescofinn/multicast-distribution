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

// Global array for active file buffers.
FileBuffer *file_buffers[MAX_FILES] = {0};

// Compute a simple additive checksum.
uint32_t compute_checksum(const unsigned char *data, size_t length) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum;
}

// Offload flush operation in a separate thread.
void *flush_thread_func(void *arg) {
    FileBuffer *fb = (FileBuffer *)arg;
    flush_all_chunks(fb);
    // Mark flush complete.
    pthread_mutex_lock(&fb->flush_mutex);
    fb->flush_in_progress = 0;
    pthread_mutex_unlock(&fb->flush_mutex);
    return NULL;
}

// Flush all pending chunks from the FileBuffer to disk.
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
            fb->received[i] = 2;  // Mark as flushed.
            flushedCount++;
        }
    }
    if (flushedCount > 0)
        printf("Flushed %d chunks for file %d\n", flushedCount, fb->file_id);
    fflush(fb->fp);
}

// Free the FileBuffer and its allocated resources.
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

// Get or create a FileBuffer for a given file_id; open the output file for random-access writes.
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
        fb->flush_in_progress = 0;
        fb->chunks = (unsigned char **)calloc(total_chunks, sizeof(unsigned char *));
        fb->chunk_sizes = (int *)calloc(total_chunks, sizeof(int));
        fb->received = (int *)calloc(total_chunks, sizeof(int));  // initially all 0
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
        fb->fp = fopen(filename, "wb+");  // Random-access writing.
        if (!fb->fp) {
            perror("fopen");
            free(fb->chunks);
            free(fb->chunk_sizes);
            free(fb->received);
            free(fb);
            return NULL;
        }
        pthread_mutex_init(&fb->flush_mutex, NULL);
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

// Store an incoming chunk in memory and offload flush if needed.
void store_chunk(chunk_header_t header, unsigned char *data) {
    FileBuffer *fb = get_file_buffer(header.file_id, header.total_chunks);
    if (!fb)
        return;
    if (header.seq_num >= fb->total_chunks) {
        fprintf(stderr, "Invalid sequence number %d for file %d\n", header.seq_num, header.file_id);
        return;
    }
    // Ignore duplicate chunks.
    if (fb->received[header.seq_num] != 0)
        return;
    
    fb->chunks[header.seq_num] = (unsigned char *)malloc(header.data_size);
    if (!fb->chunks[header.seq_num]) {
        perror("malloc");
        return;
    }
    memcpy(fb->chunks[header.seq_num], data, header.data_size);
    fb->chunk_sizes[header.seq_num] = header.data_size;
    fb->received[header.seq_num] = 1;  // Mark as buffered.
    fb->chunks_received++;
    printf("Received file %d, chunk %d (%d/%d)\n", header.file_id, header.seq_num,
           fb->chunks_received, fb->total_chunks);
    
    // If too many chunks are pending flush, offload flushing to a separate thread.
    if (get_pending_flush_count(fb) >= CACHE_LIMIT) {
        pthread_mutex_lock(&fb->flush_mutex);
        if (!fb->flush_in_progress) {
            fb->flush_in_progress = 1;
            pthread_t thread;
            if (pthread_create(&thread, NULL, flush_thread_func, fb) != 0) {
                perror("pthread_create");
                fb->flush_in_progress = 0;
            } else {
                pthread_detach(thread);
            }
        }
        pthread_mutex_unlock(&fb->flush_mutex);
    }
    
    // If all chunks have been received, perform a final flush.
    if (fb->chunks_received == fb->total_chunks) {
        pthread_mutex_lock(&fb->flush_mutex);
        if (fb->flush_in_progress) {
            pthread_mutex_unlock(&fb->flush_mutex);
            sleep(1);
            pthread_mutex_lock(&fb->flush_mutex);
        }
        flush_all_chunks(fb);
        pthread_mutex_unlock(&fb->flush_mutex);
        
        printf("File %d fully received and flushed to disk.\n", fb->file_id);
        fclose(fb->fp);
        pthread_mutex_destroy(&fb->flush_mutex);
        free_file_buffer(fb);
        file_buffers[header.file_id] = NULL;
    }
}

// --- NAK Control ---
// Send individual 8-byte NAK packets for all missing chunks in the FileBuffer.
// Each NAK packet consists of 2 uint32_t values: [file_id, missing_chunk_seq]
void send_individual_naks(FileBuffer *fb, mcast_t *m) {
    for (int i = 0; i < fb->total_chunks; i++) {
        if (fb->received[i] == 0) { // missing chunk
            uint32_t nak_packet[2];
            nak_packet[0] = fb->file_id;
            nak_packet[1] = i;
            multicast_send(m, (unsigned char *)nak_packet, sizeof(nak_packet));
            printf("Sent NAK for file %u, missing chunk %d\n", fb->file_id, i);
        }
    }
}

int main() {
    mcast_t *m = multicast_init("239.0.0.1", 5000, 5000);
    multicast_setup_recv(m);
    printf("Receiver started. Listening for multicast data...\n");
    
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
                fprintf(stderr, "Checksum mismatch for file %d, chunk %d\n",
                        header.file_id, header.seq_num);
                continue;
            }
            store_chunk(header, data);
        }
        usleep(10000);  // Sleep for 10ms.
    }
    
    multicast_destroy(m);
    return 0;
}