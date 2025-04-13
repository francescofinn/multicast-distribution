#include <stdint.h>

typedef enum {
    PKT_DATA,
    PKT_NAK
} packet_type_t;

typedef struct {
    packet_type_t type;
    uint32_t file_id;
    uint32_t seq_num; 
} control_packet_t;