#include "stm32f103xb.h"  // Header thanh ghi CMSIS
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "tusb.h"
#include <stdio.h> // Để dùng sprintf
#include <string.h>

// Hàng đợi lưu nhãn vật thể (Giả sử băng tải chứa tối đa 10 vật cùng lúc)
#define QUEUE_LENGTH 10
#define ITEM_SIZE    sizeof(uint8_t)

QueueHandle_t xQueue_A1; // Nhớ nhãn các vật đang trôi từ A0 -> A1
QueueHandle_t xQueue_A2; // Nhớ nhãn các vật đang trôi từ A1 -> A2

// Timer tự động tắt Piston
TimerHandle_t xTimer_P1;
TimerHandle_t xTimer_P2;

// Hàng đợi đẩy Log từ Ngắt (ISR) sang Task USB
QueueHandle_t xQueue_Log;

// Biến đếm thống kê (Hiển thị lên LCD)
volatile uint32_t count_type0 = 0;
volatile uint32_t count_type1 = 0;
volatile uint32_t count_type2 = 0;

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

    // --- CẤU HÌNH CẢM BIẾN (PA0, PA1, PA2) ---
    // Clear cấu hình cũ (bits 0-11)
    GPIOA->CRL &= ~(0x00000FFF);
    // Set Input Pull-up/Pull-down (mode = 00, cnf = 10 -> 0x8)
    GPIOA->CRL |= 0x00000888;
    // Bật Pull-up cho PA0, PA1, PA2 (kéo lên 3.3V)
    GPIOA->ODR |= (1 << 0) | (1 << 1) | (1 << 2);

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

    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
}

// Ngắt Cảm biến A0
void EXTI0_IRQHandler(void) {
    if (EXTI->PR & EXTI_PR_PR0) {
        EXTI->PR = EXTI_PR_PR0; 
        
        // BỘ LỌC CỨNG (Deep Filter): Chờ ~2ms để nhiễu tia lửa điện từ Relay tản hết
        for (volatile int i = 0; i < 72000; i++); 
        
        // Nếu sau 2ms mà chân PA0 vẫn bị kéo xuống 0V thì đích thị là VẬT THẬT
        if ((GPIOA->IDR & (1 << 0)) == 0) { 
            static uint32_t last_interrupt_time = 0;
            uint32_t current_time = xTaskGetTickCountFromISR();
            
            if (current_time - last_interrupt_time > 100) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                uint8_t mock_label = 2; 
                xQueueSendFromISR(xQueue_A1, &mock_label, &xHigherPriorityTaskWoken);
                last_interrupt_time = current_time;
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            }
        }
    }
}

// Ngắt Cảm biến A1 (Piston 1)
void EXTI1_IRQHandler(void) {
    if (EXTI->PR & EXTI_PR_PR1) {
        EXTI->PR = EXTI_PR_PR1; 
        
        // BỘ LỌC CỨNG (Deep Filter): Chờ ~2ms
        for (volatile int i = 0; i < 72000; i++); 
        
        if ((GPIOA->IDR & (1 << 1)) == 0) { // Đảm bảo vật thật vẫn đang che cảm biến
            static uint32_t last_interrupt_time = 0;
            uint32_t current_time = xTaskGetTickCountFromISR();
            
            if (current_time - last_interrupt_time > 100) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                uint8_t incoming_label = 0;
                
                if (xQueueReceiveFromISR(xQueue_A1, &incoming_label, &xHigherPriorityTaskWoken) == pdPASS) {
                    if (incoming_label == 1) {
                        GPIOB->BSRR = (1 << 13); 
                        count_type1++;
                        xTimerStartFromISR(xTimer_P1, &xHigherPriorityTaskWoken);
                        xQueueSendFromISR(xQueue_Log, &incoming_label, &xHigherPriorityTaskWoken);
                    } else {
                        xQueueSendFromISR(xQueue_A2, &incoming_label, &xHigherPriorityTaskWoken);
                    }
                }
                last_interrupt_time = current_time;
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            }
        }
    }
}

// Ngắt Cảm biến A2 (Piston 2)
void EXTI2_IRQHandler(void) {
    if (EXTI->PR & EXTI_PR_PR2) {
        EXTI->PR = EXTI_PR_PR2; 
        
        // BỘ LỌC CỨNG (Deep Filter): Chờ ~2ms
        for (volatile int i = 0; i < 72000; i++); 
        
        if ((GPIOA->IDR & (1 << 2)) == 0) { 
            static uint32_t last_interrupt_time = 0;
            uint32_t current_time = xTaskGetTickCountFromISR();
            
            if (current_time - last_interrupt_time > 100) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                uint8_t incoming_label = 0;
                
                if (xQueueReceiveFromISR(xQueue_A2, &incoming_label, &xHigherPriorityTaskWoken) == pdPASS) {
                    if (incoming_label == 2) {
                        GPIOB->BSRR = (1 << 14); 
                        count_type2++;
                        xTimerStartFromISR(xTimer_P2, &xHigherPriorityTaskWoken);
                        xQueueSendFromISR(xQueue_Log, &incoming_label, &xHigherPriorityTaskWoken);
                    } else if (incoming_label == 0) {
                        count_type0++;
                    }
                }
                last_interrupt_time = current_time;
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            }
        }
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

// Task test hệ thống
void Task_Blink(void *pvParameters) {
    // Bật clock cho Port C (Giả sử LED test ở PC13)
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    
    // Cấu hình PC13 là Output Push-Pull, tốc độ 2MHz
    GPIOC->CRH &= ~(0xF << 20); // Clear bits 20-23
    GPIOC->CRH |= (0x2 << 20);  // Output, 2MHz, Push-Pull

    for(;;) {
        GPIOC->ODR ^= (1 << 13); // Đảo trạng thái PC13
        vTaskDelay(pdMS_TO_TICKS(500)); // Chờ 500ms một cách non-blocking
    }
}

// Task Hợp nhất: Xử lý USB và Ghi Log an toàn
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

    // 3. Tạo Tasks
    // Cấp phát 256 word (1KB RAM) là đủ cho cả USB và Log
    xTaskCreate(Task_USB_And_Logging, "USB_LOG", 256, NULL, configMAX_PRIORITIES - 1, NULL);
    // Task_Blink nháy LED (nếu bạn vẫn giữ lại)

    // Khởi động hệ điều hành
    vTaskStartScheduler();

    while(1) {}
}