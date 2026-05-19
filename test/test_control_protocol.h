#ifndef TEST_CONTROL_PROTOCOL_H
#define TEST_CONTROL_PROTOCOL_H

/* Commands (coordinator → node) */
#define CTRL_STORE_FILE      "STORE_FILE"
#define CTRL_FETCH_FILE      "FETCH_FILE"
#define CTRL_PEER_ADD        "PEER_ADD"
#define CTRL_ADD_PEER        "ADD_PEER"
#define CTRL_CONNECT_RELAY   "CONNECT_RELAY"
#define CTRL_WAIT_FOR_PEER   "WAIT_FOR_PEER"
#define CTRL_STATUS          "STATUS"
#define CTRL_SHUTDOWN        "SHUTDOWN"
#define CTRL_FORCE_RELAY_ENDPOINT "FORCE_RELAY_ENDPOINT"

/* Event log and Hebbian query commands (OFFS_TEST only) */
#define CTRL_GET_EVENTS      "GET_EVENTS"
#define CTRL_CLEAR_EVENTS    "CLEAR_EVENTS"
#define CTRL_FIND_BLOCK      "FIND_BLOCK"
#define CTRL_PING_PEER       "PING_PEER"
#define CTRL_HEBBIAN         "HEBBIAN"
#define CTRL_RANK_BLOCK      "RANK_BLOCK"
#define CTRL_GOSSIP          "GOSSIP"
#define CTRL_CLOSEST_NODES   "CLOSEST_NODES"
#define CTRL_MEASURE_NODES   "MEASURE_NODES"

/* Additional RPC test commands (OFFS_TEST only) */
#define CTRL_PING_CAPACITY   "PING_CAPACITY"
#define CTRL_PING_BLOCK      "PING_BLOCK"
#define CTRL_FIND_NODE       "FIND_NODE"
#define CTRL_SEEKING_BLOCKS  "SEEKING_BLOCKS"
#define CTRL_RECALL_BLOCK    "RECALL_BLOCK"
#define CTRL_STORE_BLOCK     "STORE_BLOCK"
#define CTRL_SET_CAPACITY   "SET_CAPACITY"

/* Response prefixes for new commands */
#define CTRL_RESP_CLOSEST_NODES "CLOSEST_NODES_RESP"
#define CTRL_RESP_MEASURE_NODES "MEASURE_NODES_RESP"
#define CTRL_RESP_PING_CAPACITY  "PING_CAPACITY_RESP"
#define CTRL_RESP_PING_BLOCK     "PING_BLOCK_RESP"
#define CTRL_RESP_FIND_NODE      "FIND_NODE_RESP"
#define CTRL_RESP_SEEKING_BLOCKS "SEEKING_BLOCKS_RESP"
#define CTRL_RESP_RECALL         "RECALL_RESP"
#define CTRL_RESP_EVENTS     "EVENTS"
#define CTRL_RESP_HEBBIAN    "HEBBIAN_RESP"

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