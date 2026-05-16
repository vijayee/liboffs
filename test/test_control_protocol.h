#ifndef TEST_CONTROL_PROTOCOL_H
#define TEST_CONTROL_PROTOCOL_H

/* Commands (coordinator → node) */
#define CTRL_STORE_FILE      "STORE_FILE"
#define CTRL_FETCH_FILE      "FETCH_FILE"
#define CTRL_PEER_ADD        "PEER_ADD"
#define CTRL_CONNECT_RELAY   "CONNECT_RELAY"
#define CTRL_WAIT_FOR_PEER   "WAIT_FOR_PEER"
#define CTRL_STATUS          "STATUS"
#define CTRL_SHUTDOWN        "SHUTDOWN"

/* Response prefixes (node → coordinator) */
#define CTRL_RESP_OK         "OK"
#define CTRL_RESP_HASH       "HASH"
#define CTRL_RESP_DATA       "DATA"
#define CTRL_RESP_STATUS     "STATUS"
#define CTRL_RESP_ERROR      "ERROR"

/* Block type enum strings for parsing */
#define CTRL_BLOCK_NANO      "0"
#define CTRL_BLOCK_MINI     "1"
#define CTRL_BLOCK_STANDARD  "2"
#define CTRL_BLOCK_MEGA     "3"

/* Default timeouts (ms) */
#define CTRL_HANDSHAKE_TIMEOUT_MS  5000
#define CTRL_TRANSFER_TIMEOUT_MS  10000
#define CTRL_POLL_INTERVAL_MS     200

/* HASH response format: HASH <descriptor_hash_hex> <file_hash_hex> <final_byte> <stored_checksum_hex> */

#endif