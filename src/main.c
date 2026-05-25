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

void delay_ms(uint32_t ms) {
    uint32_t start = msTicks;
    while ((msTicks - start) < ms);
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

void Force_USB_ReEnumerate(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN; // Bật clock Port A
    
    // Cấu hình PA12 (USB DP) là Output Push-Pull
    GPIOA->CRH &= ~(GPIO_CRH_MODE12 | GPIO_CRH_CNF12);
    GPIOA->CRH |= GPIO_CRH_MODE12_1; // Output 2MHz
    
    GPIOA->ODR &= ~(1 << 12); // Kéo PA12 xuống LOW để ngắt kết nối ảo với PC
    
    delay_ms(15); // Chờ 15ms để PC kịp nhận diện thiết bị đã "rút ra"
    
    // Khôi phục PA12 về trạng thái Floating Input (Để module USB ngoại vi tiếp quản lại)
    GPIOA->CRH &= ~(GPIO_CRH_MODE12 | GPIO_CRH_CNF12);
    GPIOA->CRH |= GPIO_CRH_CNF12_0; 
    
    delay_ms(15);
}

int main(void) {
    SystemClock_Config();
    LED_PC13_Init();
    Force_USB_ReEnumerate();

    // Khởi tạo thông số phần cứng cho I2C1
    hi2c1.Instance = I2C1;
    hi2c1.ClockSpeed = 100000;
    I2C_Init(&hi2c1);

    // Trỏ LCD sang interface I2C1 và khởi tạo LCD
    lcd1.hi2c = &hi2c1;
    lcd1.address = 0x27; // Hãy sửa lại thành 0x3F nếu LCD PCF8574T của bạn mang phiên bản cũ 
    lcd1.col = 16;
    lcd1.row = 2;
    lcd_init(&lcd1);
    delay_ms(1000);

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
        lcd_task(&lcd1);

        // -----------------------------------------------------------
        // KHỐI 1: NHỊP TIM
        // -----------------------------------------------------------
        heartbeat_counter++;
        if (heartbeat_counter >= 3000000) { 
            GPIOC->ODR ^= (1 << 13);
            heartbeat_counter = 0;
        }

        // -----------------------------------------------------------
        // KHỐI 2: XỬ LÝ NHẬN/GỬI USB CDC CHUẨN TINYUSB
        // -----------------------------------------------------------
        // Chỉ thực thi khi thực sự có dữ liệu mới từ máy tính gửi xuống
        if (tud_cdc_available()) {
            char buf[64] = {0}; 
            uint32_t count = tud_cdc_read(buf, sizeof(buf) - 1);
            
            if (count > 0) {
                // Đợi LCD rảnh (nếu trước đó nó đang in dở)
                while (lcd1.state != LCD_SM_IDLE) {
                    lcd_task(&lcd1); // Bơm task để nó mau hoàn thành
                }

                // Đặt con trỏ về đầu dòng 2 (Lệnh này gửi nhanh nên dùng blocking cũ cũng được)
                lcd_setcursor(&lcd1, 0, 1);
                
                // Format chuỗi hiển thị thành 16 ký tự (căn trái), tự động điền khoảng trắng ở đuôi
                char display_buf[17];
                snprintf(display_buf, sizeof(display_buf), "%-16s", buf);
                
                // Gửi xuống State Machine xử lý Non-blocking
                lcd_print_string(&lcd1, display_buf);

                // Loại bỏ ký tự \r hoặc \n nếu phần mềm Terminal / Python vô tình gửi kèm
                // Để lệnh strcmp hoạt động chuẩn xác
                if (strncmp(buf, "1", 1) == 0) {
                    tud_cdc_write_str("ON\r\n");  
                } 
                else if (strncmp(buf, "0", 1) == 0) {
                    tud_cdc_write_str("OFF\r\n"); 
                } 
                else {
                    tud_cdc_write_str("Hien thi: ");
                    tud_cdc_write_str(buf);
                    tud_cdc_write_str("\r\n");
                }
                    
                tud_cdc_write_flush(); // Đẩy dữ liệu lên PC
            }
        }
    }
}
