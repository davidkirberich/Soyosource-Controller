#include <unity.h>

#include "safety_logic.h"

static void test_total_demand_is_divided_per_inverter() {
    TEST_ASSERT_EQUAL_INT32(800, distributeTotalDemand(2400, 3));
    TEST_ASSERT_EQUAL_INT32(50, distributeTotalDemand(50, 1));
    TEST_ASSERT_EQUAL_INT32(0, distributeTotalDemand(2400, 0));
}

static void test_demand_is_clamped_to_per_inverter_limit() {
    TEST_ASSERT_EQUAL_INT32(2400, applyDemandLimits(3000, 2400, false, 500));
    TEST_ASSERT_EQUAL_INT32(0, applyDemandLimits(-10, 2400, false, 500));
    TEST_ASSERT_EQUAL_INT32(700, applyDemandLimits(700, 2400, false, 500));
}

static void test_night_limit_can_only_reduce_demand() {
    TEST_ASSERT_EQUAL_INT32(300, applyDemandLimits(800, 2400, true, 300));
    TEST_ASSERT_EQUAL_INT32(200, applyDemandLimits(200, 2400, true, 300));
    TEST_ASSERT_EQUAL_INT32(300, applyDemandLimits(3000, 2400, true, 300));
}

static void test_selected_shelly_phases_are_summed() {
    float total = -1;
    TEST_ASSERT_TRUE(sumSelectedShellyPhases(
        true, true, 10.5f, true, true, -3.0f, false, false, 99.0f, total));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.5f, total);
}

static void test_zero_is_a_valid_shelly_measurement() {
    float total = -1;
    TEST_ASSERT_TRUE(sumSelectedShellyPhases(
        true, true, 0.0f, true, true, 0.0f, true, true, 0.0f, total));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, total);
}

static void test_missing_selected_shelly_phase_is_rejected() {
    float total = 123.0f;
    TEST_ASSERT_FALSE(sumSelectedShellyPhases(
        true, true, 10.0f, true, false, 0.0f, false, false, 0.0f, total));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 123.0f, total);
}

static void test_no_selected_shelly_phase_is_rejected() {
    float total = 123.0f;
    TEST_ASSERT_FALSE(sumSelectedShellyPhases(
        false, false, 0.0f, false, false, 0.0f, false, false, 0.0f, total));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 123.0f, total);
}

static void test_zero_demand_frame_matches_protocol() {
    const uint8_t expected[] = {0x24, 0x56, 0x00, 0x21, 0x00, 0x00, 0x80, 0x08};
    uint8_t frame[8] = {};
    buildDemandFrame(0, frame);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

static void test_demand_frame_encodes_watts_and_checksum() {
    uint8_t frame[8] = {};
    buildDemandFrame(0x1234, frame);
    TEST_ASSERT_EQUAL_UINT8(0x12, frame[4]);
    TEST_ASSERT_EQUAL_UINT8(0x34, frame[5]);
    TEST_ASSERT_EQUAL_UINT8((264 - 0x12 - 0x34) & 0xFF, frame[7]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_total_demand_is_divided_per_inverter);
    RUN_TEST(test_demand_is_clamped_to_per_inverter_limit);
    RUN_TEST(test_night_limit_can_only_reduce_demand);
    RUN_TEST(test_selected_shelly_phases_are_summed);
    RUN_TEST(test_zero_is_a_valid_shelly_measurement);
    RUN_TEST(test_missing_selected_shelly_phase_is_rejected);
    RUN_TEST(test_no_selected_shelly_phase_is_rejected);
    RUN_TEST(test_zero_demand_frame_matches_protocol);
    RUN_TEST(test_demand_frame_encodes_watts_and_checksum);
    return UNITY_END();
}
