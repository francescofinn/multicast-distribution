#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include "multicast.h"
#include "sender.h"
#include "common.h"


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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 [file2 ...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    mcast_t *m = multicast_init("239.0.0.1", 5000, 5000); // Adjust IP and port later
    multicast_setup_recv(m);

    // Process each file BUG WAS HERE - argv[i] -> argv[i+1]
    for (int i = 0; i < argc - 1; i++) // -1 because the first argc is just the program name
    {
        FILE *fp = fopen(argv[i+1], "rb"); // Open in binary mode
        if (!fp) {
            perror("fopen");
            continue;
        }

        // Determine file size
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        // Calculate num total chunks needed
        int total_chunks = file_size / CHUNK_SIZE;
        if (file_size % CHUNK_SIZE != 0) { // Account for non-whole divisbility
            total_chunks++;
        }

         // Allocate arrays to store all packets (so they can be retransmitted if needed)
         unsigned char **packets = malloc(total_chunks * sizeof(unsigned char *));
         size_t *packet_sizes = malloc(total_chunks * sizeof(size_t));
         if (!packets || !packet_sizes) {
             fprintf(stderr, "Memory allocation failed\n");
             fclose(fp);
             exit(EXIT_FAILURE);
         }

        // Read and send each chunk
        for (int seq = 0; seq < total_chunks; seq++) {
            unsigned char buffer[CHUNK_SIZE];
            size_t bytes_read = fread(buffer, 1, CHUNK_SIZE, fp);
            if (bytes_read == 0) {
                break;
            }

            // Prepare chunk header
            chunk_header_t header;
            header.file_id = i;
            header.seq_num = seq;
            header.total_chunks =total_chunks;
            header.data_size = bytes_read;
            header.checksum = compute_checksum(buffer, bytes_read);

            // Allocate a packet buffer for the header and chunk data
            size_t packet_size = sizeof(chunk_header_t) + bytes_read;
            unsigned char *packet = malloc(packet_size);
            if (!packet) {
                perror("malloc");
                fclose(fp);
                exit(EXIT_FAILURE);
            }

            // Copy header and data into the packet buffer
            memcpy(packet, &header, sizeof(chunk_header_t));
            memcpy(packet + sizeof(chunk_header_t), buffer, bytes_read);

            packets[seq] = packet;
            packet_sizes[seq] = packet_size;
        }

        fclose(fp);

        // Initially send all chunks over multicast
        for (int seq = 0; seq < total_chunks; seq++) {
            multicast_send(m, packets[seq], packet_sizes[seq]);
        }
        printf("Sent all %d chunks for file %s. Waiting for NAKs...\n", total_chunks, argv[i+1]);

        // Listen for NAKs for a total of 10 seconds
        // Listen for NAKs for a total of 10 seconds
        const int total_wait_sec = 100;
        time_t start_time = time(NULL);
        while ((time(NULL) - start_time) < total_wait_sec) {
            int timeout_ms = 1000;  // Poll every 1 second
            int ret = poll(m->fds, m->nfds, timeout_ms);
            if (ret < 0) {
                perror("poll");
                break;
            } else if (ret == 0) {
                // No NAK received in this interval; keep waiting until overall time expires.
                continue;
            } else {
                // There is incoming data: expect an individual NAK packet of 8 bytes.
                unsigned char buffer[1500];
                int bytes_received = recvfrom(m->sock, buffer, sizeof(buffer), 0, NULL, NULL);
                if (bytes_received < 0) {
                    perror("recvfrom");
                    continue;
                }
                // Expect exactly 8 bytes (2 x uint32_t)
                if (bytes_received != (int)(2 * sizeof(uint32_t))) {
                    //fprintf(stderr, "NAK packet size mismatch; expected %zu, got %d\n", 2 * sizeof(uint32_t), bytes_received);
                    continue;
                }
                uint32_t file_id;
                uint32_t seq;
                memcpy(&file_id, buffer, sizeof(uint32_t));
                memcpy(&seq, buffer + sizeof(uint32_t), sizeof(uint32_t));

                // Verify that this NAK is for our current file.
                if (file_id != (uint32_t)i) {
                    continue;
                }
                if (seq >= (uint32_t)total_chunks) {
                    fprintf(stderr, "Received NAK for invalid chunk %u (total_chunks=%d)\n", seq, total_chunks);
                    continue;
                }
                printf("Received NAK for file %u, chunk %u. Resending...\n", file_id, seq);
                multicast_send(m, packets[seq], packet_sizes[seq]);
            }
        }

        // Free stored packet memory for this file
        for (int k = 0; k < total_chunks; k++) {
            free(packets[k]);
        }
        free(packets);
        free(packet_sizes);
    }

    multicast_destroy(m);
    return 0;
    
}