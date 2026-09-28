/*
 * SPDX-License-Identifier: MIT
 * Sender reliability state machine (ACK, timeout, backoff, Karn-safe RTT).
 */
#include "mesh_protocol.h"
#include "mesh_tx.h"
#include "unity.h"

static mesh_tx_t tx;
static mesh_retry_policy_t pol;

void setUp(void)
{
    pol = (mesh_retry_policy_t){
        .max_attempts = 3, .ack_timeout_us = 1000, .backoff_base_us = 500, .backoff_max_us = 1500, .jitter_us = 0};
    mesh_tx_init(&tx, &pol, 1);
}
void tearDown(void) {}

static void test_happy_path_reports_rtt(void)
{
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_SEND, mesh_tx_start(&tx, 42));
    mesh_tx_sent(&tx, 10000);
    TEST_ASSERT_TRUE(mesh_tx_busy(&tx));
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_WAIT, mesh_tx_poll(&tx, 10500));
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_DONE, mesh_tx_on_ack(&tx, 42, 1, MESH_ACK_OK, 10800));
    TEST_ASSERT_EQUAL_UINT32(800, tx.rtt_us);
    TEST_ASSERT_EQUAL_INT(MESH_TX_DONE, tx.state);
    TEST_ASSERT_FALSE(mesh_tx_busy(&tx));
}

static void test_foreign_or_stale_ack_is_ignored(void)
{
    mesh_tx_start(&tx, 42);
    mesh_tx_sent(&tx, 0);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_WAIT, mesh_tx_on_ack(&tx, 41, 1, MESH_ACK_OK, 100));
    TEST_ASSERT_EQUAL_INT(MESH_TX_WAIT_ACK, tx.state);
}

static void test_timeout_then_retry_with_exponential_backoff(void)
{
    mesh_tx_start(&tx, 7);
    mesh_tx_sent(&tx, 0);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_WAIT, mesh_tx_poll(&tx, 1000)); /* timeout -> backoff */
    TEST_ASSERT_EQUAL_INT(MESH_TX_BACKOFF, tx.state);
    TEST_ASSERT_EQUAL_UINT64(1000 + 500, mesh_tx_deadline(&tx)); /* base * 2^0 */
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_WAIT, mesh_tx_poll(&tx, 1499));
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_SEND, mesh_tx_poll(&tx, 1500));
    TEST_ASSERT_EQUAL_UINT8(2, tx.attempt);
    mesh_tx_sent(&tx, 1500);
    mesh_tx_poll(&tx, 2500);
    TEST_ASSERT_EQUAL_UINT64(2500 + 1000, mesh_tx_deadline(&tx)); /* base * 2^1 */
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_SEND, mesh_tx_poll(&tx, 3500));
    TEST_ASSERT_EQUAL_UINT8(3, tx.attempt);
    mesh_tx_sent(&tx, 3500);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_FAILED, mesh_tx_poll(&tx, 4500)); /* 3 attempts exhausted */
    TEST_ASSERT_EQUAL_INT(MESH_TX_FAILED, tx.state);
}

static void test_backoff_is_capped(void)
{
    uint32_t rng = 1;
    mesh_retry_policy_t p = {.max_attempts = 8, .ack_timeout_us = 1, .backoff_base_us = 500,
                             .backoff_max_us = 1500, .jitter_us = 0};
    TEST_ASSERT_EQUAL_UINT32(500, mesh_retry_backoff_us(&p, 2, &rng));
    TEST_ASSERT_EQUAL_UINT32(1000, mesh_retry_backoff_us(&p, 3, &rng));
    TEST_ASSERT_EQUAL_UINT32(1500, mesh_retry_backoff_us(&p, 4, &rng));
    TEST_ASSERT_EQUAL_UINT32(1500, mesh_retry_backoff_us(&p, 8, &rng));
    TEST_ASSERT_EQUAL_UINT32(1500, mesh_retry_backoff_us(&p, 200, &rng)); /* no shift overflow */
}

static void test_jitter_is_bounded_and_deterministic(void)
{
    mesh_retry_policy_t p = {.max_attempts = 4, .ack_timeout_us = 1, .backoff_base_us = 1000,
                             .backoff_max_us = 1000, .jitter_us = 300};
    uint32_t a = 99, b = 99;
    for (int i = 0; i < 1000; i++) {
        const uint32_t da = mesh_retry_backoff_us(&p, 2, &a);
        TEST_ASSERT_TRUE(da >= 1000 && da < 1300);
        TEST_ASSERT_EQUAL_UINT32(da, mesh_retry_backoff_us(&p, 2, &b)); /* same seed, same sequence */
    }
}

