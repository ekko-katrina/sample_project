#include "myuart.h"

#include "driver/uart.h"
#include "driver/gpio.h"

void myuart_init(void)
{
    uart_config_t uart_structure = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_0, &uart_structure);

    uart_set_pin(UART_NUM_0, GPIO_NUM_43, GPIO_NUM_44, -1, -1);

    uart_driver_install(UART_NUM_0, 1024, 1024, 0, NULL, 0);
}