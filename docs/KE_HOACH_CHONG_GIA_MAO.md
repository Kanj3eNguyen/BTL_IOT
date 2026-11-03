# Chống sửa và phát lại dữ liệu MQTT

Phạm vi đã chốt: **chỉ xác thực dữ liệu MQTT từ ESP32-S3**. Không làm
phát hiện ảnh in/video giả, nhịp tim hoặc SpO₂. Mắt/miệng, blink, yawn và
PERCLOS tiếp tục được phân tích trên bo. Xem [hướng dẫn local A–Z](CHAY_LOCAL_A_Z.md).

## 1. Luồng dữ liệu

```text
OV3660 → ESP-WHO → phân tích hành vi trên ESP32-S3
 → JSON + HMAC-SHA256 → MQTT qua TLS tới PC:8885
 → xác minh khóa/phiên/sequence/thời gian → SQLite outbox
 → ThingsBoard local, MQTT 127.0.0.1:18884
 → quản trị tại http://127.0.0.1:8080
```

PC chạy cả ThingsBoard, PostgreSQL và gateway xác minh. Chỉ gateway giữ
access token của device đích. Trong chế độ guard, firmware không sử dụng
token ThingsBoard để gửi thẳng. Cổng MQTT của ThingsBoard chỉ mở ở localhost;
bo trên LAN phải qua gateway. Tệp nguồn, khóa, database, môi trường Python,
log và filesystem WSL của dự án nằm trên D; công cụ có sẵn được dùng lại.

## 2. Vai trò từng lớp bảo vệ

| Lớp | Mục đích |
| --- | --- |
| TLS | Mã hóa LAN; bo kiểm tra CA, IP/hostname và thời hạn chứng chỉ |
| Mật khẩu MQTT và ACL | Mỗi bo chỉ gửi hello/telemetry và nhận downlink của chính mình |
| HMAC-SHA256 | Phát hiện nội dung bị sửa hoặc được tạo bởi người không có khóa |
| Phiên server cấp | Từ chối gói thuộc phiên đã bị thay thế hoặc hết hạn |
| Sequence và SQLite | Không chuyển tiếp gói trùng; giữ mốc chống replay sau restart gateway |
| Thời gian và tuổi phép đo | Loại gói cũ và kết quả đo cũ được bọc trong thông điệp mới |
| Token đích riêng ở gateway | Tránh đường gửi thẳng từ firmware vào device đã xác minh |

Checksum UART không thay được HMAC: người sửa gói có thể tự tính lại
checksum thông thường. HMAC yêu cầu khóa bí mật riêng của thiết bị.

## 3. Định dạng ký DM-MQTT-1

```json
{"v":1,"payload":"BASE64_CUA_JSON_GOC","mac":"HMAC_SHA256_HEX_64_KY_TU"}
```

```text
mac = HMAC-SHA256(khóa riêng, UTF8("DM-MQTT-1\n") || byte_JSON_gốc)
```

JSON bên trong gồm phiên bản, loại thông điệp, device ID, key ID, boot ID,
thời gian gửi và các trường tùy loại. Toàn bộ JSON được ký. Máy chủ xác
minh đúng byte gốc sau khi giải mã base64, không serialize JSON lại trước
khi tính HMAC. Parser từ chối khóa JSON trùng, NaN/Infinity, phong bì sai
và thông điệp quá giới hạn. HMAC được so sánh bằng phép so sánh thời gian hằng.

## 4. Bắt tay và sequence

1. Bo kết nối Wi-Fi, lấy giờ PC rồi mở MQTT có TLS.
2. Bo subscribe `dm/v1/<device_id>/down`, gửi hello đã ký với boot ID và
   request ID ngẫu nhiên 128 bit. RNG được gọi khi Wi-Fi/RF đang hoạt động.
3. Server xác thực hello, cấp session ID ngẫu nhiên có thời hạn 5 phút.
   Câu trả lời cũng có HMAC; bo kiểm tra request/boot/device trước khi dùng.
