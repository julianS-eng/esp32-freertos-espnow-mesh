/*
 * SPDX-License-Identifier: MIT
 * Gateway lifecycle state machine.
 */
#include "gw_fsm.h"
#include "unity.h"

static gw_fsm_t fsm;

void setUp(void)
{
    gw_fsm_init(&fsm);
}
void tearDown(void) {}

static void test_nominal_boot_sequence(void)
{
    TEST_ASSERT_EQUAL_INT(GW_ST_BOOT, fsm.state);
    TEST_ASSERT_TRUE(gw_fsm_dispatch(&fsm, GW_EV_BOOT_DONE, NULL));
    TEST_ASSERT_TRUE(gw_fsm_dispatch(&fsm, GW_EV_REGISTRY_LOADED, NULL));
    TEST_ASSERT_EQUAL_INT(GW_ST_RADIO_INIT, fsm.state);
    TEST_ASSERT_FALSE(gw_fsm_accepts_traffic(fsm.state));
    TEST_ASSERT_TRUE(gw_fsm_dispatch(&fsm, GW_EV_RADIO_READY, NULL));
    TEST_ASSERT_EQUAL_INT(GW_ST_RUNNING, fsm.state);
    TEST_ASSERT_TRUE(gw_fsm_accepts_traffic(fsm.state));
    TEST_ASSERT_EQUAL_UINT(3, fsm.transitions);
}

static void test_corrupt_registry_still_boots(void)
{
    gw_fsm_dispatch(&fsm, GW_EV_BOOT_DONE, NULL);
    TEST_ASSERT_TRUE(gw_fsm_dispatch(&fsm, GW_EV_REGISTRY_CORRUPT, NULL));
    TEST_ASSERT_EQUAL_INT(GW_ST_RADIO_INIT, fsm.state);
}

static void test_radio_fault_and_retry(void)
{
    gw_fsm_dispatch(&fsm, GW_EV_BOOT_DONE, NULL);
    gw_fsm_dispatch(&fsm, GW_EV_REGISTRY_LOADED, NULL);
    gw_state_t from;
    TEST_ASSERT_TRUE(gw_fsm_dispatch(&fsm, GW_EV_RADIO_FAILED, &from));
    TEST_ASSERT_EQUAL_INT(GW_ST_RADIO_INIT, from);
    TEST_ASSERT_EQUAL_INT(GW_ST_FAULT, fsm.state);
    TEST_ASSERT_FALSE(gw_fsm_accepts_traffic(fsm.state));
    TEST_ASSERT_TRUE(gw_fsm_dispatch(&fsm, GW_EV_RETRY_TIMER, NULL));
    TEST_ASSERT_EQUAL_INT(GW_ST_RADIO_INIT, fsm.state);
}

static void test_degraded_hysteresis(void)
{
    fsm.state = GW_ST_RUNNING;
    TEST_ASSERT_FALSE(gw_fsm_dispatch(&fsm, GW_EV_QUEUE_RELIEVED, NULL)); /* already running */
    TEST_ASSERT_TRUE(gw_fsm_dispatch(&fsm, GW_EV_QUEUE_PRESSURE, NULL));
    TEST_ASSERT_EQUAL_INT(GW_ST_DEGRADED, fsm.state);
    TEST_ASSERT_TRUE(gw_fsm_accepts_traffic(fsm.state));
    TEST_ASSERT_FALSE(gw_fsm_dispatch(&fsm, GW_EV_QUEUE_PRESSURE, NULL));
    TEST_ASSERT_TRUE(gw_fsm_dispatch(&fsm, GW_EV_QUEUE_RELIEVED, NULL));
    TEST_ASSERT_EQUAL_INT(GW_ST_RUNNING, fsm.state);
}

static void test_every_invalid_pair_is_rejected_without_side_effects(void)
{
    unsigned valid = 0;
    for (int s = 0; s < GW_ST_COUNT_; s++) {
        for (int e = 0; e < GW_EV_COUNT_; e++) {
            gw_state_t next;
            if (gw_fsm_next((gw_state_t)s, (gw_event_t)e, &next)) {
                valid++;
                TEST_ASSERT_NOT_EQUAL(s, (int)next); /* no self-loops in the table */
            } else {
                TEST_ASSERT_EQUAL_INT(s, (int)next);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(8, valid); /* matches the diagram in gw_fsm.h */
    gw_state_t next;
    TEST_ASSERT_FALSE(gw_fsm_next(GW_ST_COUNT_, GW_EV_BOOT_DONE, &next));
    TEST_ASSERT_FALSE(gw_fsm_next(GW_ST_BOOT, GW_EV_COUNT_, &next));
    TEST_ASSERT_FALSE(gw_fsm_dispatch(&fsm, GW_EV_RADIO_READY, NULL));
    TEST_ASSERT_EQUAL_UINT(1, fsm.rejected);
    TEST_ASSERT_EQUAL_INT(GW_ST_BOOT, fsm.state);
}

static void test_names(void)
{
    TEST_ASSERT_EQUAL_STRING("degraded", gw_state_str(GW_ST_DEGRADED));
    TEST_ASSERT_EQUAL_STRING("?", gw_state_str(GW_ST_COUNT_));
    TEST_ASSERT_EQUAL_STRING("queue_pressure", gw_event_str(GW_EV_QUEUE_PRESSURE));
    TEST_ASSERT_EQUAL_STRING("?", gw_event_str(GW_EV_COUNT_));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nominal_boot_sequence);
    RUN_TEST(test_corrupt_registry_still_boots);
    RUN_TEST(test_radio_fault_and_retry);
    RUN_TEST(test_degraded_hysteresis);
    RUN_TEST(test_every_invalid_pair_is_rejected_without_side_effects);
    RUN_TEST(test_names);
    return UNITY_END();
}
