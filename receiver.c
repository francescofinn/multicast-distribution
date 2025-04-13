#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "multicast.h"
#include "sender.h" 

#define MAX_FILES 100
#define CACHE_LIMIT 10000
#define MAX_PACKET_SIZE (sizeof(chunk_header_t) + CHUNK_SIZE)
#define NUM_WORKER_THREADS 6
#define NAK_INACTIVITY_SECONDS 5
#define NAK_BATCH_SIZE 100   

static char output_folder[256];

typedef struct {
    int file_id;                
    int total_chunks;          
    int chunks_received;        
    unsigned char **chunks;    
    int *chunk_sizes;          
    int *received;              
    FILE *fp;                   
    pthread_mutex_t flush_mutex; 
    pthread_mutex_t fb_mutex;    
    int flush_in_progress;      
    int naks_sent;              
    time_t last_chunk_time;     
} FileBuffer;

FileBuffer *file_buffers[MAX_FILES] = {0};  // Global array of file buffers

typedef struct packet_node {
    int packet_length;
    unsigned char *data;
    struct packet_node *next;
} packet_node;

typedef struct {
    packet_node *head;
    packet_node *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} packet_queue;

packet_queue recv_queue;

void init_queue(packet_queue *q) {
    q->head = q->tail = NULL;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void enqueue_packet(packet_queue *q, int length, unsigned char *data) {
    packet_node *node = malloc(sizeof(packet_node));
    if (!node) {
        perror("malloc");
        return;
    }
    node->packet_length = length;
    node->data = data;
    node->next = NULL;
    
    pthread_mutex_lock(&q->mutex);
    if (q->tail == NULL) {
        q->head = q->tail = node;
    } else {
        q->tail->next = node;
        q->tail = node;
    }
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

packet_node* dequeue_packet(packet_queue *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->head == NULL) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    packet_node *node = q->head;
    q->head = node->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mutex);
    return node;
}

uint32_t compute_checksum(const unsigned char *data, size_t length) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum;
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

void *flush_thread_func(void *arg) {
    FileBuffer *fb = (FileBuffer *)arg;
    pthread_mutex_lock(&fb->fb_mutex);
    flush_all_chunks(fb);
    fb->flush_in_progress = 0;
    pthread_mutex_unlock(&fb->fb_mutex);
    return NULL;
}

// Modified: instead of always using "received_files", we now use output_folder.
FileBuffer *get_file_buffer(int file_id, int total_chunks) {
    if (file_id < 0 || file_id >= MAX_FILES) {
        fprintf(stderr, "File id %d out of bounds (max %d).\n", file_id, MAX_FILES);
        return NULL;
    }
    if (file_buffers[file_id] == NULL) {
        FileBuffer *fb = malloc(sizeof(FileBuffer));
        if (!fb) {
            perror("malloc");
            return NULL;
        }
        fb->file_id = file_id;
        fb->total_chunks = total_chunks;
        fb->chunks_received = 0;
        fb->flush_in_progress = 0;
        fb->naks_sent = 0;
        fb->last_chunk_time = time(NULL);
        fb->chunks = calloc(total_chunks, sizeof(unsigned char *));
        fb->chunk_sizes = calloc(total_chunks, sizeof(int));
        fb->received = calloc(total_chunks, sizeof(int)); // initialize all to 0.
        if (!fb->chunks || !fb->chunk_sizes || !fb->received) {
            perror("calloc");
            free(fb);
            return NULL;
        }
#ifdef _WIN32
        _mkdir(output_folder);
#else
        mkdir(output_folder, 0777);
#endif
        char filename[512];
        snprintf(filename, sizeof(filename), "%s/file_%d", output_folder, file_id);
        fb->fp = fopen(filename, "wb+"); // Open file for random-access writing.
        if (!fb->fp) {
            perror("fopen");
            free(fb->chunks);
            free(fb->chunk_sizes);
            free(fb->received);
            free(fb);
            return NULL;
        }
        pthread_mutex_init(&fb->flush_mutex, NULL);
        pthread_mutex_init(&fb->fb_mutex, NULL);
        file_buffers[file_id] = fb;
    }
    return file_buffers[file_id];
}

