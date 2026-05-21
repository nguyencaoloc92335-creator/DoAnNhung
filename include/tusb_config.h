#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

// -------------------------------------------------------------------------
// 1. CẤU HÌNH HỆ THỐNG CƠ BẢN
// -------------------------------------------------------------------------
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_NONE
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU            OPT_MCU_STM32F1
#endif

#define CFG_TUD_ENABLED         1
#define BOARD_TUD_MAX_SPEED     OPT_MODE_FULL_SPEED
#define BOARD_TUD_RHPORT        0

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

// -------------------------------------------------------------------------
// 2. CẤU HÌNH BỘ NHỚ & ENDPOINT
// -------------------------------------------------------------------------
#define CFG_TUD_ENDPOINT0_SIZE  64

// -------------------------------------------------------------------------
// 3. BẬT/TẮT CÁC LỚP THIẾT BỊ (USB CLASSES)
// -------------------------------------------------------------------------

#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// -------------------------------------------------------------------------
// 4. CẤU HÌNH RIÊNG CHO LỚP CDC
// -------------------------------------------------------------------------

#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  64

#define CFG_TUD_CDC_EP_BUFSIZE  64

#ifdef __cplusplus
 }
#endif

#endif