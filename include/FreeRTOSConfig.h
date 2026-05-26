#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Cấu hình cơ bản */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      ( 72000000 ) // Xung nhịp hệ thống 72MHz
#define configTICK_RATE_HZ                      ( 1000 )     // 1 Tick = 1ms
#define configMAX_PRIORITIES                    ( 5 )
#define configMINIMAL_STACK_SIZE                ( 128 )      // Tính bằng word (128 * 4 = 512 bytes)
#define configMAX_TASK_NAME_LEN                 ( 16 )
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0

/* Quản lý bộ nhớ (Heap) - Cực kỳ quan trọng cho 20KB RAM */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 10240 ) ) // Cấp 10KB RAM cho FreeRTOS, giữ lại 10KB cho biến toàn cục và TinyUSB

/* Timer phần mềm (Dùng cho Piston sau này) */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            configMINIMAL_STACK_SIZE

/* Định tuyến lại Vector Ngắt (QUAN TRỌNG VỚI BARE-METAL) */
/* Nếu không có 3 dòng này, FreeRTOS không thể chuyển đổi task và sẽ treo MCU */
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

/* Mức ưu tiên ngắt (Cấu hình Cortex-M3) */
#ifdef __NVIC_PRIO_BITS
 #define configPRIO_BITS __NVIC_PRIO_BITS
#else
 #define configPRIO_BITS 4 
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY              ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY         ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

/* Tùy chọn bật/tắt các API của FreeRTOS */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_xTaskGetSchedulerState          1

#endif /* FREERTOS_CONFIG_H */