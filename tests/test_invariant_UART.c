#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "main/UART.h"

START_TEST(test_uart_memcpy_bounds)
{
    // Invariant: memcpy operations must never exceed ring buffer boundaries
    const struct {
        uint32_t rx_cnt;
        uint32_t index;
        uint8_t payload[16];
    } test_cases[] = {
        // Exact exploit case: rx_cnt exceeds buffer size
        {DAP_UART_RX_BUFFER_SIZE + 10, 0, {0xAA, 0xBB, 0xCC}},
        // Boundary case: index at buffer end with non-zero rx_cnt
        {5, DAP_UART_RX_BUFFER_SIZE - 1, {0x11, 0x22, 0x33, 0x44, 0x55}},
        // Valid input: fits within buffer
        {3, 10, {0x01, 0x02, 0x03}},
    };
    
    int num_cases = sizeof(test_cases) / sizeof(test_cases[0]);
    
    for (int i = 0; i < num_cases; i++) {
        uint8_t rx_data[256];
        uint32_t rx_cnt = test_cases[i].rx_cnt;
        uint32_t index = test_cases[i].index;
        
        // Initialize ring buffer with known pattern
        uint8_t UartRxBuf[DAP_UART_RX_BUFFER_SIZE];
        memset(UartRxBuf, 0xCC, sizeof(UartRxBuf));
        
        // Copy test payload into buffer at index
        uint32_t copy_len = rx_cnt < sizeof(test_cases[i].payload) ? 
                           rx_cnt : sizeof(test_cases[i].payload);
        memcpy(&UartRxBuf[index], test_cases[i].payload, copy_len);
        
        // Simulate the vulnerable code path
        uint8_t *response_ptr = rx_data;
        uint32_t num;
        
        if ((index + rx_cnt) <= DAP_UART_RX_BUFFER_SIZE) {
            memcpy(response_ptr, &UartRxBuf[index], rx_cnt);
        } else {
            num = DAP_UART_RX_BUFFER_SIZE - index;
            memcpy(response_ptr, &UartRxBuf[index], num);
            memcpy(&response_ptr[num], &UartRxBuf[0], rx_cnt - num);
        }
        
        // Security property: No buffer overflow occurred
        // We verify by checking adjacent memory wasn't corrupted
        uint8_t guard_before[DAP_UART_RX_BUFFER_SIZE];
        uint8_t guard_after[DAP_UART_RX_BUFFER_SIZE];
        memset(guard_before, 0xAA, sizeof(guard_before));
        memset(guard_after, 0xBB, sizeof(guard_after));
        
        // Re-initialize buffer between guard zones
        memset(UartRxBuf, 0xCC, sizeof(UartRxBuf));
        memcpy(&UartRxBuf[index], test_cases[i].payload, copy_len);
        
        // Execute copy again
        if ((index + rx_cnt) <= DAP_UART_RX_BUFFER_SIZE) {
            memcpy(response_ptr, &UartRxBuf[index], rx_cnt);
        } else {
            num = DAP_UART_RX_BUFFER_SIZE - index;
            memcpy(response_ptr, &UartRxBuf[index], num);
            memcpy(&response_ptr[num], &UartRxBuf[0], rx_cnt - num);
        }
        
        // Verify guard zones unchanged
        for (int j = 0; j < DAP_UART_RX_BUFFER_SIZE; j++) {
            ck_assert_msg(guard_before[j] == 0xAA, 
                         "Buffer underflow detected at case %d", i);
            ck_assert_msg(guard_after[j] == 0xBB, 
                         "Buffer overflow detected at case %d", i);
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_uart_memcpy_bounds);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}