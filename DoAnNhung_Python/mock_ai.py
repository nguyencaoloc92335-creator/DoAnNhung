import serial
import time
import threading
import queue
import csv
import cv2
from ultralytics import YOLO

# --- CẤU HÌNH SERIAL ---
SERIAL_PORT = 'COM20'  # Thay đổi theo thực tế cổng COM của STM32
BAUD_RATE = 115200
CSV_FILE = 'ai_log.csv'

# --- CẤU HÌNH CAMERA & AI ---
CAMERA_INDEX = 0      # ID của Camera (0 là cam mặc định)
Y_LINE_TOP =   100    # Toạ độ Y của đường line trên
Y_LINE_BOTTOM = 480   # Toạ độ Y của đường line dưới
CONF_THRESHOLD = 0.6  # Ngưỡng tự tin (chỉ nhận vật > 60% chắc chắn)
IOU_THRESHOLD = 0.4   # Ngưỡng IoU để lọc các khung hình trùng lặp

# --- BẢNG ÁNH XẠ ID (YOLO -> STM32) ---
# Cú pháp: YOLO_CLASS_ID : (MÃ_GỬI_STM32, "TÊN_HIỂN_THỊ_MÀN_HÌNH")
CLASS_MAPPING = {
    0: (1, "Chin (1)"),    # YOLO ID 0 (ripe)    -> STM32 ID 1
    1: (2, "Xanh (2)"),    # YOLO ID 1 (unripe)  -> STM32 ID 2
    2: (0, "Hong (0)")     # YOLO ID 2 (damaged) -> STM32 ID 0
}

print("[*] Dang load model (Vui long doi)...")
model = YOLO('phanloaicachua_cocanbangdulieu_final.pt') 
print("[*] Load model thanh cong!")

# Hàng đợi & Biến toàn cục
ai_queue = queue.Queue()
latest_results = None         # Biến lưu trữ kết quả AI mới nhất
data_lock = threading.Lock()  # Khóa chống xung đột luồng
ser = None
current_stm32_obj_id = "N/A"  # Lưu trữ ID vật thể mới nhất mà STM32 yêu cầu

# Khởi tạo file CSV
with open(CSV_FILE, mode='a', newline='', encoding='utf-8') as f:
    writer = csv.writer(f)
    if f.tell() == 0: 
        writer.writerow(['ID', 'Type', 'Status', 'Timestamp'])

def connect_serial():
    global ser
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
        print(f"[*] Da ket noi voi {SERIAL_PORT}")
    except Exception as e:
        print(f"[!] Loi ket noi: {e}")

connect_serial()

def save_to_csv(obj_id, obj_type):
    while True:
        try:
            with open(CSV_FILE, mode='a', newline='', encoding='utf-8') as f:
                csv.writer(f).writerow([obj_id, obj_type, "DONE", time.strftime("%H:%M:%S")])
            break
        except PermissionError:
            print(f"\n[!] CANH BAO: Khong the ghi vao {CSV_FILE}. Vui long dong file Excel!")
            time.sleep(2)
        except Exception:
            break

# ---------------------------------------------------------
# LUỒNG 1: WATCHDOG HEARTBEAT 
# ---------------------------------------------------------
def heartbeat_worker():
    global ser
    while True:
        try:
            if ser and ser.is_open:
                ser.write(b"PING\n")
        except Exception:
            pass 
        time.sleep(0.5)

# ---------------------------------------------------------
# LUỒNG 2: AI WORKER (Xử lý yêu cầu từ STM32)
# ---------------------------------------------------------
def ai_worker():
    global latest_results
    
    while True:
        obj_id = ai_queue.get() 
        print(f"[AI] Nhan lenh phan tich ID: {obj_id} tu STM32...")
        
        best_type = -1 
        max_conf = 0.0
        
        # Đọc lấy kết quả AI ngay tại khoảnh khắc STM32 ra lệnh
        with data_lock:
            current_results = latest_results
            
        if current_results is not None:
            # Quét các vật thể đang có trên màn hình
            for box in current_results.boxes:
                y1, y2 = int(box.xyxy[0][1]), int(box.xyxy[0][3])
                conf = float(box.conf[0])
                cls_id = int(box.cls[0])
                
                # Tính tâm Y của vật thể
                center_y = int((y1 + y2) / 2)
                
                # Chỉ lọc lấy vật thể có tâm rơi vào giữa 2 đường line
                if Y_LINE_TOP <= center_y <= Y_LINE_BOTTOM:
                    if conf > max_conf:
                        max_conf = conf
                        best_type = cls_id 
        
        # Logic gửi dữ liệu cho STM32 sử dụng CLASS_MAPPING
        if best_type != -1:
            # Lấy mã gửi STM32 từ bảng ánh xạ, mặc định là 99 nếu class lạ
            mapped_data = CLASS_MAPPING.get(best_type, (99, "Unknown"))
            final_type = mapped_data[0] 
        else:
            # Nếu AI không thấy gì trong vùng line
            final_type = 99 
            
        cmd = f"AI:{obj_id},{final_type}\n"
        
        try:
            if ser and ser.is_open:
                ser.write(cmd.encode('utf-8'))
                print(f"<-- [Gui UART] {cmd.strip()} (YOLO ID: {best_type} -> STM32 Mã: {final_type} | Do chinh xac: {max_conf*100:.1f}%)")
        except Exception:
            pass
            
        ai_queue.task_done()

