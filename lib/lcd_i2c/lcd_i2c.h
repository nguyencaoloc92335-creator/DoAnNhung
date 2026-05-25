#ifndef I2C_LCD_H
#define I2C_LCD_H

#include <stdint.h>
#include <stm32f1xx.h>
#include "i2c.h"
//#include "delay.h"

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

// Cấu trúc để quản lý LCD qua I2C, Lưu ý sắp xếp các biến theo thứ tự giảm dần về kích thước để tối ưu bộ nhớ
typedef struct {
    I2C_TypeDef *I2C; // Con trỏ đến I2C được sử dụng
    uint8_t address;   // Địa chỉ I2C của LCD
    uint8_t col;     // Số cột của LCD
    uint8_t row;     // Số hàng của LCD
}I2C_LCD_HandleTypeDef;

LCD_Status_t lcd_init(I2C_LCD_HandleTypeDef *lcd);
LCD_Status_t lcd_setcursor(I2C_LCD_HandleTypeDef *lcd, uint8_t col, uint8_t row);
LCD_Status_t lcd_print_char(I2C_LCD_HandleTypeDef *lcd, char c);    
LCD_Status_t lcd_print_string(I2C_LCD_HandleTypeDef *lcd, const char *str);

#endif
