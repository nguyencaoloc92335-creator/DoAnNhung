#include "stm32f103xb.h"  
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "tusb.h"
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

// Hàm kiểm tra ngưỡng riêng biệt - Chỉ cần 1 loại đạt ngưỡng là dừng toàn hệ thống
void Check_Limit(void) {
    if (system_halted == 0) {
        if (count_type0 >= limit_type0 || count_type1 >= limit_type1 || count_type2 >= limit_type2) {
            GPIOB->BRR = (1 << 12); // Tắt chân PB12 để dừng băng tải ngay lập tức
            system_halted = 1;
        }
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
    // 1. Bật Clock cho PORTA, PORTB và AFIO (Cực kỳ quan trọng cho ngắt)
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;
    RCC->APB1ENR |= RCC_APB1ENR_USBEN;

    NVIC_SetPriorityGrouping(3);

    // --- ÉP PC NHẬN LẠI USB (FORCE RE-ENUMERATE) ---
    // Cấu hình chân DP (PA12) là Output Push-Pull để kéo xuống GND
    GPIOA->CRH &= ~(0x000F0000); // Clear bits của PA12
    GPIOA->CRH |= 0x00020000;    // Output 2MHz
    GPIOA->BRR = (1 << 12);      // Kéo PA12 xuống LOW
    for(volatile int i = 0; i < 720000; i++); // Trễ cứng ~10ms để PC ngắt kết nối cũ
    // TinyUSB sau đó sẽ tự động lấy lại cấu hình PA12 thành Alternate Function.

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

    // --- CẤU HÌNH NVIC CHO FREERTOS ---
    // BẮT BUỘC: Mức ưu tiên ngắt phải thấp hơn (số lớn hơn) configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (đang là 5)
    NVIC_SetPriority(EXTI0_IRQn, 6);
    NVIC_SetPriority(EXTI1_IRQn, 6);
    NVIC_SetPriority(EXTI2_IRQn, 6);

    // BẮT BUỘC: Cấu hình ưu tiên ngắt cho USB (Priority phải >= 5)
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 6);
    NVIC_SetPriority(I2C1_EV_IRQn, 6);

    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    
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

// Hàm này sẽ dùng chung cho cả 2 Piston
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

void Task_USB_And_Logging(void *pvParameters) {
    tusb_init(); // Khởi tạo TinyUSB
    uint8_t log_label = 0;
    char log_buffer[] = "LOG,0\r\n";
    
    for(;;) {
        tud_task(); // Luôn chạy để duy trì sự sống cho USB
        
        // Đọc Queue KHÔNG CHẶN (tham số 0) để không làm kẹt tud_task
        if (xQueueReceive(xQueue_Log, &log_label, 0) == pdPASS) {
            if (tud_cdc_connected()) {
                log_buffer[4] = log_label + '0';
                tud_cdc_write(log_buffer, 7);
                tud_cdc_write_flush();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1)); // Nhường 1ms CPU cho các Task khác
    }
}

// --- HÀM NGẮT BẮT BUỘC CHO TINYUSB ---
void USB_LP_CAN1_RX0_IRQHandler(void) {
    tud_int_handler(0);
}
void USBWakeUp_IRQHandler(void) {
    tud_int_handler(0);
}

void Task_LCD_Display(void *pvParameters) {
    // 1. Khởi tạo I2C và cấu hình màn hình LCD 1602
    hi2c1.Instance = I2C1;
    hi2c1.ClockSpeed = 100000;
    I2C_Init(&hi2c1);

    hlcd.hi2c = &hi2c1;
    hlcd.address = 0x27; 
    hlcd.col = 16;       // Chỉnh thành 16 cột
    hlcd.row = 2;        // Chỉnh thành 2 hàng
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
        if (force_render && hlcd.state == LCD_SM_IDLE) {
            if (render_step == 0) render_step = 1;
            char num_buf[11];
            
            switch (render_step) {
                case 1:
                    // VẼ HÀNG 1 (Tiêu đề trang)
                    lcd_setcursor(&hlcd, 0, 0);
                    if (in_setting_mode) {
                        my_strcpy(buffer, "CAI NGUONG L");
                    } else {
                        my_strcpy(buffer, "LOAI ");
                    }
                    
                    char p[2] = {current_page + '0', '\0'};
                    my_strcat(buffer, p);

                    // Thêm thông báo nếu hệ thống DỪNG ở trang hiển thị
                    if (!in_setting_mode && system_halted) {
                        my_strcat(buffer, " [DUNG]");
                    }
                    
                    while (my_strlen(buffer) < 16) my_strcat(buffer, " "); // Pad đúng 16 dấu cách
                    lcd_print_string(&hlcd, buffer);
                    render_step = 2;
                    break;

                case 2:
                    // VẼ HÀNG 2 (Dữ liệu)
                    lcd_setcursor(&hlcd, 0, 1);
                    if (in_setting_mode) {
                        my_strcpy(buffer, "Limit: ");
                        uint32_t cur_limit = (current_page == 0) ? limit_type0 : ((current_page == 1) ? limit_type1 : limit_type2);
                        uint32_to_string(cur_limit, num_buf);
                        my_strcat(buffer, num_buf);
                    } else {
                        my_strcpy(buffer, "So luong: ");
                        uint32_t cur_count = (current_page == 0) ? count_type0 : ((current_page == 1) ? count_type1 : count_type2);
                        uint32_to_string(cur_count, num_buf);
                        my_strcat(buffer, num_buf);
                    }
                    
                    while (my_strlen(buffer) < 16) my_strcat(buffer, " "); // Pad đúng 16 dấu cách
                    lcd_print_string(&hlcd, buffer);
                    
                    render_step = 0;     // Hoàn thành
                    force_render = 0;
                    break;
            }
        }

        lcd_task(&hlcd);
        vTaskDelay(pdMS_TO_TICKS(10)); // Nhường CPU
    }
}

