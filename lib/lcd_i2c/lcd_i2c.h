#ifndef I2C_LCD_H
#define I2C_LCD_H

#include <stdint.h>
#include <stm32f1xx.h>
#include "i2c.h"

#define LCD_RS 0
#define LCD_RW 1
#define LCD_EN 2
#define LCD_BL 3

// Cấu hình các trạng thái của LCD
typedef enum {
    LCD_ok    = 0,
    LCD_error,
    LCD_busy,
    LCD_timeout
}LCD_Status_t;

typedef enum {
    LCD_SM_IDLE = 0,        // Đang rảnh, chờ dữ liệu mới
    LCD_SM_SENDING_HIGH,    // Bắt đầu gửi Nibble cao
    LCD_SM_WAIT_HIGH,       // Chờ I2C gửi xong Nibble cao
    LCD_SM_SENDING_LOW,     // Bắt đầu gửi Nibble thấp
    LCD_SM_WAIT_LOW         // Chờ I2C gửi xong Nibble thấp
} LCD_SM_State_t;

// Cấu trúc để quản lý LCD qua I2C, Lưu ý sắp xếp các biến theo thứ tự giảm dần về kích thước để tối ưu bộ nhớ
typedef struct {
    I2C_Handle_t *hi2c;
    uint8_t address;
    uint8_t col;
    uint8_t row;
    
    // --- Các biến phục vụ State Machine ---
    volatile LCD_SM_State_t state;
    char buffer[64];        // Bộ đệm chứa chuỗi cần in
    uint8_t buf_index;      // Vị trí ký tự đang xử lý
    uint8_t buf_length;     // Tổng số ký tự cần in
    uint8_t temp_i2c_data[2]; // Mảng chứa 2 byte (EN=1 và EN=0) để I2C gửi đi
    uint8_t current_rs;     // Trạng thái RS (0: Lệnh, 1: Dữ liệu)
} I2C_LCD_HandleTypeDef;

LCD_Status_t lcd_init(I2C_LCD_HandleTypeDef *lcd);
LCD_Status_t lcd_setcursor(I2C_LCD_HandleTypeDef *lcd, uint8_t col, uint8_t row);
LCD_Status_t lcd_print_char(I2C_LCD_HandleTypeDef *lcd, char c);    
LCD_Status_t lcd_print_string(I2C_LCD_HandleTypeDef *lcd, const char *str);
void lcd_task(I2C_LCD_HandleTypeDef *lcd);

#endif
