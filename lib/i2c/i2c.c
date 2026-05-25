#include "i2c.h"

static I2C_Handle_t *hi2c1_global; // Biến toàn cục để lưu trữ con trỏ đến I2C_Handle_t, giúp ISR có thể truy cập và cập nhật trạng thái của quá trình giao tiếp I2C
static I2C_Handle_t *hi2c2_global; 

static uint32_t I2C_GetAPB1FreqMHz(void)
{
    uint32_t ppre1 = (RCC->CFGR & RCC_CFGR_PPRE1_Msk) >> RCC_CFGR_PPRE1_Pos; // Đọc giá trị của PPRE1 từ thanh ghi CFGR
    uint32_t prescaler = (ppre1 < 4) ? 1 : (1 << (ppre1 - 3)); // Tính toán hệ số chia dựa trên giá trị của PPRE1, nếu PPRE1 < 4 thì không chia, ngược lại chia theo công thức 2^(PPRE1-3)
    return SystemCoreClock / prescaler / 1000000; // Trả về tần số APB1 tính bằng MHz, SystemCoreClock là tần số của hệ thống (thường là 72MHz), chia cho hệ số chia để có tần số thực tế của APB1, sau đó chia cho 1 triệu để chuyển đổi từ Hz sang MHz
}

I2C_Status_t I2C_Init(I2C_Handle_t *hi2c) {
    // Đọc 26.3.3 của datasheet để biết cách cấu hình I2C
    if (hi2c->Instance == I2C1)
    {
        RCC->APB2ENR |= RCC_APB2ENR_IOPBEN; // Bật clock cho GPIOB (I2C1 thường sử dụng PB6 và PB7)
        RCC->APB1ENR |= RCC_APB1ENR_I2C1EN; // Bật clock cho I2C1
        GPIOB->CRL &= ~(0b11 << (GPIO_CRL_MODE6_Pos)); // Xóa cấu hình cũ của PB6
        GPIOB->CRL &= ~(0b11 << (GPIO_CRL_CNF6_Pos)); // Xóa cấu hình cũ của PB6
        GPIOB->CRL |= (0b11 << (GPIO_CRL_CNF6_Pos)); // Đặt PB6 thành output open-drain
        GPIOB->CRL |= (0b11 << (GPIO_CRL_MODE6_Pos)); // Đặt PB6 thành output open-drain max speed 50MHz
        GPIOB->CRL &= ~(0b11 << (GPIO_CRL_MODE7_Pos)); // Xóa cấu hình cũ của PB7
        GPIOB->CRL &= ~(0b11 << (GPIO_CRL_CNF7_Pos)); // Xóa cấu hình cũ của PB7
        GPIOB->CRL |= (0b11 << (GPIO_CRL_CNF7_Pos)); // Đặt PB7 thành output open-drain max speed 50MHz
        GPIOB->CRL |= (0b11 << (GPIO_CRL_MODE7_Pos)); // Đặt PB7 thành output open-drain
        // Có thể dùng lệnh như sau cho ngắn gọn nhưng viết tách ra cho dễ hiểu, khi làm slide hãy nói cả 2 cách
        // GPIOB->CRL &= ~(0xFF << 24); // Xóa cấu hình cũ của PB6 và PB7
        // GPIOB->CRL |= (0xFF << 24); // Đặt PB6 và PB7 thành output open-drain max speed 50MHz
    }else if (hi2c->Instance == I2C2)
    {
        RCC->APB2ENR |= RCC_APB2ENR_IOPBEN; // Bật clock cho GPIOB (I2C2 thường sử dụng PB10 và PB11)
        RCC->APB1ENR |= RCC_APB1ENR_I2C2EN; // Bật clock cho I2C2
        GPIOB->CRH &= ~(0b11 << (GPIO_CRH_MODE10_Pos)); // Xóa cấu hình cũ của PB10
        GPIOB->CRH &= ~(0b11 << (GPIO_CRH_CNF10_Pos)); // Xóa cấu hình cũ của PB10
        GPIOB->CRH |= (0b11 << (GPIO_CRH_CNF10_Pos)); // Đặt PB10 thành output open-drain
        GPIOB->CRH |= (0b11 << (GPIO_CRH_MODE10_Pos)); // Đặt PB10 thành output open-drain max speed 50MHz
        GPIOB->CRH &= ~(0b11 << (GPIO_CRH_MODE11_Pos)); // Xóa cấu hình cũ của PB11
        GPIOB->CRH &= ~(0b11 << (GPIO_CRH_CNF11_Pos)); // Xóa cấu hình cũ của PB11
        GPIOB->CRH |= (0b11 << (GPIO_CRH_CNF11_Pos)); // Đặt PB11 thành output open-drain max speed 50MHz
        GPIOB->CRH |= (0b11 << (GPIO_CRH_MODE11_Pos)); // Đặt PB11 thành output open-drain
    } else {
        return I2C_ERROR; // Báo lỗi nếu Instance không hợp lệ
    }

    // Reset I2C để đảm bảo nó ở trạng thái mặc định
    hi2c->Instance->CR1 |= I2C_CR1_SWRST; // Ghi bit SWRST trong thanh ghi CR1 để reset I2C
    hi2c->Instance->CR1 &= ~I2C_CR1_SWRST; // Xóa bit SWRST để kết thúc quá trình reset, đưa I2C trở lại trạng thái hoạt động bình thường
 
    // Cấu hình FREQ 
    uint32_t apb1_freq_mhz = I2C_GetAPB1FreqMHz(); // Lấy tần số APB1 tính bằng MHz
    hi2c->Instance->CR2 &= ~I2C_CR2_FREQ_Msk;       // Xóa sạch vùng bit FREQ
    hi2c->Instance->CR2 |= (apb1_freq_mhz << I2C_CR2_FREQ_Pos); // Ghi tốc độ clock của APB1 vào FREQ
    // FREQ = Tốc độ clock của APB1 (36MHz) được ghi vào thanh ghi CR2 để I2C biết được tần số hoạt động của nó, từ đó tính toán thời gian cho các tín hiệu I2C như START, STOP, ACK, v.v. Nếu không cấu hình đúng FREQ, I2C sẽ không hoạt động chính xác và có

    // Cấu hình CCR 
    hi2c->Instance->CCR &= ~I2C_CCR_CCR_Msk;        // Xóa sạch vùng bit CCR
    hi2c->Instance->CCR |= (apb1_freq_mhz * 1000000 / (2 * hi2c->ClockSpeed)) << I2C_CCR_CCR_Pos; 
    // Tính toán giá trị CCR dựa trên công thức CCR = F_PCLK1 / (2 * F_SCL) cho chế độ chuẩn (Sm mode)
    // Sau đó ghi vào thanh ghi CCR. F_PCLK1 là tần số clock của APB1 (36MHz), F_SCL là tốc độ giao tiếp I2C mà bạn muốn (ví dụ: 100kHz). 

    hi2c->Instance->TRISE &= ~I2C_TRISE_TRISE_Msk;   // Xóa sạch vùng bit TRISE
    hi2c->Instance->TRISE |= (apb1_freq_mhz + 1) << I2C_TRISE_TRISE_Pos;
    // TRISE = (t_rise_max × f_PCLK1) + 1
    //       = (1000ns × 36MHz) + 1
    //       = 36 + 1 = 37
    // Hoặc rút gọn cho Sm mode: TRISE = FREQ_value + 1 = 36 + 1 = 37
    // Công thức này được ghi vào thanh ghi TRISE để I2C biết được thời gian tối đa cho phép để tín hiệu trên bus tăng lên từ mức thấp đến mức cao. Nếu không cấu hình đúng TRISE, I2C có thể không đáp ứng được yêu cầu về thời gian của các tín hiệu, dẫn đến lỗi giao tiếp.

    hi2c->Instance->CR1 |= I2C_CR1_PE; // Bật I2C bằng cách ghi bit PE trong thanh ghi CR1
    return I2C_OK;
}

