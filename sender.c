#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include "multicast.h"
#include "sender.h"
#include "common.h"

#define SLEEP_TIME 1000 // microseconds delay between initial sends
#define OVERALL_WAIT_SEC 1000      // overall maximum waiting time (seconds)
#define BATCH_CAPACITY 10000       // number of requests to accumulate before sending a batch
#define BATCH_WAIT_USEC 500        // pause time (microseconds) between batches
#define NAK_WAIT_SEC 12            // if 12 seconds elapse without any new NAK, assume transmission complete

#define FILE_LIST_CTRL   0xFFFFFFFF  // Marker for file list control message
#define FILE_MISSING_REQ 0xFFFFFFFE  // Marker for file missing request
#define FILE_HASH_CTRL   0xFFFFFFFD  // Marker for file hash control message
#define FILE_LIST_WAIT_SEC 2         // How long (in seconds) to wait after sending file list

uint32_t compute_checksum(const unsigned char *data, size_t length) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum;
}

uint32_t *construct_NAK(int num_chunks) {
    uint32_t *seq_num_array = malloc(num_chunks * sizeof(uint32_t));
    if (!seq_num_array) {
        fprintf(stderr, "Malloc fail - NAK construction\n");
        exit(EXIT_FAILURE);
    }
    return seq_num_array;
}

uint32_t compute_file_hash(const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("compute_file_hash fopen");
        return 0;
    }
    uint32_t hash = 0;
    unsigned char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        for (size_t i = 0; i < bytes; i++) {
            hash += buffer[i];
        }
    }
    fclose(fp);
    return hash;
}

