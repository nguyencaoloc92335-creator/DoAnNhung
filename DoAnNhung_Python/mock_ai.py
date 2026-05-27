import serial
import time
import threading
import queue
import random
import csv

# --- CẤU HÌNH ---
SERIAL_PORT = 'COM20'  # Thay đổi theo thực tế
BAUD_RATE = 115200
CSV_FILE = 'mock_log.csv'

# Hàng đợi trung chuyển ID từ Luồng Đọc sang Luồng AI
ai_queue = queue.Queue()

# Khởi tạo file CSV
with open(CSV_FILE, mode='a', newline='', encoding='utf-8') as f:
    writer = csv.writer(f)
    writer.writerow(['ID', 'Type', 'Status', 'Timestamp'])

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
    print(f"[*] Da ket noi voi {SERIAL_PORT}")
except Exception as e:
    print(f"[!] Loi ket noi: {e}")
    exit()

def save_to_csv(obj_id, obj_type):
    """Hàm ghi file an toàn, chống mất dữ liệu khi file bị khóa"""
    while True:
        try:
            with open(CSV_FILE, mode='a', newline='', encoding='utf-8') as f:
                csv.writer(f).writerow([obj_id, obj_type, "DONE", time.strftime("%H:%M:%S")])
            break  # Ghi thành công thì thoát vòng lặp
            
        except PermissionError:
            # Báo động đỏ lên màn hình nếu người dùng đang mở file
            print(f"\n[!] CANH BAO: Khong the ghi vao {CSV_FILE}!")
            print(f"[*] Vui long DONG FILE EXCEL lai. He thong dang cho...\n")
            time.sleep(2) # Chờ 2 giây rồi thử ghi lại
            
        except Exception as e:
            print(f"\n[!] LOI GHI FILE KHONG XAC DINH: {e}")
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
            # Nếu lỗi (do đứt cáp), cứ im lặng bỏ qua, để luồng Listener lo việc kết nối lại
            pass 
        time.sleep(0.5)

# ---------------------------------------------------------
# LUỒNG 2: MOCK AI (Người tiêu thụ - Xử lý ảnh giả lập)
# ---------------------------------------------------------
def ai_worker():
    while True:
        # Lấy ID ra khỏi hàng đợi (sẽ block nếu queue trống, không tốn CPU)
        obj_id = ai_queue.get() 
        
        print(f"[AI Mock] Dang 'chup anh' va phan tich ID: {obj_id}...")
        time.sleep(0.15) # Giả lập Model YOLO mất 150ms để chạy
        
        # Giả lập YOLO trả về ngẫu nhiên loại 0, 1 hoặc 2
        mock_type = random.randint(0, 2)
        
        # Gửi kết quả xuống STM32
        cmd = f"AI:{obj_id},{mock_type}\n"
        ser.write(cmd.encode('utf-8'))
        print(f"<-- [Gui] {cmd.strip()}")
        
        ai_queue.task_done()

# ---------------------------------------------------------
# LUỒNG 3: LẮNG NGHE SERIAL (Bổ sung Auto-Reconnect)
# ---------------------------------------------------------
def serial_listener():
    global ser
    while True:
        try:
            # Chỉ đọc khi cổng COM thực sự đang mở
            if ser and ser.is_open and ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if not line: continue
                
                if line.startswith("A0:"):
                    obj_id = line.split(":")[1]
                    print(f"--> [Nhan] Phat hien vat the ID {obj_id} tai A0")
                    ai_queue.put(obj_id)

                elif line.startswith("DONE:"):
                    data = line.split(":")[1].split(",")
                    print(f"--> [Nhan] STM32 da xu ly xong ID {data[0]} (Loai {data[1]})")
                    save_to_csv(data[0], data[1])
                
                else:
                    print(f"[STM32 Log] {line}")
                    
        # BẮT ĐÚNG LỖI ĐỨT CÁP HOẶC RESET CỔNG COM
        except serial.SerialException as e:
            print(f"\n[!] MAT KET NOI USB: {e}")
            print("[*] He thong dang cho cam cap lai...")
            if ser:
                ser.close()
            
            # Vòng lặp cố gắng kết nối lại (Self-Healing)
            while True:
                time.sleep(2) # Đợi 2 giây mỗi lần thử
                try:
                    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
                    print(f"\n[*] DA KET NOI LAI THANH CONG VOI {SERIAL_PORT}!\n")
                    break # Thoát vòng lặp chờ, quay lại đọc dữ liệu
                except Exception:
                    pass # Cứ im lặng thử lại cho đến khi cắm cáp
                    
        except Exception as e:
            print(f"[Loi Parse UART] {e}")

# Kích hoạt hệ thống
if __name__ == "__main__":
    threading.Thread(target=heartbeat_worker, daemon=True).start()
    threading.Thread(target=ai_worker, daemon=True).start()
    threading.Thread(target=serial_listener, daemon=True).start()
    
    print("[*] He thong Mock AI dang chay. Bam Ctrl+C de thoat.")
    try:
        while True: time.sleep(1)
    except KeyboardInterrupt:
        print("\n[*] Tat he thong.")
        ser.close()