I2C_Status_t I2C_Master_Transmit_IT(I2C_Handle_t *hi2c, uint16_t address, uint8_t *data, uint8_t length) {
    if (hi2c->busy) {return I2C_BUSY;} // Nếu I2C đang bận, trả về trạng thái bận để main biết không thể khởi động quá trình mới ngay lúc này

    if (hi2c->Instance == I2C1) hi2c1_global = hi2c; // Lưu con trỏ đến I2C_Handle_t vào biến toàn cục để ISR có thể truy cập và cập nhật trạng thái của quá trình giao tiếp I2C
    else if (hi2c->Instance == I2C2) hi2c2_global = hi2c;
    else return I2C_ERROR;
    
    hi2c->error = I2C_OK;

    // truyền tham số vào hi2c để ISR có thể sử dụng khi quá trình truyền dữ liệu diễn ra trong ngắt
    hi2c->busy = 1; // Đặt cờ bận để báo rằng I2C đang được sử dụng
    hi2c->address = address; // Lưu địa chỉ của thiết bị I2C
    hi2c->tx_data = data; // Lưu con trỏ đến buffer dữ liệu
    hi2c->tx_length = length; // Lưu độ dài của buffer
    hi2c->tx_index = 0; // Khởi tạo chỉ số gửi dữ liệu
    hi2c->state = I2C_STATE_START_SENT; // Đặt trạng thái ban đầu là đã gửi START

    hi2c->Instance->CR2 |= I2C_CR2_ITEVTEN; // Bật ngắt sự kiện I2C
    hi2c->Instance->CR2 |= I2C_CR2_ITBUFEN; // Bật ngắt bộ đệm I2C
    if (hi2c->Instance == I2C1) {
        NVIC_EnableIRQ(I2C1_EV_IRQn); // Kích hoạt ngắt I2C1 event trong NVIC, nếu bạn dùng I2C2 thì thay bằng I2C2_EV_IRQn
    }else if (hi2c->Instance == I2C2) {
        NVIC_EnableIRQ(I2C2_EV_IRQn); // Kích hoạt ngắt I2C2 event trong NVIC
    } else {
        return I2C_ERROR; // Báo lỗi nếu Instance không hợp lệ
    }
    hi2c->Instance->CR1 |= I2C_CR1_START; // Ghi bit START trong thanh ghi CR1 để bắt đầu quá trình truyền dữ liệu, sau khi START được gửi đi, quá trình truyền sẽ tiếp tục trong ISR dựa trên trạng thái hiện tại và các sự kiện xảy ra trên bus I2C
    return I2C_OK; // Trả về trạng thái OK để báo rằng quá trình
}