# ---------------------------------------------------------
# LUỒNG 3: LẮNG NGHE SERIAL
# ---------------------------------------------------------
def serial_listener():
    global ser, current_stm32_obj_id
    while True:
        try:
            if ser and ser.is_open and ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if not line: continue
                
                if line.startswith("A0:"):
                    obj_id = line.split(":")[1]
                    current_stm32_obj_id = obj_id # Lưu lại ID để hiển thị lên camera
                    ai_queue.put(obj_id)          # Giao việc cho AI Worker

                elif line.startswith("DONE:"):
                    data = line.split(":")[1].split(",")
                    print(f"--> [Nhan UART] Hoan thanh phan loai ID {data[0]} (Loai {data[1]})")
                    save_to_csv(data[0], data[1])
                    
        except serial.SerialException as e:
            if ser: ser.close()
            while True:
                time.sleep(2)
                try:
                    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
                    print(f"\n[*] DA KET NOI LAI THANH CONG!\n")
                    break
                except Exception:
                    pass
        except Exception:
            pass

# ---------------------------------------------------------
# HÀM CHÍNH: CHẠY CAMERA VÀ VẼ GIAO DIỆN LIÊN TỤC
# ---------------------------------------------------------
if __name__ == "__main__":
    threading.Thread(target=heartbeat_worker, daemon=True).start()
    threading.Thread(target=ai_worker, daemon=True).start()
    threading.Thread(target=serial_listener, daemon=True).start()
    
    cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        print("[!] Khong the mo Camera!")
        exit()

    print("[*] He thong AI dang chay. Nhan 'q' tren cua so Camera de thoat.")
    
    try:
        while True:
            ret, frame = cap.read()
            if not ret: break
            
            # 1. CHẠY AI LIÊN TỤC TRÊN MỖI KHUNG HÌNH 
            results = model.predict(source=frame, conf=CONF_THRESHOLD, iou=IOU_THRESHOLD, verbose=False)[0]
            
            with data_lock:
                latest_results = results
            
            # 2. VẼ VÙNG XỬ LÝ (ROI) CÓ LỚP PHỦ MỜ
            overlay = frame.copy()
            alpha = 0.2  # Độ trong suốt (0.0 là vô hình, 1.0 là đặc)
            
            # Tô màu xanh biên nguyên vùng ROI trên lớp overlay
            cv2.rectangle(overlay, (0, Y_LINE_TOP), (frame.shape[1], Y_LINE_BOTTOM), (255, 0, 0), -1)
            # Trộn với frame gốc
            cv2.addWeighted(overlay, alpha, frame, 1 - alpha, 0, frame)
            
            # Vẽ ranh giới line
            cv2.line(frame, (0, Y_LINE_TOP), (frame.shape[1], Y_LINE_TOP), (255, 0, 0), 2)
            cv2.line(frame, (0, Y_LINE_BOTTOM), (frame.shape[1], Y_LINE_BOTTOM), (255, 0, 0), 2)
            
            # Hiển thị Tên vùng & Tracking ID từ STM32
            info_text = f"VUNG XU LY (ROI) - Yeu cau tu STM32 ID: {current_stm32_obj_id}"
            cv2.putText(frame, info_text, (10, Y_LINE_TOP - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 0, 0), 2)
            
            # 3. VẼ KHUNG VẬT THỂ (BOUNDING BOX)
            for box in results.boxes:
                x1, y1, x2, y2 = map(int, box.xyxy[0])
                conf = float(box.conf[0])
                cls_id = int(box.cls[0])
                
                center_x = int((x1 + x2) / 2)
                center_y = int((y1 + y2) / 2)
                
                if Y_LINE_TOP <= center_y <= Y_LINE_BOTTOM:
                    box_color = (0, 255, 0) # Xanh Lá
                    status = "IN-ZONE"
                else:
                    box_color = (0, 0, 255) # Đỏ
                    status = "OUT"
                
                # --- TRA CỨU ID ĐỂ HIỂN THỊ TRÊN MÀN HÌNH ---
                mapped_data = CLASS_MAPPING.get(cls_id, (99, f"LoaiLa({cls_id})"))
                stm32_code = mapped_data[0] # Mã ID thật sự gửi cho STM32
                label_name = mapped_data[1] # Tên dễ hiểu
                
                cv2.rectangle(frame, (x1, y1), (x2, y2), box_color, 2)
                cv2.circle(frame, (center_x, center_y), 4, box_color, -1)
                
                # Tạo nhãn text sử dụng ID đã mapping thay vì Class ID của YOLO
                label_text = f"ID: {stm32_code} [{label_name}] | Conf: {conf:.2f} | {status}"
                
                text_y = y1 - 10 if y1 - 10 > 10 else y1 + 20
                cv2.putText(frame, label_text, (x1, text_y), cv2.FONT_HERSHEY_SIMPLEX, 0.5, box_color, 2)
            
            cv2.imshow("He Thong Nhan Dien AI", frame)
            
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
                
    except KeyboardInterrupt:
        pass
    finally:
        print("\n[*] Dang tat he thong...")
        cap.release()
        cv2.destroyAllWindows()
        if ser: ser.close()