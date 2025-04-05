#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "multicast.h"
#include "sender.h"

uint32_t compute_checksum(const unsigned char *data, size_t length) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum += data[i];
    }

    return checksum;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 [file2 ...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    mcast_t *m = multicast_init("0.0.0.0", 5000, 5000); // Adjust IP and port later

    // Process each file 
    for (int i = 0; i < argc; i++)
    {
        FILE *fp = fopen(argv[i], "rb"); // Open in binary mode
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

            // Send packet over multicast
            multicast_send(m, packet, packet_size);

            free(packet);

            // TODO: (optional) delay, log stats, handle retransmission requests
        }

        fclose(fp);
    }

    multicast_destroy(m);
    return 0;
    
}