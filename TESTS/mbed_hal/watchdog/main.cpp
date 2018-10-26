/* mbed Microcontroller Library
 * Copyright (c) 2017 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#if !DEVICE_WATCHDOG
#error [NOT_SUPPORTED] Watchdog not supported for this target
#endif

#include "greentea-client/test_env.h"
#include "utest/utest.h"
#include "unity/unity.h"
#include "hal/watchdog_api.h"
#include "watchdog_api_tests.h"

/* This is platform specific and depends on the watchdog timer implementation,
 * e.g. STM32F4 uses 32kHz internal RC oscillator to clock the IWDG, so
 * when the prescaler divider is set to max value of 256 the resolution
 * drops to 8 ms.
 */
#define WORST_TIMEOUT_RESOLUTION_MS 8UL

#define TIMEOUT_DELTA_MS (WORST_TIMEOUT_RESOLUTION_MS)
#define WDG_TIMEOUT_MS 500UL

#define MSG_VALUE_DUMMY "0"
#define MSG_VALUE_LEN 24
#define MSG_KEY_LEN 24

#define MSG_KEY_DEVICE_READY "ready"
#define MSG_KEY_START_CASE "start_case"
#define MSG_KEY_DEVICE_RESET "reset_on_case_teardown"

/* Flush serial buffer before deep sleep/reset
 *
 * Since deepsleep()/reset would shut down the UART peripheral, we wait for some time
 * to allow for hardware serial buffers to completely flush.
 *
 * Take NUMAKER_PFM_NUC472 as an example:
 * Its UART peripheral has 16-byte Tx FIFO. With baud rate set to 9600, flush
 * Tx FIFO would take: 16 * 8 * 1000 / 9600 = 13.3 (ms). So set wait time to
 * 20ms here for safe.
 *
 * This should be replaced with a better function that checks if the
 * hardware buffers are empty. However, such an API does not exist now,
 * so we'll use the wait_ms() function for now.
 */
#define SERIAL_FLUSH_TIME_MS    20

int CASE_INDEX_START;
int CASE_INDEX_CURRENT;

using utest::v1::Case;
using utest::v1::Specification;
using utest::v1::Harness;

const watchdog_config_t WDG_CONFIG_DEFAULT = { .timeout_ms = WDG_TIMEOUT_MS };

void test_max_timeout_is_valid()
{
    TEST_ASSERT(hal_watchdog_get_platform_features().max_timeout > 1UL);
}

void test_restart_is_possible()
{
    watchdog_features_t features = hal_watchdog_get_platform_features();
    if (!features.disable_watchdog) {
        TEST_IGNORE_MESSAGE("Disabling watchdog not supported for this platform");
        return;
    }
    TEST_ASSERT(features.update_config);
}

void test_stop()
{
    watchdog_features_t features = hal_watchdog_get_platform_features();
    if (!features.disable_watchdog) {
        TEST_ASSERT_EQUAL(WATCHDOG_STATUS_NOT_SUPPORTED, hal_watchdog_stop());
        TEST_IGNORE_MESSAGE("Disabling watchdog not supported for this platform");
        return;
    }

    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_stop());

    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_init(&WDG_CONFIG_DEFAULT));
    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_stop());
    // Make sure that a disabled watchdog does not reset the core.
    wait_ms(WDG_TIMEOUT_MS + TIMEOUT_DELTA_MS);

    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_stop());
}

void test_update_config()
{
    watchdog_features_t features = hal_watchdog_get_platform_features();
    if (!features.update_config) {
        TEST_IGNORE_MESSAGE("Updating watchdog config not supported for this platform");
        return;
    }

    watchdog_config_t config = WDG_CONFIG_DEFAULT;
    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_init(&config));
    TEST_ASSERT_UINT32_WITHIN(WORST_TIMEOUT_RESOLUTION_MS, config.timeout_ms, hal_watchdog_get_reload_value());

    config.timeout_ms = features.max_timeout - 2 * WORST_TIMEOUT_RESOLUTION_MS;
    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_init(&config));
    TEST_ASSERT_UINT32_WITHIN(WORST_TIMEOUT_RESOLUTION_MS, config.timeout_ms, hal_watchdog_get_reload_value());

    config.timeout_ms = features.max_timeout;
    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_init(&config));
    TEST_ASSERT_UINT32_WITHIN(WORST_TIMEOUT_RESOLUTION_MS, config.timeout_ms, hal_watchdog_get_reload_value());
}

utest::v1::status_t case_setup_sync_on_reset(const Case * const source, const size_t index_of_case)
{
    CASE_INDEX_CURRENT = index_of_case;
    return utest::v1::greentea_case_setup_handler(source, index_of_case);
}