void I2C1_EV_IRQHandler(void) {
    uint32_t sr1 = hi2c1_global->Instance->SR1; // Đọc thanh ghi SR1 để xác định sự kiện nào đã xảy ra, điều này cũng sẽ xóa cờ ngắt tương ứng
    // Kiểm tra xem slave có gửi ACK không
    if (sr1 & I2C_SR1_AF)
    {
        hi2c1_global->Instance->CR1 |= I2C_CR1_STOP; // Gửi tín hiệu STOP vì lỗi rồi
        hi2c1_global->Instance->SR1 &= ~I2C_SR1_AF; // xóa cờ AF-cờ báo lỗi không nhận được ACK
        hi2c1_global->error = I2C_ERROR; // ghi vào là lỗi
        hi2c1_global->busy = 0; // Xóa cờ bận để báo rằng I2C đã sẵn sàng cho quá trình mới
        hi2c1_global->Instance->CR2 &= ~I2C_CR2_ITEVTEN; // Tắt ngắt sự kiện I2C
        hi2c1_global->Instance->CR2 &= ~I2C_CR2_ITBUFEN; // Tắt ngắt buffer
        NVIC_DisableIRQ(I2C1_EV_IRQn);
        return;
    }
    
    if (sr1 & I2C_SR1_SB) {
        hi2c1_global->Instance->DR = (hi2c1_global->address << 1); // Gửi địa chỉ thiết bị I2C kèm bit R/W (0 cho write) vào thanh ghi DR
        hi2c1_global->state = I2C_STATE_ADDR_SENT; // Cập nhật trạng thái là đã gửi địa chỉ
    }else if (sr1 & I2C_SR1_ADDR) {
        (void)hi2c1_global->Instance->SR2; // Đọc SR2 để xóa cờ ADDR, void để tránh cảnh báo unused variable
        hi2c1_global->state = I2C_STATE_DATA_SENDING; // Cập nhật trạng thái là đang gửi dữ liệu
    }else if (sr1 & I2C_SR1_TXE) {
        if (hi2c1_global->tx_index < hi2c1_global->tx_length) {
            hi2c1_global->Instance->DR = hi2c1_global->tx_data[hi2c1_global->tx_index++]; // Gửi byte dữ liệu tiếp theo vào thanh ghi DR và tăng chỉ số
        } else {
            hi2c1_global->Instance->CR1 |= I2C_CR1_STOP; // Gửi tín hiệu STOP khi đã gửi hết dữ liệu
            hi2c1_global->state = I2C_STATE_DONE; // Cập nhật trạng thái là đã hoàn thành
            hi2c1_global->busy = 0; // Xóa cờ bận để báo rằng I2C đã sẵn sàng cho quá trình mới
            hi2c1_global->Instance->CR2 &= ~I2C_CR2_ITEVTEN; // Tắt ngắt sự kiện I2C
            hi2c1_global->Instance->CR2 &= ~I2C_CR2_ITBUFEN; // Tắt ngắt buffer
            NVIC_DisableIRQ(I2C1_EV_IRQn); // Tắt ngắt I2C1 event trong NVIC
        }
    }
}

