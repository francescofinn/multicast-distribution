#include <stdint.h>

typedef struct {
    uint32_t file_id;
    uint32_t seq_num;
    uint32_t total_chunks;
    uint32_t data_size;
    uint32_t checksum;
} chunk_header_t;

#define CHUNK_SIZE 1024