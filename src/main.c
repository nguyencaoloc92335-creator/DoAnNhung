#include "stm32f1xx.h"
#include "tusb.h"

void SystemClock_Config(void) {
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0);
    FLASH->ACR |= FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1;
    RCC->CFGR |= RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9;
    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0);
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
}

void USB_LP_CAN1_RX0_IRQHandler(void) {
    tud_int_handler(0);
}

void LED_PC13_Init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1; // Output 2MHz
    GPIOC->ODR |= (1 << 13); // Tắt LED ban đầu
}

int main(void) {
    SystemClock_Config();
    LED_PC13_Init();

    RCC->APB1ENR |= RCC_APB1ENR_USBEN;
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 0);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);

    tusb_init();

    uint32_t heartbeat_counter = 0;

    while (1) {
        // Tác vụ nền của USB (luôn phải chạy liên tục)
        tud_task();

        // -----------------------------------------------------------
        // KHỐI 1: NHỊP TIM (Chỉ lo nháy LED, độc lập hoàn toàn)
        // -----------------------------------------------------------
        heartbeat_counter++;
        if (heartbeat_counter >= 3000000) { 
            GPIOC->ODR ^= (1 << 13); // Đảo trạng thái LED
            heartbeat_counter = 0;
        }

        // -----------------------------------------------------------
        // KHỐI 2: USB PARSER (Nằm NGOÀI khối nhịp tim, chạy liên tục)
        // -----------------------------------------------------------
        if (tud_cdc_available()) {
            char buf[64] = {0}; // Tạo mảng sạch để chứa dữ liệu
            uint32_t count = tud_cdc_read(buf, sizeof(buf) - 1);

            // BẮT BUỘC: Cắt đuôi \r hoặc \n do phím Enter tạo ra
            for(int i = 0; i < count; i++) {
                if(buf[i] == '\r' || buf[i] == '\n') {
                    buf[i] = '\0';
                    break;
                }
            }

            // Phân tích lệnh 1 và 0 (Logic ngược để test)
            if (strcmp(buf, "1") == 0) {
                GPIOB->BSRR = (1 << 12);      // Bật Relay/LED
                tud_cdc_write_str("ON\r\n");  // Dội chữ ON
            } 
            else if (strcmp(buf, "0") == 0) {
                GPIOB->BRR = (1 << 12);       // Tắt Relay/LED
                tud_cdc_write_str("OFF\r\n"); // Dội chữ OFF
            } 
            else if (strlen(buf) > 0) {
                tud_cdc_write_str("Sai lenh!\r\n");
            }
            
            tud_cdc_write_flush();
        }
    }
}