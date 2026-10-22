//
// Regression: zero-id ring entries are routing poison (self-propagate via
// gossip; once gossip rewards lift their weight past the gate, FindBlock
// routes finds to an empty node id). ring_set_insert must reject them.
//
#include <gtest/gtest.h>
extern "C" {
#include "../src/Network/ring_set.h"
#include "../src/Network/net_node.h"
#include "../src/Network/node_id.h"
}

TEST(RingSetZeroId, InsertRejectsZeroIdNode) {
  ring_set_t* set = ring_set_create(8, 4, 2);
  ASSERT_NE(set, nullptr);
  net_node_t* node = net_node_create_unidentified(0xC0A80101u, 23401);
  ASSERT_NE(node, nullptr);
  EXPECT_TRUE(node_id_is_null(&node->id));
  EXPECT_EQ(ring_set_insert(set, node, 0), -2);
  EXPECT_EQ(ring_set_find_by_id(set, &node->id), nullptr);
  net_node_destroy(node);
  ring_set_destroy(set);
}

TEST(RingSetZeroId, InsertAcceptsRealIdNode) {
  ring_set_t* set = ring_set_create(8, 4, 2);
  ASSERT_NE(set, nullptr);
  node_id_t id;
  ASSERT_EQ(node_id_from_public_key((const uint8_t*)"ring-zero-id-key", 16, &id), 0);
  net_node_t* node = net_node_create(&id, 0xC0A80101u, 23401);
  ASSERT_NE(node, nullptr);
  EXPECT_FALSE(node_id_is_null(&node->id));
  EXPECT_EQ(ring_set_insert(set, node, 0), 0);
  net_node_t* found = ring_set_find_by_id(set, &id);
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(node_id_equals(&found->id, &id));
  net_node_destroy(node);
  ring_set_destroy(set);
}