void I2C2_EV_IRQHandler(void) {
    uint32_t sr1 = hi2c2_global->Instance->SR1; // Đọc thanh ghi SR1 để xác định sự kiện nào đã xảy ra, điều này cũng sẽ xóa cờ ngắt tương ứng
    // Kiểm tra xem slave có gửi ACK không
    if (sr1 & I2C_SR1_AF)
    {
        hi2c2_global->Instance->CR1 |= I2C_CR1_STOP; // Gửi tín hiệu STOP vì lỗi rồi
        hi2c2_global->Instance->SR1 &= ~I2C_SR1_AF; // xóa cờ AF-cờ báo lỗi không nhận được ACK
        hi2c2_global->error = I2C_ERROR; // ghi vào là lỗi
        hi2c2_global->busy = 0; // Xóa cờ bận để báo rằng I2C đã sẵn sàng cho quá trình mới
        hi2c2_global->Instance->CR2 &= ~I2C_CR2_ITEVTEN; // Tắt ngắt sự kiện I2C
        hi2c2_global->Instance->CR2 &= ~I2C_CR2_ITBUFEN; // Tắt ngắt buffer
        NVIC_DisableIRQ(I2C2_EV_IRQn);
        return;
    }
    
    if (sr1 & I2C_SR1_SB) {
        hi2c2_global->Instance->DR = (hi2c2_global->address << 1); // Gửi địa chỉ thiết bị I2C kèm bit R/W (0 cho write) vào thanh ghi DR
        hi2c2_global->state = I2C_STATE_ADDR_SENT; // Cập nhật trạng thái là đã gửi địa chỉ
    }else if (sr1 & I2C_SR1_ADDR) {
        (void)hi2c2_global->Instance->SR2; // Đọc SR2 để xóa cờ ADDR, void để tránh cảnh báo unused variable
        hi2c2_global->state = I2C_STATE_DATA_SENDING; // Cập nhật trạng thái là đang gửi dữ liệu
    }else if (sr1 & I2C_SR1_TXE) {
        if (hi2c2_global->tx_index < hi2c2_global->tx_length) {
            hi2c2_global->Instance->DR = hi2c2_global->tx_data[hi2c2_global->tx_index++]; // Gửi byte dữ liệu tiếp theo vào thanh ghi DR và tăng chỉ số
        } else {
            hi2c2_global->Instance->CR1 |= I2C_CR1_STOP; // Gửi tín hiệu STOP khi đã gửi hết dữ liệu
            hi2c2_global->state = I2C_STATE_DONE; // Cập nhật trạng thái là đã hoàn thành
            hi2c2_global->busy = 0; // Xóa cờ bận để báo rằng I2C đã sẵn sàng cho quá trình mới
            hi2c2_global->Instance->CR2 &= ~I2C_CR2_ITEVTEN; // Tắt ngắt sự kiện I2C
            hi2c2_global->Instance->CR2 &= ~I2C_CR2_ITBUFEN; // Tắt ngắt buffer
            NVIC_DisableIRQ(I2C2_EV_IRQn); // Tắt ngắt I2C2 event trong NVIC

        }
    }
}

// Chờ đến khi I2C rảnh (blocking nhưng ngắn)
I2C_Status_t I2C_WaitUntilReady(I2C_Handle_t *hi2c) {
    uint32_t timeout = 100000;
    while (hi2c->busy) {
        if (--timeout == 0) return I2C_TIMEOUT;
    }
    return hi2c->error;
}
