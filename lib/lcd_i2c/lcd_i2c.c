#include "lcd_i2c.h"

extern void delay_ms(uint32_t ms);

static LCD_Status_t LCD_Write_nibble(I2C_LCD_HandleTypeDef *lcd, uint8_t nibble_data, uint8_t rs_state)
{
    uint8_t data[2];
    
    // Byte 0: EN = 1 (Tạo sườn lên / trạng thái High để IC dịch dữ liệu)
    data[0] = nibble_data | (rs_state << LCD_RS) | (1 << LCD_EN) | (1 << LCD_BL); 
    
    // Byte 1: EN = 0 (Tạo sườn xuống / trạng thái Low để chốt dữ liệu)
    data[1] = nibble_data | (rs_state << LCD_RS) | (0 << LCD_EN) | (1 << LCD_BL); 
    
    // Gửi liên tục 2 byte qua I2C ngắt
    if (I2C_Master_Transmit_IT(lcd->hi2c, lcd->address, data, 2) != I2C_OK) {
        return LCD_error;
    }
    
    // Phải chờ ngắt I2C báo rảnh trước khi thoát hàm, vì mảng 'data' là biến cục bộ.
    // Nếu thoát hàm ngay, mảng data sẽ bị ghi đè gây sai lỗi logic trên đường truyền.
    if (I2C_WaitUntilReady(lcd->hi2c) != I2C_OK) {
        return LCD_error;
    }

    return LCD_ok;
}

static LCD_Status_t LCD_Write_cmd(I2C_LCD_HandleTypeDef *lcd, uint8_t cmd)
{
    LCD_Write_nibble(lcd, cmd & 0xF0, 0);        // Gửi nửa cao của lệnh với RS = 0 
    LCD_Write_nibble(lcd, (cmd << 4) & 0xF0, 0); // Gửi nửa thấp của lệnh với RS = 0
    return LCD_ok;
}

static LCD_Status_t LCD_Write_data(I2C_LCD_HandleTypeDef *lcd, uint8_t data)
{
    LCD_Write_nibble(lcd, data & 0xF0, 1);        // Gửi nửa cao của dữ liệu với RS = 1
    LCD_Write_nibble(lcd, (data << 4) & 0xF0, 1); // Gửi nửa thấp của dữ liệu với RS = 1
    return LCD_ok;
}

LCD_Status_t lcd_init(I2C_LCD_HandleTypeDef *lcd)
{
    // theo ảnh 24 trang 46 datasheet HD44780, cần gửi 0x03 3 lần để đảm bảo LCD được khởi động đúng cách, sau đó mới có thể chuyển sang chế độ 4-bit
    // phải dịch 4 vì trong hàm LCD_Write_nibble, dữ liệu sẽ được dịch sang trái 4 bit để phù hợp với giao tiếp 4-bit của LCD
    // vì hàm chỉ hoạt động 1 lần nên việc làm tròn thời gian delay không ảnh hưởng đến hiệu suất của hệ thống, đồng thời đảm bảo LCD có đủ thời gian để khởi động và nhận lệnh đúng cách.
    delay_ms(50);
    LCD_Write_nibble(lcd, (0x03 << 4), 0); // Gửi lệnh 0x03 (8-bit mode) lần 1
    delay_ms(5);
    LCD_Write_nibble(lcd, (0x03 << 4), 0); // Gửi lệnh 0x03 (8-bit mode) lần 2
    delay_ms(1);
    LCD_Write_nibble(lcd, (0x03 << 4), 0); // Gửi lệnh 0x03 (8-bit mode) lần 3
    delay_ms(1); // delay để đảm bảo LCD đã nhận lệnh 0x03 lần cuối cùng đọc bảng 6 trang 24 datasheet HD44780 mỗi lệnh cần tối thiểu 47 us để thực thi
    LCD_Write_nibble(lcd, (0x02 << 4), 0); // Gửi lệnh 0x02 để chuyển sang chế độ 4-bit
    delay_ms(1);
    // theo bảng 6 trang 25 datasheet HD44780, sau khi đã chuyển sang chế độ 4-bit, cần gửi các lệnh để cấu hình LCD
    // Cấu trúc gửi cho LCD như sau RS-gửi lệnh nên là 0 sẵn, R/W-là 0 vì ở chế độ ghi (tín hiệu vật lí) + DB7 DB6 DB5 DB4 DB3 DB2 DB1 DB0 (giá trị logic)
    LCD_Write_cmd(lcd, 0x28); // DL = 0 (4-bit mode), N = 1 (2 dòng), F = 0 (font 5x8) => (00) 0010 1000 = 0x28
    delay_ms(1);
    LCD_Write_cmd(lcd, 0x0D); // D = 1 (bật hiển thị), C = 0 (tắt con trỏ), B = 1 (bật nhấp nháy) => (00) 0000 1101 = 0x0D
    delay_ms(1);
    LCD_Write_cmd(lcd, 0x01); // Lệnh xóa màn hình
    delay_ms(2); 
    LCD_Write_cmd(lcd, 0x06); // I/D = 1 (tăng địa chỉ DDRAM sau mỗi lần ghi), S = 0 (không di chuyển màn hình) => (00) 0000 0110 = 0x06
    delay_ms(1);

    return LCD_ok;
}