void vTimerCallback_Debounce0(TimerHandle_t xTimer) {
    // Đã qua 2ms, kiểm tra lại xem chân PA0 có thực sự đang bị kéo xuống 0V không
    if ((GPIOA->IDR & (1 << 0)) == 0) { 
        static uint32_t last_trigger_time = 0;
        uint32_t current_time = xTaskGetTickCount(); // Dùng API thường
        
        if (current_time - last_trigger_time > pdMS_TO_TICKS(100)) {
            uint8_t mock_label = 1; 
            xQueueSend(xQueue_A1, &mock_label, 0); // Dùng API thường, timeout = 0
            last_trigger_time = current_time;
        }
    }
    // Xóa cờ ngắt rác sinh ra trong 2ms vừa qua và BẬT LẠI ngắt EXTI0
    EXTI->PR = EXTI_PR_PR0; 
    EXTI->IMR |= EXTI_IMR_MR0; 
}

void vTimerCallback_Debounce1(TimerHandle_t xTimer) {
    if ((GPIOA->IDR & (1 << 1)) == 0) { 
        static uint32_t last_trigger_time = 0;
        uint32_t current_time = xTaskGetTickCount();
        
        if (current_time - last_trigger_time > pdMS_TO_TICKS(100)) {
            uint8_t incoming_label = 0;
            
            if (xQueueReceive(xQueue_A1, &incoming_label, 0) == pdPASS) { // Dùng API thường
                if (incoming_label == 1) {
                    GPIOB->BSRR = (1 << 13); 
                    count_type1++;
                    Check_Limit();
                    xTimerStart(xTimer_P1, 0); // Khởi động Timer Piston bằng API thường
                    xQueueSend(xQueue_Log, &incoming_label, 0); 
                } else {
                    xQueueSend(xQueue_A2, &incoming_label, 0); 
                }
            }
            last_trigger_time = current_time;
        }
    }
    EXTI->PR = EXTI_PR_PR1; 
    EXTI->IMR |= EXTI_IMR_MR1;
}

void vTimerCallback_Debounce2(TimerHandle_t xTimer) {
    if ((GPIOA->IDR & (1 << 2)) == 0) { 
        static uint32_t last_trigger_time = 0;
        uint32_t current_time = xTaskGetTickCount();
        
        if (current_time - last_trigger_time > pdMS_TO_TICKS(100)) {
            uint8_t incoming_label = 0;
            
            if (xQueueReceive(xQueue_A2, &incoming_label, 0) == pdPASS) { 
                if (incoming_label == 2) {
                    GPIOB->BSRR = (1 << 14); 
                    count_type2++;
                    Check_Limit();
                    xTimerStart(xTimer_P2, 0); 
                    xQueueSend(xQueue_Log, &incoming_label, 0); 
                } else if (incoming_label == 0) {
                    count_type0++;
                    Check_Limit();
                }
            }
            last_trigger_time = current_time;
        }
    }
    EXTI->PR = EXTI_PR_PR2; 
    EXTI->IMR |= EXTI_IMR_MR2;
}

int main(void) {
    SystemClock_Config();
    SystemCoreClockUpdate();
    Hardware_Init(); // Hàm Init chân GPIO/EXTI ở Bước 2

    // 1. Tạo Queues
    xQueue_A1 = xQueueCreate(10, sizeof(uint8_t));
    xQueue_A2 = xQueueCreate(10, sizeof(uint8_t));
    xQueue_Log = xQueueCreate(10, sizeof(uint8_t));

    // 2. Tạo Software Timers (Chế độ pdFALSE = One-shot, chạy 1 lần rồi tắt)
    // 500 là thời gian Piston bật (ms), (void*)1 là ID để phân biệt Piston
    xTimer_P1 = xTimerCreate("Tim_P1", pdMS_TO_TICKS(500), pdFALSE, (void*)1, vTimerCallback_Piston);
    xTimer_P2 = xTimerCreate("Tim_P2", pdMS_TO_TICKS(500), pdFALSE, (void*)2, vTimerCallback_Piston);

    xTimer_Debounce0 = xTimerCreate("Deb0", pdMS_TO_TICKS(2), pdFALSE, (void*)0, vTimerCallback_Debounce0);
    xTimer_Debounce1 = xTimerCreate("Deb1", pdMS_TO_TICKS(2), pdFALSE, (void*)1, vTimerCallback_Debounce1);
    xTimer_Debounce2 = xTimerCreate("Deb2", pdMS_TO_TICKS(2), pdFALSE, (void*)2, vTimerCallback_Debounce2);

    // 3. Tạo Tasks
    // Cấp phát 256 word (1KB RAM) là đủ cho cả USB và Log
    xTaskCreate(Task_USB_And_Logging, "USB_LOG", 256, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(Task_LCD_Display, "LCD_DISP", 256, NULL, tskIDLE_PRIORITY + 2, NULL);

    // Khởi động hệ điều hành
    vTaskStartScheduler();

    while(1) {}
}