4. Mỗi telemetry chứa session ID, sequence, timestamp, tuổi phép đo và
   `data` chứa số liệu hành vi hiện có.
5. Server xác minh và ghi sequence cùng bản dữ liệu chờ gửi trong một
   giao dịch SQLite. Gói bị loại không được đưa vào outbox.
6. Gateway gửi ThingsBoard với timestamp đo gốc. Sau PUBACK của ThingsBoard,
   server gửi ACK đã ký cho bo; bo tăng bộ đếm đã chuyển và nháy LED xanh.

Bo xin phiên mới khi reconnect hoặc còn dưới 15 giây trước hết hạn.
Gửi lại cùng hello không reset sequence. Hello cũ không khôi phục được
phiên đã bị thay thế. SQLite lưu mốc chống replay bền qua restart gateway.

| Tình huống | Quyết định |
| --- | --- |
| Cùng phiên/sequence/nội dung đã nhận | DUPLICATE, không chuyển tiếp lần hai |
| Sequence cũ hoặc nội dung khác với số đã dùng | REPLAY_OR_SEQUENCE_CONFLICT |
| Gói thuộc phiên cũ sau reset/reconnect | SESSION_INVALID |
| Phiên hết hạn | SESSION_EXPIRED |
| Nội dung bị sửa/khóa sai | BAD_MAC |
| Timestamp cũ quá 30 giây hoặc vượt tương lai quá 5 giây | STALE_OR_FUTURE |
| Tuổi kết quả lúc đóng gói quá 15 giây | Từ chối kết quả cũ |
| Tuổi phép đo lúc server nhận quá 30 giây | Từ chối phép đo cũ |
| Sequence nhảy do mất mạng nhưng vẫn tăng | Cho phép nếu các kiểm tra khác đạt |

Không suy đoán ý định từ gói giống hệt: retry QoS 1 và cố tình replay cùng
byte đều bị bỏ qua như một bản trùng. Mất mạng/sequence nhảy không tự có
nghĩa là bị tấn công. Outbox hết hạn dữ liệu cũ khi ThingsBoard ngừng lâu,
không phát lại như dữ liệu mới sau khi khôi phục.

Outbox không cam kết exactly-once ở mọi điểm sập máy: nếu ThingsBoard đã
PUBACK nhưng PC dừng trước khi lưu trạng thái đã gửi, gateway có thể gửi lại
cùng mẫu khi khởi động lại. Mẫu vẫn mang timestamp gốc và bộ đếm tuyệt đối;
không dùng mỗi lần nhận MQTT để cộng thêm một blink/yawn. Gói trùng nhận
từ bo không tạo thêm một hàng outbox mới.

## 5. File nào làm gì?

Các file `main/` và viewer dưới `examples/ov3660_face_detect/`.

