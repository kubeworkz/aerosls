#define DSPP_MAGIC 0x534c534e45544d41 // "SLSNETMA"

enum DSPPOpcode {
    DSPP_PAGE_READ_REQ  = 1, // Request a page frame from a remote node's RAM
    DSPP_PAGE_READ_ACK  = 2, // Contains the requested 4KB data frame payload
    DSPP_PAGE_WRITE_REQ = 3, // Push a dirty page replication packet to a mirror backup node
    DSPP_PAGE_WRITE_ACK = 4  // Replication acknowledgment confirmation
};

struct DSPPPacketHeader {
    uint64_t magic;
    uint64_t system_object_id;
    uint64_t virtual_address;
    uint32_t transaction_id;
    uint16_t opcode;         // DSPPOpcode
    uint16_t node_source_id;
} __attribute__((packed));

struct DSPPFullPagePacket {
    struct DSPPPacketHeader header;
    uint8_t                 payload_4kb[4096]; // The actual page contents
} __attribute__((packed));