void free_file_buffer(FileBuffer *fb) {
    if (!fb)
        return;
    for (int i = 0; i < fb->total_chunks; i++) {
        if ((fb->received[i] == 0 || fb->received[i] == 1) && fb->chunks[i])
            free(fb->chunks[i]);
    }
    free(fb->chunks);
    free(fb->chunk_sizes);
    free(fb->received);
    fclose(fb->fp);
    pthread_mutex_destroy(&fb->flush_mutex);
    pthread_mutex_destroy(&fb->fb_mutex);
    free(fb);
}

// When a new chunk is stored, reset the file’s NAK state and update last_chunk_time.
void store_chunk(chunk_header_t header, unsigned char *data) {
    FileBuffer *fb = get_file_buffer(header.file_id, header.total_chunks);
    if (!fb)
        return;
    
    pthread_mutex_lock(&fb->fb_mutex);
    
    if (header.seq_num >= fb->total_chunks) {
        fprintf(stderr, "Invalid sequence number %d for file %d\n", header.seq_num, header.file_id);
        pthread_mutex_unlock(&fb->fb_mutex);
        return;
    }
    if (fb->received[header.seq_num] != 0) {  // duplicate chunk check
        pthread_mutex_unlock(&fb->fb_mutex);
        return;
    }
    
    fb->chunks[header.seq_num] = malloc(header.data_size);
    if (!fb->chunks[header.seq_num]) {
        perror("malloc");
        pthread_mutex_unlock(&fb->fb_mutex);
        return;
    }
    memcpy(fb->chunks[header.seq_num], data, header.data_size);
    fb->chunk_sizes[header.seq_num] = header.data_size;
    fb->received[header.seq_num] = 1;
    fb->chunks_received++;
    fb->naks_sent = 0;                    // new chunk received, so allow NAKs again
    fb->last_chunk_time = time(NULL);       // update timer
    
    if (fb->chunks_received % 10000 == 0 || fb->chunks_received == fb->total_chunks) {
        printf("Received file %d, chunk %d (%d/%d)\n", header.file_id, header.seq_num,
               fb->chunks_received, fb->total_chunks);
    }
    
    if (get_pending_flush_count(fb) >= CACHE_LIMIT && fb->flush_in_progress == 0) {
        fb->flush_in_progress = 1;
        pthread_t flush_thread;
        if (pthread_create(&flush_thread, NULL, flush_thread_func, fb) != 0) {
            perror("pthread_create");
            fb->flush_in_progress = 0;
        } else {
            pthread_detach(flush_thread);
        }
    }
    
    if (fb->chunks_received == fb->total_chunks) {
        while (fb->flush_in_progress) {
            pthread_mutex_unlock(&fb->fb_mutex);
            usleep(10000);
            pthread_mutex_lock(&fb->fb_mutex);
        }
        flush_all_chunks(fb);
        printf("File %d fully received and flushed to disk.\n", fb->file_id);
        pthread_mutex_unlock(&fb->fb_mutex);
        free_file_buffer(fb);
        file_buffers[header.file_id] = NULL;
        return;
    }
    
    pthread_mutex_unlock(&fb->fb_mutex);
}

void *worker_thread_func(void *arg) {
    (void)arg;
    while (1) {
        packet_node *node = dequeue_packet(&recv_queue);
        if (!node)
            continue;
        if (node->packet_length < (int)sizeof(chunk_header_t)) {
            free(node->data);
            free(node);
            continue;
        }
        chunk_header_t header;
        memcpy(&header, node->data, sizeof(chunk_header_t));
        unsigned char *data_ptr = node->data + sizeof(chunk_header_t);
        uint32_t chk = compute_checksum(data_ptr, header.data_size);
        if (chk != header.checksum) {
            fprintf(stderr, "Checksum mismatch for file %d, chunk %d\n",
                    header.file_id, header.seq_num);
            free(node->data);
            free(node);
            continue;
        }
        store_chunk(header, data_ptr);
        free(node->data);
        free(node);
    }
    return NULL;
}