| File | Ý nghĩa |
| --- | --- |
| main/driver_message_auth.cpp/.hpp | Khóa, nonce, HMAC, ký hello/sample và kiểm tra grant/ACK |
| main/driver_telemetry.cpp/.hpp | Wi-Fi/MQTT, subscribe downlink, nhịp gửi và chọn chế độ guard |
| main/Kconfig.projbuild | Các tham số server/device/key; giá trị riêng trong sdkconfig ignored |
| main/CMakeLists.txt | Biên dịch module xác thực và nhúng đúng CA local |
| main/serial_jpeg_stream.cpp | Xuất trạng thái guard qua COM để kiểm tra trực quan |
| driver_monitor_viewer.py | Hiển thị phiên READY/WAIT và sequence ThingsBoard đã ACK |
| server/mqtt_guard/protocol.py | Định dạng byte ký, parser chặt và xác minh HMAC |
| server/mqtt_guard/verifier.py | Phiên, độ mới, sequence, schema và SQLite outbox |
| server/mqtt_guard/gateway.py | Broker TLS, auth/ACL, gọi verifier, chuyển ThingsBoard và ký ACK |
| server/mqtt_guard/ntp.py | Giờ bootstrap từ PC qua UDP123 để chạy local không cần public NTP |
| server/mqtt_guard/provision.py | Sinh khóa/mật khẩu/CA, cấu hình bo và backup sdkconfig |
| server/thingsboard_local/compose.yml | PostgreSQL và ThingsBoard; chỉ mở web/MQTT ở localhost |
| server/thingsboard_local/setup.py | Đổi mật khẩu demo, tạo device và cấp token chỉ gateway giữ |
| tools/Setup-Local.ps1 | Chuẩn bị server khi dùng lần đầu, giữ dữ liệu trên D |
| tools/Start-Local.ps1 | Bật engine, ThingsBoard và gateway, không xóa dữ liệu |
| tools/Stop-Local.ps1 | Dừng dịch vụ và giữ nguyên volume/khóa |
| tools/Build-Flash.ps1 | Kích hoạt ESP-IDF, build một luồng trong build_guard và nạp bo |
| tests/security/test_mqtt_guard.py | Kiểm tra sửa/replay/duplicate/reset/restart/freshness/schema |
| tests/security/run_mqtt_integration.py | Thử TLS/MQTT thật trên device và cổng test riêng; tùy chọn --thingsboard |

## 6. Dữ liệu quản trị

Gateway bổ sung `integrity_status=VERIFIED`, `integrity_reason=OK`,
`auth_device_id`, `auth_key_id`, `auth_session_id`, `auth_sequence`,
`last_verified_at` và `sample_age_ms`. Bo không được tự đặt các trường
xác minh này trong `data`. Những trường blink/yawn/PERCLOS và cờ valid
được giữ như trước; mất quan sát không bị chuyển thành mắt mở/PERCLOS 0.

Các counter do gateway tạo gồm `guard_rejected_count`, `guard_bad_mac_count`,
`guard_duplicate_count` và `guard_replay_rejected_count`. Chúng cập nhật trên
ThingsBoard cùng mẫu hợp lệ kế tiếp; khi bo không gửi mẫu mới thì xem counter
SQLite/log local. Một gói bị loại không tự chứng minh có người tấn công.

`mqtt_acked` là PUBACK của broker đầu vào. `guard_forwarded_sequence` và
`guard_forwarded_count` chỉ cập nhật sau ACK có HMAC từ gateway khi
ThingsBoard đã PUBACK. PUBACK không đồng nghĩa đã commit database; bài
thử REST kiểm tra việc lưu riêng vì ThingsBoard xử lý bất đồng bộ.

Gói bị loại được đếm ở SQLite và ghi lý do vào log local. Dashboard phải
xét timestamp/mất kết nối, không coi VERIFIED cũ là thiết bị vẫn online.

Audit bền qua restart ở `.private/mqtt_guard/state.audit.log`, tự xoay tối
đa bốn file 5 MB. Log khởi động gateway ở `.private/runtime/gateway-error.log`.

## 7. Giới hạn

HMAC không chứng minh người trong ảnh là thật, không sửa sai số model
và không ngăn người đã lấy được khóa/chiếm quyền firmware ký dữ liệu giả.
Người có token đích hoặc quyền quản trị ThingsBoard có thể bỏ qua gateway.
Khóa phần mềm hiện dành cho bản thử nghiệm; chưa bật Secure Boot, mã hóa
flash hoặc đốt eFuse. Máy chủ PC cần bật, cùng mạng với bo và có giờ đúng.

Không đưa `.private`, sdkconfig, build hay firmware chứa khóa lên Git.
Không tắt kiểm tra TLS để khắc phục lỗi giờ/chứng chỉ.

Tham khảo: [Espressif HMAC](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/hmac.html),
[ThingsBoard MQTT](https://thingsboard.io/docs/reference/mqtt-api/telemetry/),
[cài ThingsBoard Docker](https://thingsboard.io/docs/installation/docker/).
