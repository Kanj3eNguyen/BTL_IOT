# CA cho gateway MQTTS local

Với `CONFIG_DRIVER_MQTT_GUARD_ENABLED=y`, CMake nhúng **ingest-root-ca.pem**
để ESP32 xác minh máy chủ gateway trên laptop ở `mqtts://IP_PC:8885`.
CA là điểm tin cậy công khai, khác khóa HMAC, mật khẩu MQTT và private key.
Đây chưa phải xác thực client bằng chứng chỉ/mTLS: bo dùng password MQTT
và thông điệp có HMAC, còn TLS xác minh máy chủ.

## Sinh và dùng chứng chỉ

Chạy setup lần đầu hoặc provision từ **gốc repo**, dùng IP PC trên LAN:

```powershell
.\.venv\Scripts\python.exe -m server.mqtt_guard.provision --host IP_CUA_PC --configure-board
```

Thay `IP_CUA_PC` bằng IPv4 thật trước khi chạy. Với máy chưa setup đầy đủ,
làm theo [local A–Z](../../../../docs/CHAY_LOCAL_A_Z.md) thay vì chỉ chạy lệnh này.

- `.private/mqtt_guard/ca.pem`: CA công khai cho client tin cậy.
- `ca-key.pem`: khóa CA riêng; `server.pem`/`server-key.pem`: chứng chỉ/khóa
  server. Tất cả nằm trong `.private/mqtt_guard` và không commit.
- Provision chép CA công khai thành `main/certs/ingest-root-ca.pem`, cập
  nhật URI/SNTP/credentials trong sdkconfig. Bản chép này cũng bị Git bỏ qua.
- `EMBED_TXTFILES` nhúng CA vào firmware; `driver_telemetry.cpp` đặt CA trong
  cấu hình xác minh broker. Bo kiểm tra CA, IP/hostname và hạn chứng chỉ.

Private key của CA/server không được nhúng vào firmware. Khóa HMAC phần mềm
của bo là bí mật khác được biên dịch từ sdkconfig; không chia sẻ firmware
đã chứa cấu hình cá nhân.

## Đổi mạng hoặc IP PC

Provision giữ khóa HMAC/mật khẩu hiện có cho cùng device. Khi host đổi,
phiên bản hiện tại sinh lại CA/chứng chỉ và cập nhật IP SAN; phải restart
gateway để nạp chứng chỉ mới, build/nạp bo để thay URI/SNTP/CA nhúng. Chỉ
chép PEM trên PC hoặc nhấn RST không thay CA trong flash.

Khi IP giữ nguyên và đủ ca.pem, server.pem, server-key.pem cùng marker
certificate-host.txt khớp IP, provision tái sử dụng bộ cũ. Khi điều kiện đó
không đạt, code sinh lại cả CA và chứng chỉ server. Mặc định CA có hạn 3650
ngày, chứng chỉ server 365 ngày. Code hiện chưa tự gia hạn hoặc kiểm tra hạn
trong nhánh tái sử dụng; chạy lại provision cùng IP không bảo đảm gia hạn
chứng chỉ đã hết hạn. Kiểm tra thời hạn khi xử lý lỗi TLS, không bỏ xác minh.

Một CA có thể dùng cho ESP32 và client thử trên máy ảo cùng tin gateway.
Chia sẻ riêng ca.pem công khai không làm lộ khóa ký CA/HMAC và không cho
client quyền đăng nhập MQTT. Bản hiện tại không cần CA mới cho tài khoản
demo. Khi đổi IP làm sinh CA mới, tất cả client dùng CA cũ cần được cập nhật.
Chi tiết [phạm vi thử từ máy ảo](../../../../docs/DEMO_BAO_MAT_VA_KIEM_THU.md#11-máy-ảo-cùng-lan-phương-án-mở-rộng-chưa-có-runner-từ-xa).

Các private key được lưu bằng PEM không mã hóa trong thư mục riêng trên
laptop. Việc Git bỏ qua file không thay thế quyền truy cập hệ điều hành.

Bo lấy giờ PC qua UDP 123 trước TLS. NTP bootstrap chưa có xác thực riêng;
không tắt kiểm tra TLS để vượt lỗi giờ, hostname hoặc CA. Quy trình dừng,
provision, build/nạp và bật server nằm trong [hướng dẫn local](../../../../docs/CHAY_LOCAL_A_Z.md).

Tham khảo [ESP-MQTT: xác minh broker](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/protocols/mqtt.html#verification).