void *receiver_thread_func(void *arg) {
    mcast_t *m = (mcast_t *)arg;
    while (1) {
        unsigned char *packet = malloc(MAX_PACKET_SIZE);
        if (!packet) {
            perror("malloc");
            continue;
        }
        int n = multicast_receive(m, packet, MAX_PACKET_SIZE);
        if (n < (int)sizeof(chunk_header_t)) {
            free(packet);
            continue;
        }
        enqueue_packet(&recv_queue, n, packet);
    }
    return NULL;
}

void send_missing_naks(mcast_t *m, FileBuffer *fb) {
    int sent_count = 0;
    for (int j = 0; j < fb->total_chunks; j++) {
        if (fb->received[j] == 0) {  // missing chunk
            uint32_t nak_packet[2];
            nak_packet[0] = fb->file_id;
            nak_packet[1] = j;
            multicast_send(m, (unsigned char *)nak_packet, sizeof(nak_packet));
            sent_count++;
            if (sent_count % NAK_BATCH_SIZE == 0) {
                pthread_mutex_unlock(&fb->fb_mutex);
                usleep(100000); // 100ms pause between batches
                pthread_mutex_lock(&fb->fb_mutex);
            }
        }
    }
    if (sent_count > 0) {
        printf("Sent %d NAKs for missing chunks in file %d\n", sent_count, fb->file_id);
        fb->naks_sent = 1;  // Mark that NAKs have been sent until new chunks arrive.
    }
}

void *nak_sender_thread_func(void *arg) {
    mcast_t *m = (mcast_t *)arg;
    while (1) {
        sleep(1);
        time_t now = time(NULL);
        for (int i = 0; i < MAX_FILES; i++) {
            FileBuffer *fb = file_buffers[i];
            if (fb == NULL)
                continue;
            pthread_mutex_lock(&fb->fb_mutex);
            if (fb->chunks_received > 0 && fb->chunks_received < fb->total_chunks) {
                if ((now - fb->last_chunk_time) >= NAK_INACTIVITY_SECONDS && fb->naks_sent == 0) {
                    printf("File %d inactive for %d seconds, sending NAKs...\n", fb->file_id, NAK_INACTIVITY_SECONDS);
                    send_missing_naks(m, fb);
                }
            }
            pthread_mutex_unlock(&fb->fb_mutex);
        }
    }
    return NULL;
}

int main() {
    mcast_t *m = multicast_init("239.0.0.1", 5000, 5000);
    int rcvbuf = 4 * 1024 * 1024; // 4 MB
    if (setsockopt(m->sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("setsockopt SO_RCVBUF");
    }
    multicast_setup_recv(m);
    printf("Receiver started. Listening for multicast data...\n");

    // Set up a unique output folder for this receiver using its process ID.
    snprintf(output_folder, sizeof(output_folder), "received_files_%d", getpid());
#ifdef _WIN32
    _mkdir(output_folder);
#else
    mkdir(output_folder, 0777);
#endif

    init_queue(&recv_queue);
    
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receiver_thread_func, m) != 0) {
        perror("pthread_create receiver_thread");
        exit(1);
    }
    
    pthread_t workers[NUM_WORKER_THREADS];
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread_func, NULL) != 0) {
            perror("pthread_create worker_thread");
            exit(1);
        }
    }
    
    pthread_t nak_thread;
    if (pthread_create(&nak_thread, NULL, nak_sender_thread_func, m) != 0) {
        perror("pthread_create nak_sender_thread");
        exit(1);
    }
    
    pthread_join(recv_thread, NULL);
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        pthread_join(workers[i], NULL);
    }
    pthread_join(nak_thread, NULL);
    
    multicast_destroy(m);
    return 0;
}