void resend_file(const char *filename, uint32_t file_id, mcast_t *m) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen (resend_file)");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    int total_chunks = file_size / CHUNK_SIZE;
    if (file_size % CHUNK_SIZE != 0)
        total_chunks++;
    unsigned char **packets = malloc(total_chunks * sizeof(unsigned char *));
    size_t *packet_sizes = malloc(total_chunks * sizeof(size_t));
    if (!packets || !packet_sizes) {
        fprintf(stderr, "Memory allocation failed in resend_file\n");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    for (int seq = 0; seq < total_chunks; seq++) {
        unsigned char buffer[CHUNK_SIZE];
        size_t bytes_read = fread(buffer, 1, CHUNK_SIZE, fp);
        if (bytes_read == 0)
            break;
        chunk_header_t header;
        header.file_id = file_id;
        header.seq_num = seq;
        header.total_chunks = total_chunks;
        header.data_size = bytes_read;
        header.checksum = compute_checksum(buffer, bytes_read);
        size_t packet_size = sizeof(chunk_header_t) + bytes_read;
        unsigned char *packet = malloc(packet_size);
        if (!packet) {
            perror("malloc in resend_file");
            fclose(fp);
            exit(EXIT_FAILURE);
        }
        memcpy(packet, &header, sizeof(chunk_header_t));
        memcpy(packet + sizeof(chunk_header_t), buffer, bytes_read);
        packets[seq] = packet;
        packet_sizes[seq] = packet_size;
    }
    fclose(fp);
    // Transmit all the chunks for the file again
    for (int seq = 0; seq < total_chunks; seq++) {
        multicast_send(m, packets[seq], packet_sizes[seq]);
        usleep(SLEEP_TIME);
    }
    printf("Resent all %d chunks for file %s (file_id=%u)\n", total_chunks, filename, file_id);
    for (int i = 0; i < total_chunks; i++) {
        free(packets[i]);
    }
    free(packets);
    free(packet_sizes);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 [file2 ...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    mcast_t *m = multicast_init("239.0.0.1", 5000, 5000);
    multicast_setup_recv(m);
    // Store file names and file count for retransmission requests
    char **file_names = argv + 1;
    int num_files = argc - 1;
    // Process each file (file id = argv index - 1)
    for (int i = 0; i < num_files; i++) {
        FILE *fp = fopen(file_names[i], "rb");
        if (!fp) {
            perror("fopen");
            continue;
        }
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        int total_chunks = file_size / CHUNK_SIZE;
        if (file_size % CHUNK_SIZE != 0)
            total_chunks++;
        unsigned char **packets = malloc(total_chunks * sizeof(unsigned char *));
        size_t *packet_sizes = malloc(total_chunks * sizeof(size_t));
        if (!packets || !packet_sizes) {
            fprintf(stderr, "Memory allocation failed\n");
            fclose(fp);
            exit(EXIT_FAILURE);
        }
        for (int seq = 0; seq < total_chunks; seq++) {
            unsigned char buffer[CHUNK_SIZE];
            size_t bytes_read = fread(buffer, 1, CHUNK_SIZE, fp);
            if (bytes_read == 0)
                break;
            chunk_header_t header;
            header.file_id = i;
            header.seq_num = seq;
            header.total_chunks = total_chunks;
            header.data_size = bytes_read;
            header.checksum = compute_checksum(buffer, bytes_read);
            size_t packet_size = sizeof(chunk_header_t) + bytes_read;
            unsigned char *packet = malloc(packet_size);
            if (!packet) {
                perror("malloc");
                fclose(fp);
                exit(EXIT_FAILURE);
            }
            memcpy(packet, &header, sizeof(chunk_header_t));
            memcpy(packet + sizeof(chunk_header_t), buffer, bytes_read);
            packets[seq] = packet;
            packet_sizes[seq] = packet_size;
        }
        fclose(fp);
        // Send all chunks for the file
        for (int seq = 0; seq < total_chunks; seq++) {
            multicast_send(m, packets[seq], packet_sizes[seq]);
            usleep(SLEEP_TIME);
        }
        printf("Sent all %d chunks for file %s. Waiting for NAKs...\n", total_chunks, file_names[i]);
        int *requested = calloc(total_chunks, sizeof(int));
        if (!requested) {
            fprintf(stderr, "Failed to allocate retransmission tracking array.\n");
            exit(EXIT_FAILURE);
        }
        int request_count = 0;
        time_t overall_start = time(NULL);
        time_t last_nak_time = time(NULL);
        // Existing loop to answer chunk-specific NAKs
        while ((time(NULL) - overall_start) < OVERALL_WAIT_SEC) {
            int timeout_ms = 500;
            int ret = poll(m->fds, m->nfds, timeout_ms);
            if (ret < 0) {
                perror("poll");
                break;
            } else if (ret > 0) {
                unsigned char ctrl_buffer[1500];
                int bytes_received = recvfrom(m->sock, ctrl_buffer, sizeof(ctrl_buffer), 0, NULL, NULL);
                if (bytes_received < 0) {
                    perror("recvfrom");
                    continue;
                }
                // We expect NAK messages to be two 32-bit values
                if (bytes_received != (int)(2 * sizeof(uint32_t)))
                    continue;
                uint32_t file_id, seq;
                memcpy(&file_id, ctrl_buffer, sizeof(uint32_t));
                memcpy(&seq, ctrl_buffer + sizeof(uint32_t), sizeof(uint32_t));
                if (file_id != (uint32_t)i)
                    continue;
                if (seq >= (uint32_t)total_chunks) {
                    fprintf(stderr, "Received NAK for invalid chunk %u (total_chunks=%d)\n", seq, total_chunks);
                    continue;
                }
                if (!requested[seq]) {
                    requested[seq] = 1;
                    request_count++;
                    last_nak_time = time(NULL);
                    printf("Accumulated NAK for file %u, chunk %u (total accumulated: %d)\n", file_id, seq, request_count);
                }
            }
            if ((time(NULL) - last_nak_time) >= NAK_WAIT_SEC && request_count == 0) {
                printf("No NAK received in %d seconds; assuming file transmitted successfully.\n", NAK_WAIT_SEC);
                break;
            }
            if (request_count >= BATCH_CAPACITY ||
                (((time(NULL) - last_nak_time) >= NAK_WAIT_SEC) && request_count > 0)) {
                int batch_count = 0;
                for (int j = 0; j < total_chunks; j++) {
                    if (requested[j]) {
                        multicast_send(m, packets[j], packet_sizes[j]);
                        batch_count++;
                        requested[j] = 0;
                        usleep(SLEEP_TIME);
                    }
                }
                request_count = 0;
                printf("Batch retransmitted %d chunks for file %s\n", batch_count, file_names[i]);
                usleep(BATCH_WAIT_USEC);
                last_nak_time = time(NULL);
            }
        }
        // Final batch retransmission for any remaining requests
        int remaining = 0;
        for (int j = 0; j < total_chunks; j++) {
            if (requested[j]) {
                multicast_send(m, packets[j], packet_sizes[j]);
                remaining++;
            }
        }
        if (remaining > 0)
            printf("Final batch retransmitted %d remaining chunks for file %s\n", remaining, file_names[i]);
        free(requested);

        // Send file list control message as before
        uint32_t count = i + 1;
        size_t list_msg_size = sizeof(uint32_t) + sizeof(uint32_t) + count * sizeof(uint32_t);
        uint32_t *list_msg = malloc(list_msg_size);
        if (!list_msg) {
            perror("malloc file list message");
            exit(EXIT_FAILURE);
        }
        list_msg[0] = FILE_LIST_CTRL;  // Control marker
        list_msg[1] = count;
        for (uint32_t j = 0; j < count; j++) {
            list_msg[2 + j] = j;  // File IDs that have been sent
        }
        multicast_send(m, (unsigned char *)list_msg, list_msg_size);
        printf("Sent file list control message for files 0 to %u\n", i);
        free(list_msg);

        uint32_t file_hash = compute_file_hash(file_names[i]);
        uint32_t hash_packet[3];
        hash_packet[0] = FILE_HASH_CTRL; // marker for file hash control
        hash_packet[1] = i;             // file id
        hash_packet[2] = file_hash;       // computed hash value
        multicast_send(m, (unsigned char *)hash_packet, sizeof(hash_packet));
        printf("Sent file hash control message for file %s (file_id=%u, hash=%u)\n", file_names[i], i, file_hash);

        time_t file_list_start = time(NULL);
        int *resend_done = calloc(num_files, sizeof(int));
        while ((time(NULL) - file_list_start) < FILE_LIST_WAIT_SEC) {
            int timeout_ms = 500;
            int ret = poll(m->fds, m->nfds, timeout_ms);
            if(ret > 0) {
                unsigned char ctrl_buffer[1500];
                int bytes_received = recvfrom(m->sock, ctrl_buffer, sizeof(ctrl_buffer), 0, NULL, NULL);
                if (bytes_received >= (int)(2 * sizeof(uint32_t))) {
                    uint32_t req_file_id, req_type;
                    memcpy(&req_file_id, ctrl_buffer, sizeof(uint32_t));
                    memcpy(&req_type, ctrl_buffer + sizeof(uint32_t), sizeof(uint32_t));
                    if(req_type == FILE_MISSING_REQ && req_file_id < (uint32_t)num_files) {
                        if (!resend_done[req_file_id]) {
                            printf("Received file request for file %u, retransmitting\n", req_file_id);
                            resend_file(file_names[req_file_id], req_file_id, m);
                            // After retransmission, compute the file's hash again:
                            uint32_t file_hash = compute_file_hash(file_names[req_file_id]);
                            //   word0: FILE_HASH_CTRL marker
                            //   word1: file id
                            //   word2: file hash
                            uint32_t hash_packet[3];
                            hash_packet[0] = FILE_HASH_CTRL;
                            hash_packet[1] = req_file_id;
                            hash_packet[2] = file_hash;
                            multicast_send(m, (unsigned char *)hash_packet, sizeof(hash_packet));
                            printf("Sent file hash control message for file %u (hash=%u) after retransmission\n", req_file_id, file_hash);
                            resend_done[req_file_id] = 1;
                        }
                    }
                    
                }
            }
        }
        free(resend_done);
        // Free memory for the current file’s packets
        for (int k = 0; k < total_chunks; k++) {
            free(packets[k]);
        }
        free(packets);
        free(packet_sizes);
    }
    multicast_destroy(m);
    return 0;
}