static void test_late_ack_during_backoff_uses_echoed_attempt_for_rtt(void)
{
    /* Karn's problem: an ACK for attempt 1 arriving after we started backing off
     * must be measured against attempt 1's send time, not a later one. */
    mesh_tx_start(&tx, 9);
    mesh_tx_sent(&tx, 0);
    mesh_tx_poll(&tx, 1000); /* timeout */
    TEST_ASSERT_EQUAL_INT(MESH_TX_BACKOFF, tx.state);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_DONE, mesh_tx_on_ack(&tx, 9, 1, MESH_ACK_OK, 1200));
    TEST_ASSERT_EQUAL_UINT32(1200, tx.rtt_us);

    mesh_tx_start(&tx, 10);
    mesh_tx_sent(&tx, 0);
    mesh_tx_poll(&tx, 1000);
    mesh_tx_poll(&tx, 1500); /* SEND attempt 2 */
    mesh_tx_sent(&tx, 1500);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_DONE, mesh_tx_on_ack(&tx, 10, 2, MESH_ACK_OK, 1900));
    TEST_ASSERT_EQUAL_UINT32(400, tx.rtt_us);
}

static void test_bogus_attempt_echo_falls_back_to_latest(void)
{
    mesh_tx_start(&tx, 1);
    mesh_tx_sent(&tx, 100);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_DONE, mesh_tx_on_ack(&tx, 1, 7, MESH_ACK_OK, 300));
    TEST_ASSERT_EQUAL_UINT32(200, tx.rtt_us);
}

static void test_radio_failure_skips_ack_wait(void)
{
    mesh_tx_start(&tx, 3);
    mesh_tx_sent(&tx, 0);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_WAIT, mesh_tx_on_radio_fail(&tx, 50));
    TEST_ASSERT_EQUAL_INT(MESH_TX_BACKOFF, tx.state);
    TEST_ASSERT_EQUAL_UINT64(550, mesh_tx_deadline(&tx));
    /* a second radio failure report while backing off is ignored */
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_WAIT, mesh_tx_on_radio_fail(&tx, 60));
    TEST_ASSERT_EQUAL_UINT64(550, mesh_tx_deadline(&tx));
}

static void test_busy_ack_triggers_retry(void)
{
    mesh_tx_start(&tx, 5);
    mesh_tx_sent(&tx, 0);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_WAIT, mesh_tx_on_ack(&tx, 5, 1, MESH_ACK_BUSY, 100));
    TEST_ASSERT_EQUAL_INT(MESH_TX_BACKOFF, tx.state);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_SEND, mesh_tx_poll(&tx, 600));
}

static void test_duplicate_and_rejected_acks_terminate(void)
{
    mesh_tx_start(&tx, 5);
    mesh_tx_sent(&tx, 0);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_DONE, mesh_tx_on_ack(&tx, 5, 1, MESH_ACK_DUPLICATE, 10));
    TEST_ASSERT_EQUAL_UINT8(MESH_ACK_DUPLICATE, tx.acked_status);
    mesh_tx_start(&tx, 6);
    mesh_tx_sent(&tx, 0);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_DONE, mesh_tx_on_ack(&tx, 6, 1, MESH_ACK_REJECTED, 10));
    TEST_ASSERT_EQUAL_UINT8(MESH_ACK_REJECTED, tx.acked_status);
}

static void test_policy_sanitize(void)
{
    mesh_retry_policy_t p = {.max_attempts = 0, .ack_timeout_us = 0, .backoff_base_us = 100, .backoff_max_us = 5};
    p = mesh_retry_policy_sanitize(p);
    TEST_ASSERT_EQUAL_UINT8(1, p.max_attempts);
    TEST_ASSERT_TRUE(p.ack_timeout_us > 0);
    TEST_ASSERT_EQUAL_UINT32(100, p.backoff_max_us);
    p.max_attempts = 200;
    TEST_ASSERT_EQUAL_UINT8(MESH_TX_MAX_ATTEMPTS_LIMIT, mesh_retry_policy_sanitize(p).max_attempts);
    const mesh_retry_policy_t d = mesh_retry_policy_default();
    TEST_ASSERT_EQUAL_UINT8(4, d.max_attempts);
}

static void test_single_attempt_policy_fails_immediately_on_timeout(void)
{
    pol.max_attempts = 1;
    mesh_tx_init(&tx, &pol, 1);
    mesh_tx_start(&tx, 1);
    mesh_tx_sent(&tx, 0);
    TEST_ASSERT_EQUAL_INT(MESH_TX_ACTION_FAILED, mesh_tx_poll(&tx, 1000));
    TEST_ASSERT_EQUAL_STRING("failed", mesh_tx_state_str(tx.state));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_happy_path_reports_rtt);
    RUN_TEST(test_foreign_or_stale_ack_is_ignored);
    RUN_TEST(test_timeout_then_retry_with_exponential_backoff);
    RUN_TEST(test_backoff_is_capped);
    RUN_TEST(test_jitter_is_bounded_and_deterministic);
    RUN_TEST(test_late_ack_during_backoff_uses_echoed_attempt_for_rtt);
    RUN_TEST(test_bogus_attempt_echo_falls_back_to_latest);
    RUN_TEST(test_radio_failure_skips_ack_wait);
    RUN_TEST(test_busy_ack_triggers_retry);
    RUN_TEST(test_duplicate_and_rejected_acks_terminate);
    RUN_TEST(test_policy_sanitize);
    RUN_TEST(test_single_attempt_policy_fails_immediately_on_timeout);
    return UNITY_END();
}
