#include "stm32f103xb.h"  
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "i2c.h"
#include "lcd_i2c.h"

// Hàng đợi lưu nhãn vật thể (Giả sử băng tải chứa tối đa 10 vật cùng lúc)
#define QUEUE_LENGTH 10
#define ITEM_SIZE    sizeof(uint8_t)

// --- BIẾN TOÀN CỤC CHO I2C & LCD ---
I2C_Handle_t hi2c1;
I2C_LCD_HandleTypeDef hlcd;

QueueHandle_t xQueue_A1; // Nhớ nhãn các vật đang trôi từ A0 -> A1
QueueHandle_t xQueue_A2; // Nhớ nhãn các vật đang trôi từ A1 -> A2

// Timer tự động tắt Piston
TimerHandle_t xTimer_P1;
TimerHandle_t xTimer_P2;

// Timer chống nhiễu cho cảm biến (Debounce Timers)
TimerHandle_t xTimer_Debounce0;
TimerHandle_t xTimer_Debounce1;
TimerHandle_t xTimer_Debounce2;

// Hàng đợi đẩy Log từ Ngắt (ISR) sang Task USB
QueueHandle_t xQueue_Log;

// Biến đếm thống kê (Hiển thị lên LCD)
volatile uint32_t count_type0 = 0;
volatile uint32_t count_type1 = 0;
volatile uint32_t count_type2 = 0;

// --- BIẾN TOÀN CỤC CHO UI & NGƯỠNG ĐỘC LẬP ---
volatile uint8_t current_page = 0;      // 0: Loại 0, 1: Loại 1, 2: Loại 2 (Đứng yên, chỉ chuyển khi bấm TAB)
volatile uint8_t in_setting_mode = 0;   // 0: Chế độ xem thống kê, 1: Chế độ cài đặt ngưỡng
volatile uint32_t limit_type0 = 5;     // Ngưỡng dừng riêng cho Loại 0
volatile uint32_t limit_type1 = 5;     // Ngưỡng dừng riêng cho Loại 1
volatile uint32_t limit_type2 = 5;     // Ngưỡng dừng riêng cho Loại 2
volatile uint8_t system_halted = 0;    // 0: Băng tải đang chạy, 1: Đã dừng do đủ hàng
volatile uint8_t pc_disconnected = 0; // 0: PC đang kết nối, 1: Mất PC

typedef struct {
    uint32_t id;
    uint8_t type;
} ConveyorItem_t;

volatile uint32_t global_item_id = 0;

// Thêm Timer Watchdog
TimerHandle_t xTimer_PC_Watchdog;

// Hàm kiểm tra ngưỡng riêng biệt - Chỉ cần 1 loại đạt ngưỡng là dừng toàn hệ thống
void Check_Limit(void) {
    if (system_halted == 0) {
        if (count_type0 >= limit_type0 || count_type1 >= limit_type1 || count_type2 >= limit_type2) {
            GPIOB->BRR = (1 << 12); // Tắt chân PB12 để dừng băng tải ngay lập tức
            system_halted = 1;
        }
    }
}

// --- CÁC HÀM GIAO TIẾP UART ---

// 1. Hàm gửi 1 ký tự
void UART1_SendChar(char c) {
    // Chờ cờ TXE (Transmit Data Register Empty - Bit 7) bật lên báo hiệu thanh ghi trống
    while (!(USART1->SR & USART_SR_TXE)); 
    USART1->DR = c; // Đẩy ký tự vào thanh ghi truyền
}

// 2. Hàm gửi 1 chuỗi ký tự
void UART1_SendString(const char *str) {
    while (*str != '\0') {
        UART1_SendChar(*str);
        str++;
    }
}

// 3. Hàm in số nguyên dương
void UART1_SendNumber(uint32_t num) {
    char buf[11]; // Đủ chứa số tối đa 4 tỷ (10 chữ số) + null
    int i = 0;
    
    if (num == 0) {
        UART1_SendChar('0');
        return;
    }
    
    // Tách từng chữ số (bị ngược)
    while (num > 0) {
        buf[i++] = (num % 10) + '0';
        num /= 10;
    }
    
    // In ngược mảng lại để ra số đúng
    for (int j = i - 1; j >= 0; j--) {
        UART1_SendChar(buf[j]);
    }
}

uint32_t my_strlen(const char *str) {
    uint32_t len = 0;
    while (str[len] != '\0') len++;
    return len;
}