utest::v1::status_t case_teardown_sync_on_reset(const Case * const source, const size_t passed, const size_t failed,
        const utest::v1::failure_t failure)
{
    utest::v1::status_t status = utest::v1::greentea_case_teardown_handler(source, passed, failed, failure);
    if (failed) {
        /* Return immediately and skip the device reset, if the test case failed.
         * Provided that the device won't be restarted by other means (i.e. watchdog timer),
         * this should allow the test suite to finish in a defined manner
         * and report failure to host.
         * In case of watchdog reset during test suite teardown, the loss of serial
         * connection is possible, so the host-test-runner may return 'TIMEOUT'
         * instead of 'FAIL'.
         */
        return status;
    }
    greentea_send_kv(MSG_KEY_DEVICE_RESET, CASE_INDEX_START + CASE_INDEX_CURRENT);
    utest_printf("The device will now restart.\n");
    wait_ms(SERIAL_FLUSH_TIME_MS); // Wait for the serial buffers to flush.
    NVIC_SystemReset();
    return status; // Reset is instant so this line won't be reached.
}

utest::v1::status_t case_teardown_wdg_stop_or_reset(const Case * const source, const size_t passed, const size_t failed,
        const utest::v1::failure_t failure)
{
    watchdog_features_t features = hal_watchdog_get_platform_features();
    if (features.disable_watchdog) {
        hal_watchdog_stop();
        return utest::v1::greentea_case_teardown_handler(source, passed, failed, failure);
    }

    return case_teardown_sync_on_reset(source, passed, failed, failure);
}

template<uint32_t timeout_ms>
void test_init()
{
    watchdog_config_t config = { timeout_ms };
    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_init(&config));
    TEST_ASSERT_UINT32_WITHIN(WORST_TIMEOUT_RESOLUTION_MS, timeout_ms, hal_watchdog_get_reload_value());
}

void test_init_max_timeout()
{
    watchdog_features_t features = hal_watchdog_get_platform_features();
    watchdog_config_t config = { .timeout_ms = features.max_timeout };
    TEST_ASSERT_EQUAL(WATCHDOG_STATUS_OK, hal_watchdog_init(&config));
    TEST_ASSERT_UINT32_WITHIN(WORST_TIMEOUT_RESOLUTION_MS, features.max_timeout, hal_watchdog_get_reload_value());
}

int testsuite_setup_sync_on_reset(const size_t number_of_cases)
{
    GREENTEA_SETUP(45, "sync_on_reset");
    utest::v1::status_t status = utest::v1::greentea_test_setup_handler(number_of_cases);
    if (status != utest::v1::STATUS_CONTINUE) {
        return status;
    }

    char key[MSG_KEY_LEN + 1] = { };
    char value[MSG_VALUE_LEN + 1] = { };

    greentea_send_kv(MSG_KEY_DEVICE_READY, MSG_VALUE_DUMMY);
    greentea_parse_kv(key, value, MSG_KEY_LEN, MSG_VALUE_LEN);

    if (strcmp(key, MSG_KEY_START_CASE) != 0) {
        utest_printf("Invalid message key.\n");
        return utest::v1::STATUS_ABORT;
    }

    char *tailptr = NULL;
    CASE_INDEX_START = (int) strtol(value, &tailptr, 10);
    if (*tailptr != '\0' || CASE_INDEX_START < 0) {
        utest_printf("Invalid start case index received from host\n");
        return utest::v1::STATUS_ABORT;
    }

    utest_printf("Starting with test case index %i of all %i defined test cases.\n", CASE_INDEX_START, number_of_cases);
    return CASE_INDEX_START;
}

Case cases[] = {
    Case("Platform feature max_timeout is valid", test_max_timeout_is_valid),
    Case("Stopped watchdog can be started again", test_restart_is_possible),
    Case("Watchdog can be stopped", test_stop),

    Case("Update config with multiple init calls",
        (utest::v1::case_setup_handler_t) case_setup_sync_on_reset,
        test_update_config,
        (utest::v1::case_teardown_handler_t) case_teardown_wdg_stop_or_reset),

    // Do not set watchdog timeout shorter than 500 ms as it may cause the
    // host-test-runner return 'TIMEOUT' instead of 'FAIL' / 'PASS' if watchdog
    // performs reset during test suite teardown.
    Case("Init, 500 ms", (utest::v1::case_setup_handler_t) case_setup_sync_on_reset,
        test_init<500UL>, (utest::v1::case_teardown_handler_t) case_teardown_sync_on_reset),
    Case("Init, max_timeout", (utest::v1::case_setup_handler_t) case_setup_sync_on_reset,
        test_init_max_timeout, (utest::v1::case_teardown_handler_t) case_teardown_sync_on_reset),
};

Specification specification((utest::v1::test_setup_handler_t) testsuite_setup_sync_on_reset, cases);

int main()
{
    // Harness will start with a test case index provided by host script.
    return !Harness::run(specification);
}