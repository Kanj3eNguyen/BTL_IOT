# CA dùng cho ThingsBoard Cloud MQTTS

**Chế độ local:** khi bật `DRIVER_MQTT_GUARD_ENABLED`, CMake nhúng
`ingest-root-ca.pem` do `server.mqtt_guard.provision` sinh thay cho bundle
Cloud bên dưới. CA local và khóa server được giữ trong `.private`; bản
CA chép vào thư mục này cũng được ignore để clone mới tự cấp chứng chỉ.
Không chép private key vào firmware. Đổi IP server phải cập nhật chứng
chỉ/URI và nạp lại; xem [hướng dẫn local](../../../../docs/CHAY_LOCAL_A_Z.md).

`tb-cloud-root-ca.pem` là bundle **7 chứng chỉ CA công khai**, tải ngày
2026-11-03 từ endpoint HTTPS ThingsBoard do dự án sử dụng:

[Tải CA từ ThingsBoard Cloud](https://thingsboard.cloud/api/device-connectivity/mqtts/certificate/download).

SHA-256 của bản kèm repo:

```text
69409af3104c7217c7e48376abcf15b998f92ccc707c8ca4467914bab0a9f291
```

CA là điểm tin cậy để bo xác minh chứng chỉ máy chủ. Đây không phải token,
không phải private key và không phải chứng chỉ định danh client. Có thể
đưa file này vào Git cùng firmware nguồn. Thiết bị vẫn xác thực bằng access
token của device qua kết nối TLS đã kiểm tra máy chủ.

`main/CMakeLists.txt` dùng `EMBED_TXTFILES` để nhúng PEM có kết thúc NUL vào
firmware. `driver_telemetry.cpp` gắn nó vào
`config.broker.verification.certificate`; ESP-MQTT kiểm tra CA và hostname
trong URI. Cấu hình bắt buộc bật kiểm tra thời hạn trong mbedTLS; task SNTP
đồng bộ giờ trước khi bắt đầu kết nối MQTT đầu tiên.

Để tải lại, đứng tại `examples/ov3660_face_detect` và chạy:

```powershell
curl.exe -f -S -L -o .\main\certs\tb-cloud-root-ca.pem https://thingsboard.cloud/api/device-connectivity/mqtts/certificate/download
```

Kiểm tra nội dung tải về là PEM hợp lệ rồi build và flash lại. Khi đổi sang
broker riêng, thay bằng CA tin cậy phù hợp và đổi URI tương ứng. CA tải ở
thời điểm khác có thể đổi nội dung/hash khi ThingsBoard cập nhật chuỗi tin cậy.

Tham khảo [ThingsBoard Cloud: kết nối MQTT/TLS](https://thingsboard.io/docs/paas/reference/mqtt-api/getting-connected/)
và [ESP-IDF 5.5: xác minh broker trong ESP-MQTT](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/protocols/mqtt.html#verification).