LCD_Status_t lcd_setcursor(I2C_LCD_HandleTypeDef *lcd, uint8_t col, uint8_t row)
{
    // theo bảng 7 trang 25 datasheet HD44780, địa chỉ DDRAM được tính như sau:
    // - Dòng 1: 0x00 đến 0x27
    // - Dòng 2: 0x40 đến 0x67
    // Để tính địa chỉ DDRAM từ cột và hàng, ta có thể sử dụng công thức:
    // Dùng toán tử 3 ngôi ?: Nếu row == 0 (dòng 1), địa chỉ sẽ là col, nếu row == 1 (dòng 2), địa chỉ sẽ là col + 0x40
    uint8_t address = (row == 0) ? col : (col + 0x40);
    return LCD_Write_cmd(lcd, 0x80 | address); // Lệnh thiết lập DDRAM address có dạng 1AAAAAAA, trong đó AAAAAAA là địa chỉ DDRAM
}

LCD_Status_t lcd_print_char(I2C_LCD_HandleTypeDef *lcd, char c)
{
    return LCD_Write_data(lcd, (uint8_t)c); // Gửi ký tự đến LCD
}

LCD_Status_t lcd_print_string(I2C_LCD_HandleTypeDef *lcd, const char *str) 
{
    // 1. Kiểm tra xem LCD có đang bận in chuỗi khác không (Chống ghi đè buffer)
    if (lcd->state != LCD_SM_IDLE) {
        return LCD_busy; 
    }

    // 2. Nạp dữ liệu vào buffer cục bộ của LCD VÀ tính toán độ dài
    uint8_t i = 0;
    while (str[i] != '\0' && i < (sizeof(lcd->buffer) - 1)) {
        lcd->buffer[i] = str[i];
        i++;
    }
    lcd->buffer[i] = '\0'; // Đảm bảo luôn có ký tự kết thúc chuỗi
    
    // 3. Khởi tạo các thông số cho State Machine
    lcd->buf_length = i; // Biến i lúc này chính là độ dài của chuỗi
    lcd->buf_index = 0;
    lcd->current_rs = 1; 
    
    // 4. Kích hoạt State Machine
    lcd->state = LCD_SM_SENDING_HIGH; 
    
    return LCD_ok;
}

void lcd_task(I2C_LCD_HandleTypeDef *lcd) {
    // Nếu I2C phần cứng đang bận truyền luồng khác, thoát ngay (Non-blocking)
    if (lcd->hi2c->busy || (lcd->hi2c->Instance->SR2 & I2C_SR2_BUSY)) {
        return; 
    }

    switch (lcd->state) {
        case LCD_SM_IDLE:
            // Có dữ liệu mới trong buffer cần in
            if (lcd->buf_index < lcd->buf_length) {
                lcd->state = LCD_SM_SENDING_HIGH;
            }
            break;

        case LCD_SM_SENDING_HIGH:
            {
                uint8_t data = lcd->buffer[lcd->buf_index];
                uint8_t nibble_high = data & 0xF0;
                
                // Chuẩn bị mảng 2 byte để chốt sườn xuống
                lcd->temp_i2c_data[0] = nibble_high | (lcd->current_rs << LCD_RS) | (1 << LCD_EN) | (1 << LCD_BL);
                lcd->temp_i2c_data[1] = nibble_high | (lcd->current_rs << LCD_RS) | (0 << LCD_EN) | (1 << LCD_BL);
                
                I2C_Master_Transmit_IT(lcd->hi2c, lcd->address, lcd->temp_i2c_data, 2);
                lcd->state = LCD_SM_WAIT_HIGH;
            }
            break;

        case LCD_SM_WAIT_HIGH:
            // Do đầu hàm đã check I2C rảnh, nếu lọt vào case này tức là đã gửi xong Nibble cao
            lcd->state = LCD_SM_SENDING_LOW;
            break;

        case LCD_SM_SENDING_LOW:
            {
                uint8_t data = lcd->buffer[lcd->buf_index];
                uint8_t nibble_low = (data << 4) & 0xF0;
                
                lcd->temp_i2c_data[0] = nibble_low | (lcd->current_rs << LCD_RS) | (1 << LCD_EN) | (1 << LCD_BL);
                lcd->temp_i2c_data[1] = nibble_low | (lcd->current_rs << LCD_RS) | (0 << LCD_EN) | (1 << LCD_BL);
                
                I2C_Master_Transmit_IT(lcd->hi2c, lcd->address, lcd->temp_i2c_data, 2);
                lcd->state = LCD_SM_WAIT_LOW;
            }
            break;

        case LCD_SM_WAIT_LOW:
            // Gửi xong Nibble thấp -> Hoàn thành 1 ký tự
            lcd->buf_index++;
            if (lcd->buf_index >= lcd->buf_length) {
                lcd->state = LCD_SM_IDLE; // Xong toàn bộ chuỗi
            } else {
                lcd->state = LCD_SM_SENDING_HIGH; // Tiếp tục ký tự tiếp theo
            }
            break;
    }
}
