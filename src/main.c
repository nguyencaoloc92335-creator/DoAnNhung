#include "stm32f1xx.h"
#include "tusb.h"
#include "i2c.h"
#include "lcd_i2c.h"
#include <string.h>

// --- Khởi tạo hàm delay System Tick chuẩn để phục vụ LCD Init ---
volatile uint32_t msTicks = 0;

void SysTick_Handler(void) {
    msTicks++;
}

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

    // Kích hoạt ngắt SysTick mỗi 1ms (Tần số System = 72MHz)
    SysTick_Config(SystemCoreClock / 1000);
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

// Khai báo biến toàn cục I2C và LCD
I2C_Handle_t hi2c1;
I2C_LCD_HandleTypeDef lcd1;

int main(void) {
    SystemClock_Config();
    LED_PC13_Init();

    // Khởi tạo thông số phần cứng cho I2C1
    hi2c1.Instance = I2C1;
    hi2c1.ClockSpeed = 100000;
    I2C_Init(&hi2c1);

    // Trỏ LCD sang interface I2C1 và khởi tạo LCD
    lcd1.hi2c = &hi2c1;
    lcd1.address = 0x3F; // Hãy sửa lại thành 0x3F nếu LCD PCF8574T của bạn mang phiên bản cũ 
    lcd1.col = 16;
    lcd1.row = 2;
    lcd_init(&lcd1);

    // In thông báo khi mới cấp nguồn hệ thống
    lcd_setcursor(&lcd1, 0, 0);
    lcd_print_string(&lcd1, "USB CDC Ready!");

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
        if (strlen(buf) > 0) {
            // Xóa nhanh 16 ký tự ở dòng số 2 để không bị kẹt rác từ lệnh dài trước đó
            lcd_setcursor(&lcd1, 0, 1);
            lcd_print_string(&lcd1, "                ");
                
            // In ngay nội dung lấy từ USB xuống màn LCD
            lcd_setcursor(&lcd1, 0, 1);
            lcd_print_string(&lcd1, buf);

            // Giữ lại các lệnh test logic
            if (strcmp(buf, "1") == 0) {
                tud_cdc_write_str("ON\r\n");  
            } 
            else if (strcmp(buf, "0") == 0) {
                tud_cdc_write_str("OFF\r\n"); 
            } 
            else {
                // Dội lại thông báo xác nhận thành công về Terminal của PC
                tud_cdc_write_str("Hien thi: ");
                tud_cdc_write_str(buf);
                tud_cdc_write_str("\r\n");
            }
                
            tud_cdc_write_flush();
        }
    }
}
