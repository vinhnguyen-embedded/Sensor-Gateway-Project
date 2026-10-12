# Sensor Gateway

## 1. Giới thiệu

Sensor Gateway là chương trình viết bằng C trên Linux dùng để thu nhận, xử lý, lưu trữ và ghi log dữ liệu từ nhiều sensor node thông qua TCP.

Hệ thống hiện hỗ trợ 5 loại sensor:

- Temperature
- Humidity
- Motion
- Door
- Smoke/Gas

Gateway sử dụng 3 worker thread chính:

- **Connection Manager**: nhận kết nối TCP, phân tích protocol và đưa dữ liệu đã chuẩn hóa vào shared buffer.
- **Data Manager**: kiểm tra sensor hợp lệ và xử lý dữ liệu theo từng loại sensor.
- **Storage Manager**: đọc dữ liệu từ shared buffer và lưu vào SQLite.

Ngoài ra, hệ thống có một **Logger Process** riêng. Các thành phần gửi log qua FIFO và logger ghi dữ liệu vào `gateway.log`.

---

## 2. Kiến trúc

Luồng dữ liệu chính:

```text
Sensor Nodes
     |
     | TCP
     v
Connection Manager
     |
     v
   sbuffer
   /     \
  v       v
Data     Storage
Manager  Manager
           |
           v
        SQLite
```

Luồng ghi log:

```text
MAIN / CONN / DATA / STOR
           |
           v
        logFifo
           |
           v
     Logger Process
           |
           v
      gateway.log
```

Gateway sử dụng 4 cổng TCP liên tiếp tính từ `base_port` để hỗ trợ 4 protocol:

```text
P     -> TBP -> Temperature
P + 1 -> HLP -> Humidity
P + 2 -> EVP -> Motion / Door
P + 3 -> SGP -> Smoke/Gas
```

Khi kết thúc chương trình, main process sẽ:

```text
Báo các worker dừng
        |
        v
sbuffer_shutdown()
        |
        v
pthread_join()
        |
        v
Đóng SQLite / socket / FIFO
        |
        v
waitpid() Logger Process
        |
        v
Giải phóng tài nguyên
```

---

## 3. Cấu trúc thư mục

```text
.
├── include/
│   ├── connection_manager.h
│   ├── data_manager.h
│   ├── gateway.h
│   ├── logger.h
│   ├── protocol_adapter.h
│   ├── sbuffer.h
│   ├── sensor_config.h
│   ├── sensor_types.h
│   └── storage_manager.h
│
├── src/
│   ├── main.c
│   ├── connection_manager.c
│   ├── data_manager.c
│   ├── logger.c
│   ├── protocol_adapter.c
│   ├── sbuffer.c
│   └── storage_manager.c
│
├── test/
│   └── sensor_node.c
│
├── room_sensor.txt
└── Makefile
```

Một số file được sinh ra khi chương trình chạy:

```text
gateway.db   -> cơ sở dữ liệu SQLite
gateway.log  -> file log của hệ thống
logFifo      -> FIFO dùng để truyền log
```

---

## 4. Build

### Build bình thường

```bash
make
```

### Build ở chế độ debug

```bash
make debug
```

### Build chương trình mô phỏng sensor

```bash
make sensor_node
```

### Xóa file build

```bash
make clean
```

---

## 5. Run

Chạy gateway:

```bash
./sensor_gateway <base_port> <http_port>
```

Ví dụ:

```bash
./sensor_gateway 1234 8080
```

Trong đó:

- `1234` là cổng gốc cho các sensor.
- Gateway sử dụng các cổng `1234`, `1235`, `1236`, `1237`.
- `8080` là HTTP port dùng cho giai đoạn Web sau này.

Có thể chạy thông qua Makefile:

```bash
make run
```

---

## 6. Test

### Temperature

```bash
./sensor_node temperature 10 127.0.0.1 1234 1000
```

### Humidity

```bash
./sensor_node humidity 21 127.0.0.1 1234 1000
```

### Motion

```bash
./sensor_node motion 30 127.0.0.1 1234 1500
```

### Door

```bash
./sensor_node door 40 127.0.0.1 1234 1500
```

### Smoke/Gas

```bash
./sensor_node smoke_gas 50 127.0.0.1 1234 2000
```

Có thể kiểm tra thêm việc phân khung và xử lý frame lỗi bằng:

```bash
--fragment
--glue
--corrupt
```

Ví dụ:

```bash
./sensor_node smoke_gas 50 127.0.0.1 1234 2000 --corrupt
```

### Kiểm tra memory bằng Valgrind

```bash
make valgrind
```

### Kiểm tra thread synchronization bằng Helgrind

```bash
make helgrind
```

---

## 7. Kết quả kiểm thử

Các kiểm thử đã thực hiện:

- Nhiều sensor kết nối đồng thời và gửi dữ liệu qua TCP.
- Temperature và Humidity tính running average.
- Motion và Door xử lý theo event.
- Smoke/Gas xử lý cảnh báo tức thời.
- Dữ liệu được lưu vào SQLite.
- Log được gửi qua FIFO và ghi vào `gateway.log`.
- Khi SQLite bị lỗi, Storage Manager retry tối đa 3 lần rồi yêu cầu gateway shutdown có kiểm soát.
- Gateway có thể shutdown bằng `Ctrl+C`.
- Các worker thread được `pthread_join()`.
- Logger process được `waitpid()` để tránh zombie process.
- FIFO, SQLite, socket và bộ nhớ được cleanup khi kết thúc.

Kết quả Valgrind:

```text
ERROR SUMMARY: 0 errors
```

Kết quả Helgrind:

```text
ERROR SUMMARY: 0 errors from 0 contexts
```

Như vậy phần lõi Sensor Gateway từ Step 0 đến Step 7 đã hoạt động ổn định và kết thúc có kiểm soát.
