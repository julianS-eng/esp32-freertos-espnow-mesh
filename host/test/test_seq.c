/*
 * SPDX-License-Identifier: MIT
 * Sequence arithmetic and the receiver-side tracker.
 */
#include "mesh_protocol.h"
#include "mesh_seq.h"
#include "unity.h"

static mesh_seq_tracker_t t;

void setUp(void)
{
    mesh_seq_tracker_reset(&t);
}
void tearDown(void) {}

static void test_serial_arithmetic_wraps(void)
{
    TEST_ASSERT_EQUAL_INT32(1, mesh_seq_diff(0, 0xFFFF));
    TEST_ASSERT_EQUAL_INT32(-1, mesh_seq_diff(0xFFFF, 0));
    TEST_ASSERT_EQUAL_INT32(10, mesh_seq_diff(5, 0xFFFB));
    TEST_ASSERT_TRUE(mesh_seq_newer(0, 0xFFFF));
    TEST_ASSERT_FALSE(mesh_seq_newer(0xFFFF, 0));
    TEST_ASSERT_FALSE(mesh_seq_newer(7, 7));
    TEST_ASSERT_TRUE(mesh_seq_newer(0x7FFF, 0));
    TEST_ASSERT_FALSE(mesh_seq_newer(0x8001, 0)); /* more than half the space ahead = behind */
}

static void test_in_order_stream_has_no_loss(void)
{
    for (uint16_t s = 100; s < 200; s++) {
        TEST_ASSERT_EQUAL_INT(MESH_SEQ_NEW, mesh_seq_tracker_update(&t, s, NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(100, t.received);
    TEST_ASSERT_EQUAL_UINT32(0, t.lost);
    TEST_ASSERT_EQUAL_UINT32(0, mesh_seq_loss_ppm(&t));
}

static void test_gap_counts_loss(void)
{
    uint32_t gap = 99;
    mesh_seq_tracker_update(&t, 10, &gap);
    TEST_ASSERT_EQUAL_UINT32(0, gap);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_NEW, mesh_seq_tracker_update(&t, 14, &gap));
    TEST_ASSERT_EQUAL_UINT32(3, gap);
    TEST_ASSERT_EQUAL_UINT32(3, t.lost);
    TEST_ASSERT_EQUAL_UINT32(2, t.received);
    TEST_ASSERT_EQUAL_UINT32(600000, mesh_seq_loss_ppm(&t)); /* 3 / (2 + 3) */
}

static void test_duplicate_is_detected_not_counted(void)
{
    mesh_seq_tracker_update(&t, 1, NULL);
    mesh_seq_tracker_update(&t, 2, NULL);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_DUPLICATE, mesh_seq_tracker_update(&t, 2, NULL));
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_DUPLICATE, mesh_seq_tracker_update(&t, 1, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, t.received);
    TEST_ASSERT_EQUAL_UINT32(2, t.duplicates);
}

static void test_late_frame_fills_gap(void)
{
    mesh_seq_tracker_update(&t, 1, NULL);
    mesh_seq_tracker_update(&t, 4, NULL); /* 2,3 presumed lost */
    TEST_ASSERT_EQUAL_UINT32(2, t.lost);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_LATE, mesh_seq_tracker_update(&t, 3, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, t.lost);
    TEST_ASSERT_EQUAL_UINT32(1, t.reordered);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_DUPLICATE, mesh_seq_tracker_update(&t, 3, NULL));
}

static void test_wraparound_is_seamless(void)
{
    uint32_t gap;
    mesh_seq_tracker_update(&t, 0xFFFD, NULL);
    mesh_seq_tracker_update(&t, 0xFFFE, NULL);
    mesh_seq_tracker_update(&t, 0xFFFF, NULL);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_NEW, mesh_seq_tracker_update(&t, 0x0000, &gap));
    TEST_ASSERT_EQUAL_UINT32(0, gap);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_NEW, mesh_seq_tracker_update(&t, 0x0002, &gap));
    TEST_ASSERT_EQUAL_UINT32(1, gap);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_DUPLICATE, mesh_seq_tracker_update(&t, 0xFFFF, NULL));
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_LATE, mesh_seq_tracker_update(&t, 0x0001, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, t.lost);
}

static void test_full_wrap_many_times(void)
{
    for (uint32_t i = 0; i < 3u * 65536u; i++) {
        TEST_ASSERT_EQUAL_INT(MESH_SEQ_NEW, mesh_seq_tracker_update(&t, (uint16_t)i, NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(3u * 65536u, t.received);
    TEST_ASSERT_EQUAL_UINT32(0, t.lost);
}

static void test_large_forward_jump_clears_window(void)
{
    mesh_seq_tracker_update(&t, 10, NULL);
    mesh_seq_tracker_update(&t, 10 + 200, NULL);
    TEST_ASSERT_EQUAL_UINT32(199, t.lost);
    /* 11 is now 199 behind: outside the window -> treated as a sender restart */
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_RESET, mesh_seq_tracker_update(&t, 11, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, t.resets);
    TEST_ASSERT_EQUAL_UINT16(11, t.highest);
}

static void test_window_edge(void)
{
    mesh_seq_tracker_update(&t, 1000, NULL);
    mesh_seq_tracker_update(&t, 1000 + 63, NULL);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_DUPLICATE, mesh_seq_tracker_update(&t, 1000, NULL)); /* bit 63 */
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_LATE, mesh_seq_tracker_update(&t, 1001, NULL));
    mesh_seq_tracker_update(&t, 1000 + 64, NULL);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_RESET, mesh_seq_tracker_update(&t, 1000, NULL)); /* 64 behind */
}

static void test_reset_clears_state(void)
{
    mesh_seq_tracker_update(&t, 5, NULL);
    mesh_seq_tracker_reset(&t);
    TEST_ASSERT_FALSE(t.initialized);
    TEST_ASSERT_EQUAL_UINT32(0, t.received);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_NEW, mesh_seq_tracker_update(&t, 0, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_serial_arithmetic_wraps);
    RUN_TEST(test_in_order_stream_has_no_loss);
    RUN_TEST(test_gap_counts_loss);
    RUN_TEST(test_duplicate_is_detected_not_counted);
    RUN_TEST(test_late_frame_fills_gap);
    RUN_TEST(test_wraparound_is_seamless);
    RUN_TEST(test_full_wrap_many_times);
    RUN_TEST(test_large_forward_jump_clears_window);
    RUN_TEST(test_window_edge);
    RUN_TEST(test_reset_clears_state);
    return UNITY_END();
}
