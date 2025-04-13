#include <stdint.h>

typedef struct {
    uint32_t file_id;
    uint32_t seq_num;
    uint32_t total_chunks;
    uint32_t data_size;
    uint32_t checksum;
} chunk_header_t;

typedef struct {
    uint32_t file_id;  // Indicates which file this ACK is for
    uint32_t ack_seq;  // Cumulative ACK: highest contiguous sequence number received
} ack_packet_t;

#define CHUNK_SIZE 1024
#define WINDOW_SIZE 50