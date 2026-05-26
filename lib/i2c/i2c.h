#ifndef I2C_H
#define I2C_H

#include <stdint.h>
#include <stm32f1xx.h>
#include "FreeRTOS.h"      
#include "semphr.h"

// Định nghĩa các trạng thái trả về của I2C
typedef enum {
    I2C_OK = 0,
    I2C_ERROR,
    I2C_BUSY,
    I2C_TIMEOUT
} I2C_Status_t;

// Khi dùng ngắt I2C cần biết trạng thái đang ở đâu
// enum này định nghĩa các trạng thái cần có trong quá trình giao tiếp I2C
typedef enum {
    I2C_STATE_START_SENT = 0,
    I2C_STATE_ADDR_SENT,
    I2C_STATE_DATA_SENDING,
    I2C_STATE_DONE
} I2C_State_t;

// busy cần volatile vì nó do ISR thay đổi trong lúc main đang chạy nếu không có compler sẽ tối ưu hóa biến này thành một biến thường và không cập nhật giá trị mới nhất khi main đọc nó, dẫn đến lỗi logic trong chương trình. 
// Việc sử dụng volatile đảm bảo rằng mỗi lần main đọc biến busy, nó sẽ lấy giá trị mới nhất được cập nhật bởi ISR, từ đó tránh được các vấn đề về đồng bộ và đảm bảo hoạt động chính xác của hệ thống.
// tx_data và tx_length không cần volatile vì chúng chỉ được thay đổi trong main, không bị ảnh hưởng bởi ISR
typedef struct {
    I2C_TypeDef *Instance; // Con trỏ đến thanh ghi của I2C (I2C1, I2C2, ...)
    uint16_t address;       // Địa chỉ của thiết bị I2C cần giao tiếp
    volatile uint8_t busy; // Cờ báo bận để tránh xung đột khi có nhiều tác vụ cùng sử dụng I2C
    uint8_t *tx_data;       // Con trỏ đến buffer dữ liệu
    uint8_t tx_length;       // Độ dài của buffer
    uint8_t tx_index;        // Chỉ số hiện tại trong buffer đang được gửi
    volatile I2C_State_t state;       // Trạng thái hiện tại của quá trình giao
    uint32_t ClockSpeed;   // Tốc độ giao tiếp (ví dụ: 100000 cho 100kHz)
    volatile I2C_Status_t error;
    SemaphoreHandle_t semaphore;
} I2C_Handle_t;

I2C_Status_t I2C_Init(I2C_Handle_t *hi2c);

// Hàm này sẽ khởi động quá trình truyền dữ liệu qua I2C bằng ngắt dùng non-blocking, trả về ngay sau khi khởi động quá trình mà không chờ đợi hoàn thành. 
// Dữ liệu sẽ được gửi từng byte một trong ISR cho đến khi hoàn thành, và trạng thái của quá trình sẽ được cập nhật trong hi2c->state để main có thể kiểm tra. 
I2C_Status_t I2C_Master_Transmit_IT(I2C_Handle_t *hi2c, uint16_t address, uint8_t *data, uint8_t length); 
I2C_Status_t I2C_WaitUntilReady(I2C_Handle_t *hi2c);
#endif