void my_strcpy(char *dest, const char *src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

void my_strcat(char *dest, const char *src) {
    while (*dest) dest++; // Duyệt đến cuối chuỗi đích
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

void* my_memcpy(void *dest, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void uint32_to_string(uint32_t num, char* str) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    char temp[11]; // uint32_t tối đa là 4294967295 (10 chữ số)
    int i = 0;
    while (num > 0) {
        temp[i++] = (num % 10) + '0';
        num /= 10;
    }
    // Đảo ngược chuỗi (vì phép chia lấy dư tạo ra chữ số cuối cùng trước)
    int j = 0;
    while (i > 0) {
        str[j++] = temp[--i];
    }
    str[j] = '\0';
}

void delay_ms(uint32_t ms) {
    // Nếu RTOS đã chạy, dùng hàm non-blocking của hệ điều hành
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        // Nếu RTOS chưa chạy (vẫn ở Init), dùng vòng lặp cứng (Dựa trên SystemCoreClock 72MHz)
        for (volatile uint32_t i = 0; i < ms * 7200; i++);
    }
}

void Hardware_Init(void) {
    // --- 1. BẬT XUNG NHỊP CHO GPIOA VÀ USART1 ---
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;

    // --- 2. CẤU HÌNH CHÂN PA9 (TX) VÀ PA10 (RX) ---
    // Xóa cấu hình cũ của PA9 (bit 4-7) và PA10 (bit 8-11) trong thanh ghi CRH
    GPIOA->CRH &= ~(GPIO_CRH_CNF9 | GPIO_CRH_MODE9 | GPIO_CRH_CNF10 | GPIO_CRH_MODE10);
    
    // Thiết lập PA9 (TX): Alternate function Push-pull, tốc độ max 50MHz (0xB)
    // Thiết lập PA10 (RX): Floating input (0x4)
    GPIOA->CRH |= (0xB << 4) | (0x4 << 8);

    // --- 3. CẤU HÌNH TỐC ĐỘ BAUD 115200 ---
    // Giả sử vi điều khiển của bạn đang chạy ở xung nhịp tối đa 72MHz
    USART1->BRR = 72000000 / 115200; 

    // --- 4. BẬT USART1 ---
    // Kích hoạt USART (UE), Kích hoạt bộ phát (TE) và bộ thu (RE)
    USART1->CR1 |= USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

    // 1. Bật Clock 
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;

    NVIC_SetPriorityGrouping(3);

    // --- CẤU HÌNH CẢM BIẾN (PA0-PA2) & NÚT BẤM (PA3-PA6) ---
    // Clear cấu hình cũ của chân PA0 đến PA7 (bits 0-31 của CRL)
    GPIOA->CRL &= ~(0xFFFFFFFF);
    // Set Input Pull-up/Pull-down (mode = 00, cnf = 10 -> 0x8 cho mỗi chân)
    GPIOA->CRL |= 0x88888888;
    // Bật Pull-up cho PA0, PA1, PA2, PA3, PA4, PA5, PA6 (kéo lên 3.3V)
    GPIOA->ODR |= (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6);

    // --- CẤU HÌNH RELAY (PB12, PB13, PB14) ---
    // Clear cấu hình cũ của PB12, PB13, PB14 (bits 16-27 của CRH)
    GPIOB->CRH &= ~(0x0FFF0000);
    // Set Output Push-pull, max speed 2MHz (mode = 10, cnf = 00 -> 0x2)
    GPIOB->CRH |= 0x02220000;
    
    // Khởi động luôn băng tải PB12
    GPIOB->BSRR = (1 << 12); 
    // Đảm bảo Piston 1, 2 đang tắt
    GPIOB->BRR = (1 << 13) | (1 << 14);

    // --- CẤU HÌNH NGẮT NGOÀI (EXTI0, EXTI1, EXTI2) ---
    // Map chân PA0, PA1, PA2 vào line ngắt tương ứng (AFIO_EXTICR1)
    AFIO->EXTICR[0] &= ~(0x00000FFF); // Clear để chọn Port A (0000)

    // Unmask ngắt (Cho phép ngắt line 0, 1, 2)
    EXTI->IMR |= (EXTI_IMR_MR0 | EXTI_IMR_MR1 | EXTI_IMR_MR2);
    // Chọn kích hoạt sườn xuống (Vật thể đi qua làm tín hiệu kéo xuống 0V)
    EXTI->FTSR |= (EXTI_FTSR_TR0 | EXTI_FTSR_TR1 | EXTI_FTSR_TR2);

    // --- BẬT NGẮT NHẬN DỮ LIỆU TỪ PC ---
    USART1->CR1 |= USART_CR1_RXNEIE;  // Bật cờ ngắt khi có byte RX bay tới

    // --- CẤU HÌNH NVIC CHO FREERTOS ---
    // BẮT BUỘC: Mức ưu tiên ngắt phải thấp hơn (số lớn hơn) configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (đang là 5)
    NVIC_SetPriority(EXTI0_IRQn, 6);
    NVIC_SetPriority(EXTI1_IRQn, 6);
    NVIC_SetPriority(EXTI2_IRQn, 6);

    NVIC_SetPriority(I2C1_EV_IRQn, 6);

    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
    
    NVIC_SetPriority(USART1_IRQn, 6); 
    NVIC_EnableIRQ(USART1_IRQn);      // Cho phép ngắt USART1 hoạt động
}

// Ngắt Cảm biến A0
void EXTI0_IRQHandler(void) {
    if (EXTI->PR & EXTI_PR_PR0) {
        EXTI->PR = EXTI_PR_PR0; 
        EXTI->IMR &= ~EXTI_IMR_MR0; // Tắt ngắt line 0 tạm thời để chặn nhiễu dội
        
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xTimerStartFromISR(xTimer_Debounce0, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken); // Thoát ngắt ngay lập tức
    }
}

// Ngắt Cảm biến A1 (Piston 1)
void EXTI1_IRQHandler(void) {
    if (EXTI->PR & EXTI_PR_PR1) {
        EXTI->PR = EXTI_PR_PR1; 
        EXTI->IMR &= ~EXTI_IMR_MR1; // Tắt ngắt line 1 tạm thời
        
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xTimerStartFromISR(xTimer_Debounce1, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// Ngắt Cảm biến A2 (Piston 2)
void EXTI2_IRQHandler(void) {
    if (EXTI->PR & EXTI_PR_PR2) {
        EXTI->PR = EXTI_PR_PR2; 
        EXTI->IMR &= ~EXTI_IMR_MR2; // Tắt ngắt line 2 tạm thời
        
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xTimerStartFromISR(xTimer_Debounce2, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void vTimerCallback_Piston(TimerHandle_t xTimer) {
    // Lấy ID của Timer để biết Piston nào vừa hết giờ
    uint32_t timer_id = (uint32_t) pvTimerGetTimerID(xTimer);
    
    if (timer_id == 1) {
        GPIOB->BRR = (1 << 13); // Reset PB13 (Thu Piston 1 về)
    } else if (timer_id == 2) {
        GPIOB->BRR = (1 << 14); // Reset PB14 (Thu Piston 2 về)
    }
}

// Hàm setup Clock lên 72MHz (Nếu bạn đã có hàm riêng thì dùng hàm của bạn)
void SystemClock_Config(void) {
    // 1. Bật HSE (Thạch anh ngoài)
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY));

    // 2. Cấu hình Flash Prefetch và Latency
    FLASH->ACR |= FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

    // 3. Cấu hình PLL: HSE (8MHz) * 9 = 72MHz
    RCC->CFGR |= RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

    // 4. Cấu hình Buses (AHB = /1, APB1 = /2, APB2 = /1)
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1;

    // 5. Chuyển System Clock sang PLL
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
}

void Task_UART_Logging(void *pvParameters) {
    ConveyorItem_t log_item; // Hứng cả ID và Type
    
    UART1_SendString("\r\n=== HE THONG BANG TAI DA KHOI DONG ===\r\n");
    
    for(;;) {
        if (xQueueReceive(xQueue_Log, &log_item, portMAX_DELAY) == pdPASS) {
            
            // 1. BÁO CÁO DONE VỀ CHO PYTHON
            UART1_SendString("DONE:");
            UART1_SendNumber(log_item.id);
            UART1_SendString(",");
            UART1_SendNumber(log_item.type);
            UART1_SendString("\n");

            // 2. IN LOG HIỂN THỊ LÊN MÀN HÌNH TERMINAL (Tuỳ chọn xem cho dễ)
            UART1_SendString("=> Da xu ly xong vat the ID: ");
            UART1_SendNumber(log_item.id);
            UART1_SendString(" (Loai ");
            UART1_SendNumber(log_item.type);
            UART1_SendString(")\r\n");
            
            UART1_SendString("   Tong so luong: L0=");
            UART1_SendNumber(count_type0);
            UART1_SendString(", L1=");
            UART1_SendNumber(count_type1);
            UART1_SendString(", L2=");
            UART1_SendNumber(count_type2);
            UART1_SendString("\r\n-----------------------\r\n");
        }
    }
}

void Task_LCD_Display(void *pvParameters) {
    // 1. Khởi tạo I2C và cấu hình màn hình LCD 1602
    hi2c1.Instance = I2C1;
    hi2c1.ClockSpeed = 100000;
    I2C_Init(&hi2c1);

    hlcd.hi2c = &hi2c1;
    hlcd.address = 0x27; 
    hlcd.col = 16;       
    hlcd.row = 2;        
    lcd_init(&hlcd);

    TickType_t last_update = 0;
    uint8_t render_step = 0;
    char buffer[17]; // Buffer chứa đúng 16 ký tự + null terminator

    uint8_t last_tab = 1, last_set = 1, last_up = 1, last_down = 1;
    uint8_t force_render = 1;

    for(;;) {
        // --- 2. ĐỌC TRẠNG THÁI NÚT BẤM ---
        uint8_t tab  = (GPIOA->IDR & (1 << 3)) ? 1 : 0;
        uint8_t set  = (GPIOA->IDR & (1 << 4)) ? 1 : 0;
        uint8_t up   = (GPIOA->IDR & (1 << 5)) ? 1 : 0;
        uint8_t down = (GPIOA->IDR & (1 << 6)) ? 1 : 0;
        
        uint8_t btn_pressed = 0; // Cờ báo hiệu có nút được nhấn

        // Nút TAB: Chuyển trang (Loại 0 -> Loại 1 -> Loại 2)
        if (tab == 0 && last_tab == 1) {
            if (current_page < 2) current_page++;
            else current_page = 0;
            force_render = 1;
            btn_pressed = 1;
        }

        // Nút SETTING: Đảo qua lại giữa trang Thống kê và trang Cài đặt
        if (set == 0 && last_set == 1) {
            in_setting_mode = !in_setting_mode;
            force_render = 1;
            btn_pressed = 1;
        }

        // Nút UP/DOWN: Chỉ nhận khi ở trang Cài đặt
        if (in_setting_mode == 1) {
            if (up == 0 && last_up == 1) {
                if (current_page == 0) limit_type0++;
                else if (current_page == 1) limit_type1++;
                else if (current_page == 2) limit_type2++;

                if (system_halted && count_type0 < limit_type0 && count_type1 < limit_type1 && count_type2 < limit_type2) {
                    system_halted = 0;
                    GPIOB->BSRR = (1 << 12); // Bật lại băng tải
                }
                force_render = 1;
                btn_pressed = 1;
            }
            if (down == 0 && last_down == 1) {
                if (current_page == 0 && limit_type0 > 1) limit_type0--;
                else if (current_page == 1 && limit_type1 > 1) limit_type1--;
                else if (current_page == 2 && limit_type2 > 1) limit_type2--;
                force_render = 1;
                btn_pressed = 1;
            }
        }

        // --- THUẬT TOÁN CHỐNG RUNG (SOFTWARE DEBOUNCE) ---
        if (btn_pressed) {
            vTaskDelay(pdMS_TO_TICKS(250)); // Khóa, không cho bấm liên tiếp trong 250ms
            // Sau khi hết khóa, đọc lại trạng thái thực tế để xóa nhiễu
            last_tab = (GPIOA->IDR & (1 << 3)) ? 1 : 0;
            last_set = (GPIOA->IDR & (1 << 4)) ? 1 : 0;
            last_up  = (GPIOA->IDR & (1 << 5)) ? 1 : 0;
            last_down = (GPIOA->IDR & (1 << 6)) ? 1 : 0;
        } else {
            last_tab = tab; last_set = set; last_up = up; last_down = down;
        }

        // Cập nhật giá trị đếm mới mỗi 500ms
        if (xTaskGetTickCount() - last_update > pdMS_TO_TICKS(500) && render_step == 0) {
            force_render = 1;
            last_update = xTaskGetTickCount();
        }

        // --- 3. RENDER CHO MÀN HÌNH 1602 (Tối đa 16 ký tự/dòng) ---
        // Theo dõi sự thay đổi của cờ pc_disconnected để ép render ngay
        static uint8_t last_pc_state = 0;
        if (pc_disconnected != last_pc_state) {
            force_render = 1;
            last_pc_state = pc_disconnected;
        }
        if (force_render && hlcd.state == LCD_SM_IDLE) {
            if (render_step == 0) render_step = 1;
            
            switch (render_step) {
                case 1:
                    lcd_setcursor(&hlcd, 0, 0);
                    
                    // --- ƯU TIÊN 1: HIỂN THỊ LỖI PC ---
                    if (pc_disconnected == 1) {
                        my_strcpy(buffer, "LOI KET NOI PC! ");
                    } 
                    // --- ƯU TIÊN 2: CÀI ĐẶT NGƯỠNG ---
                    else if (in_setting_mode) {
                        my_strcpy(buffer, "CAI NGUONG L");
                        char p[2] = {current_page + '0', '\0'};
                        my_strcat(buffer, p);
                    } 
                    // --- ƯU TIÊN 3: CHẾ ĐỘ CHẠY BÌNH THƯỜNG ---
                    else {
                        my_strcpy(buffer, "LOAI ");
                        char p[2] = {current_page + '0', '\0'};
                        my_strcat(buffer, p);
                        if (system_halted) my_strcat(buffer, " [DUNG]");
                    }
                    
                    while (my_strlen(buffer) < 16) my_strcat(buffer, " "); 
                    lcd_print_string(&hlcd, buffer);
                    render_step = 2;
                    break;

                case 2:
                    lcd_setcursor(&hlcd, 0, 1);
                    
                    // --- ƯU TIÊN 1: LỖI PC ---
                    if (pc_disconnected == 1) {
                        my_strcpy(buffer, "Bang tai ngat...");
                    } 
                    // --- ƯU TIÊN 2 & 3: HIỂN THỊ SỐ ---
                    else {
                        if (in_setting_mode) my_strcpy(buffer, "Limit: ");
                        else my_strcpy(buffer, "So luong: ");
                        
                        char num_buf[11];
                        uint32_t cur_val = 0;
                        if (current_page == 0) cur_val = (in_setting_mode) ? limit_type0 : count_type0;
                        else if (current_page == 1) cur_val = (in_setting_mode) ? limit_type1 : count_type1;
                        else if (current_page == 2) cur_val = (in_setting_mode) ? limit_type2 : count_type2;
                        
                        uint32_to_string(cur_val, num_buf);
                        my_strcat(buffer, num_buf);
                    }
                    
                    while (my_strlen(buffer) < 16) my_strcat(buffer, " "); 
                    lcd_print_string(&hlcd, buffer);
                    
                    render_step = 0;     
                    force_render = 0;
                    break;
            }
        }
        lcd_task(&hlcd);
        vTaskDelay(pdMS_TO_TICKS(10)); // Nhường CPU
    }
}

// ========================================================
// HÀM DEBOUNCE A0: TẠO ID MỚI & CHỐNG NHIỄU BÓNG MỜ
// ========================================================
void vTimerCallback_Debounce0(TimerHandle_t xTimer) {
    static uint8_t is_locked = 0;       // 0: Đang rảnh đón vật mới, 1: Đang chờ vật cũ qua hết
    static uint8_t continuous_high = 0; // Đếm số lần cảm biến trống

    if (is_locked == 0) {
        if ((GPIOA->IDR & (1 << 0)) == 0) { // Có vật thật
            global_item_id++;
            UART1_SendString("A0:");
            UART1_SendNumber(global_item_id);
            UART1_SendString("\n");
            
            is_locked = 1;       // Khóa, không nhận thêm ID
            continuous_high = 0; 
        } else {
            // Nhiễu chớp nhoáng xẹt qua, bỏ qua và bật lại ngắt
            EXTI->PR = EXTI_PR_PR0; 
            EXTI->IMR |= EXTI_IMR_MR0;
            return;
        }
    }
    
    // Nếu đang bị khóa (vật đang ở trên cảm biến)
    if (is_locked == 1) {
        if ((GPIOA->IDR & (1 << 0)) != 0) { // Cảm biến báo không thấy vật
            continuous_high++;
            if (continuous_high >= 3) { // Phải trống liên tục 3 lần (60ms) mới tin là qua hết
                is_locked = 0;
                continuous_high = 0;
                
                // Mở khóa: Bật lại ngắt EXTI để đón vật mới
                EXTI->PR = EXTI_PR_PR0; 
                EXTI->IMR |= EXTI_IMR_MR0; 
                return; // Kết thúc quét
            }
        } else {
            continuous_high = 0; // Cảm biến lại thấy vật -> Reset biến đếm trống
        }
        
        // Vật chưa qua hết, tự động gọi lại Timer này sau 20ms để quét tiếp
        xTimerStart(xTimer_Debounce0, 0); 
    }
}

// ========================================================
// HÀM DEBOUNCE A1: CHỐNG KÍCH PISTON ĐÚP & TRỘM QUEUE
// ========================================================
void vTimerCallback_Debounce1(TimerHandle_t xTimer) {
    static uint8_t is_locked = 0;
    static uint8_t continuous_high = 0;

    if (is_locked == 0) {
        if ((GPIOA->IDR & (1 << 1)) == 0) { 
            ConveyorItem_t current_item;
            if (xQueueReceive(xQueue_A1, &current_item, 0) == pdPASS) { 
                if (current_item.type == 1) {
                    GPIOB->BSRR = (1 << 13); 
                    count_type1++;
                    Check_Limit();
                    xTimerStart(xTimer_P1, 0); 
                    xQueueSend(xQueue_Log, &current_item, 0); 
                } else {
                    xQueueSend(xQueue_A2, &current_item, 0); 
                }
            }
            is_locked = 1;
            continuous_high = 0;
        } else {
            EXTI->PR = EXTI_PR_PR1; 
            EXTI->IMR |= EXTI_IMR_MR1;
            return;
        }
    }
    
    if (is_locked == 1) {
        if ((GPIOA->IDR & (1 << 1)) != 0) {
            continuous_high++;
            if (continuous_high >= 3) { 
                is_locked = 0;
                continuous_high = 0;
                EXTI->PR = EXTI_PR_PR1; 
                EXTI->IMR |= EXTI_IMR_MR1;
                return;
            }
        } else {
            continuous_high = 0;
        }
        xTimerStart(xTimer_Debounce1, 0);
    }
}

// ========================================================
// HÀM DEBOUNCE A2: CHỐNG KÍCH PISTON ĐÚP & TRỘM QUEUE
// ========================================================
void vTimerCallback_Debounce2(TimerHandle_t xTimer) {
    static uint8_t is_locked = 0;
    static uint8_t continuous_high = 0;

    if (is_locked == 0) {
        if ((GPIOA->IDR & (1 << 2)) == 0) { 
            ConveyorItem_t current_item;
            if (xQueueReceive(xQueue_A2, &current_item, 0) == pdPASS) { 
                if (current_item.type == 2) {
                    GPIOB->BSRR = (1 << 14); 
                    count_type2++;
                    Check_Limit();
                    xTimerStart(xTimer_P2, 0); 
                    xQueueSend(xQueue_Log, &current_item, 0); 
                } else if (current_item.type == 0) {
                    count_type0++;
                    Check_Limit();
                    xQueueSend(xQueue_Log, &current_item, 0); 
                }
            }
            is_locked = 1;
            continuous_high = 0;
        } else {
            EXTI->PR = EXTI_PR_PR2; 
            EXTI->IMR |= EXTI_IMR_MR2;
            return;
        }
    }
    
    if (is_locked == 1) {
        if ((GPIOA->IDR & (1 << 2)) != 0) {
            continuous_high++;
            if (continuous_high >= 3) { 
                is_locked = 0;
                continuous_high = 0;
                EXTI->PR = EXTI_PR_PR2; 
                EXTI->IMR |= EXTI_IMR_MR2;
                return;
            }
        } else {
            continuous_high = 0;
        }
        xTimerStart(xTimer_Debounce2, 0);
    }
}

// Nếu hàm này bị gọi, nghĩa là đã 1.5s trôi qua không nhận được PING từ Python
void vTimerCallback_Watchdog(TimerHandle_t xTimer) {
    if (pc_disconnected == 0) {
        pc_disconnected = 1;      // Bật cờ lỗi PC
        system_halted = 1;        // Khóa hệ thống
        GPIOB->BRR = (1 << 12);   // Tắt ngay băng tải
    }
}

volatile char rx_buf[25];
volatile uint8_t rx_idx = 0;

void USART1_IRQHandler(void) {
    if (USART1->SR & USART_SR_RXNE) {
        char c = USART1->DR;
        
        if (c == '\n' || c == '\r') {
            rx_buf[rx_idx] = '\0'; 
            
            if (rx_idx > 0) {
                // 1. Xử lý HEARTBEAT PING
                // Xử lý HEARTBEAT PING (bên trong USART1_IRQHandler)
                if (rx_buf[0]=='P' && rx_buf[1]=='I' && rx_buf[2]=='N' && rx_buf[3]=='G') {
                    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                    xTimerResetFromISR(xTimer_PC_Watchdog, &xHigherPriorityTaskWoken);
                    
                    // Nếu trước đó đang mất kết nối, giờ có lại
                    if (pc_disconnected == 1) {
                        pc_disconnected = 0; // Xóa cờ lỗi
                        
                        // Kiểm tra xem số lượng đã đầy chưa, nếu chưa đầy thì chạy tiếp
                        if (count_type0 < limit_type0 && count_type1 < limit_type1 && count_type2 < limit_type2) {
                            system_halted = 0;
                            GPIOB->BSRR = (1 << 12); // Bật lại băng tải
                        }
                    }
                    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                }
                // 2. Xử lý DỮ LIỆU AI TRẢ VỀ: "AI:id,type"
                else if (rx_buf[0]=='A' && rx_buf[1]=='I' && rx_buf[2]==':') {
                    uint32_t p_id = 0;
                    uint8_t p_type = 0;
                    uint8_t i = 3;
                    
                    while(rx_buf[i] != ',' && rx_buf[i] != '\0') {
                        p_id = p_id * 10 + (rx_buf[i] - '0');
                        i++;
                    }
                    
                    if (rx_buf[i] == ',') {
                        p_type = rx_buf[i+1] - '0';
                        
                        ConveyorItem_t newItem = {p_id, p_type};
                        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                        // Nhét vật thể vào hàng đợi A1
                        xQueueSendFromISR(xQueue_A1, &newItem, &xHigherPriorityTaskWoken);
                        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                    }
                }
            }
            rx_idx = 0; // Reset buffer cho chuỗi mới
        } 
        else {
            // Chống tràn buffer nếu bị nhiễu dây UART
            if (rx_idx < 24) rx_buf[rx_idx++] = c;
            else rx_idx = 0; 
        }
    }
}

int main(void) {
    SystemClock_Config();
    SystemCoreClockUpdate();
    Hardware_Init(); // Hàm Init chân GPIO/EXTI ở Bước 2

    // 1. Tạo Queues
    xQueue_A1 = xQueueCreate(10, sizeof(ConveyorItem_t));
    xQueue_A2 = xQueueCreate(10, sizeof(ConveyorItem_t));
    xQueue_Log = xQueueCreate(10, sizeof(ConveyorItem_t));

    // 2. Tạo Software Timers (Chế độ pdFALSE = One-shot, chạy 1 lần rồi tắt)
    // 500 là thời gian Piston bật (ms), (void*)1 là ID để phân biệt Piston
    xTimer_P1 = xTimerCreate("Tim_P1", pdMS_TO_TICKS(500), pdFALSE, (void*)1, vTimerCallback_Piston);
    xTimer_P2 = xTimerCreate("Tim_P2", pdMS_TO_TICKS(500), pdFALSE, (void*)2, vTimerCallback_Piston);

    xTimer_Debounce0 = xTimerCreate("Deb0", pdMS_TO_TICKS(20), pdFALSE, (void*)0, vTimerCallback_Debounce0);
    xTimer_Debounce1 = xTimerCreate("Deb1", pdMS_TO_TICKS(20), pdFALSE, (void*)1, vTimerCallback_Debounce1);
    xTimer_Debounce2 = xTimerCreate("Deb2", pdMS_TO_TICKS(20), pdFALSE, (void*)2, vTimerCallback_Debounce2);    

    xTimer_PC_Watchdog = xTimerCreate("WDG", pdMS_TO_TICKS(1500), pdFALSE, 0, vTimerCallback_Watchdog);
    xTimerStart(xTimer_PC_Watchdog, 0);

    // 3. Tạo Tasks
    xTaskCreate(Task_UART_Logging, "UART_LOG", 256, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(Task_LCD_Display, "LCD_DISP", 256, NULL, tskIDLE_PRIORITY + 2, NULL);

    // Khởi động hệ điều hành
    vTaskStartScheduler();

    while(1) {}
}