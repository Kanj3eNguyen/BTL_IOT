# Giám sát tài xế với ESP32-S3-CAM OV3660

**Bản local có xác thực MQTT:** xem [hướng dẫn từ A đến Z](docs/CHAY_LOCAL_A_Z.md)
để chạy PC làm server ThingsBoard, xác minh HMAC và chống phát lại. Mọi dữ
liệu server/khóa/build mới đặt trên D; dùng lại công cụ đã có. Hướng dẫn
MQTTS Cloud bên dưới dành cho chế độ gửi trực tiếp cũ. Chế độ guard gửi
qua cổng xác minh local trước khi vào ThingsBoard.

Repo này dành cho bo **ESP32-S3-WROOM-1 N16R8** (flash 16 MB, PSRAM 8 MB),
camera **OV3660** dùng sơ đồ chân **ESP32-S3-EYE**, và module buzzer chủ động
ở **GPIO14**. Firmware chạy nhận diện mặt, mắt và miệng **ngay trên bo**.
Máy tính chỉ nhận ảnh cùng kết quả qua cổng USB serial để hiển thị. Bo có thể
gửi số liệu lên ThingsBoard Cloud qua Wi-Fi 2,4 GHz và **MQTTS (MQTT qua TLS)**.

Hướng dẫn dưới đây dành cho **Windows 10/11, PowerShell**. Dự án đã được build
với **ESP-IDF v5.5.5**. Người dùng mới cần Internet ở lần cài đặt và build đầu.
Không cần máy tính để bo chạy sau khi đã nạp firmware: cắm nguồn USB và có
Wi-Fi đúng tên/mật khẩu là bo tự hoạt động. Repo **không chứa firmware `.bin`
đã nạp token cá nhân**; mỗi người build và cấu hình device của mình theo
các bước dưới đây.

**Muốn hiểu hệ thống hoạt động ra sao?** Đọc
[Hệ thống hoạt động như thế nào?](docs/HE_THONG_HOAT_DONG.md) để xem sơ đồ
luồng dữ liệu, cách bo nhận diện mắt/miệng, đếm blink và ngáp, tính PERCLOS,
phát cảnh báo và gửi kết quả qua COM/MQTT. Có thể đọc sau bước 6 khi đã xem
được stream để đối chiếu các trạng thái trên màn hình.

Để hiểu sâu mã nguồn, đọc [Giải thích từng file](docs/GIAI_THICH_TUNG_FILE.md):
file chạy trên bo hay PC, ai gọi nó, nó nhận/xử lý/trả dữ liệu gì, công thức
model, cách giữ bộ đếm và ý nghĩa từng lệnh. Có cả hướng dẫn tra lỗi theo
file và ví dụ lần một blink/ngáp/bản tin MQTT qua các hàm liên quan.

Phần bảo vệ dữ liệu: [Cơ chế xác thực và chống phát lại MQTT](docs/KE_HOACH_CHONG_GIA_MAO.md).
Không triển khai model phát hiện ảnh in/video giả hoặc nhịp tim/SpO₂.

## 1. Chuẩn bị phần cứng

1. Bo ESP32-S3-CAM N16R8 gắn camera OV3660, **đúng sơ đồ chân ESP32-S3-EYE**.
   Bo N16R8 của hãng khác có thể dùng chân camera khác và sẽ cần sửa firmware.
2. Cáp USB **có truyền dữ liệu**. Cáp chỉ sạc có thể cấp nguồn nhưng không tạo
   cổng COM để nạp hoặc xem stream.
3. Nếu dùng buzzer: nối **chân tín hiệu vào GPIO14**, GND vào GND của bo;
   cấp nguồn cho module theo thông số của nó. Firmware mặc định dùng buzzer
   chủ động, kích hoạt ở mức **LOW**. **Không nối buzzer vào GPIO4** vì chân đó
   là SDA của camera.
4. Điểm phát Wi-Fi **2,4 GHz**. Nếu dùng hotspot điện thoại, chọn băng tần
   2,4 GHz trước khi chạy bo.

## 2. Tải mã nguồn và cài phần mềm trên PC

### 2.1. Lấy repo

Cài [Git for Windows](https://git-scm.com/download/win) nếu máy chưa có Git.
Trên trang GitHub của repo, chọn **Code → HTTPS**, sao chép URL rồi chạy:

```powershell
cd D:\
git clone 'https://github.com/Kanj3eNguyen/BTL_IOT.git' ESP32S3_Driver_Monitor_Git
cd .\ESP32S3_Driver_Monitor_Git
```

URL trên là repo của dự án. Có thể chọn **Code → Download ZIP**, giải
nén vào một đường dẫn ngắn, không dấu và không khoảng trắng, rồi mở PowerShell
tại thư mục vừa giải nén. Các lệnh sau giả sử bạn đang ở **gốc repo**.

### 2.2. Cài ESP-IDF v5.5.5

Làm theo [hướng dẫn cài ESP-IDF trên Windows cho ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/windows-setup.html).
Chọn đúng phiên bản **v5.5.5** và cài cả công cụ biên dịch. Đường dẫn ESP-IDF
nên ngắn và không có dấu, ví dụ `D:\esp\v5.5.5\esp-idf`. Bộ cài của Espressif
có thể cài Python, Git và các công cụ build cần cho ESP-IDF. Xem thêm
[trang phát hành v5.5.5](https://github.com/espressif/esp-idf/releases/tag/v5.5.5).

Để xem stream bằng cửa sổ Python, cài riêng **Python 3.11 64-bit** nếu chưa có.
Bạn có thể lấy từ [Python 3.11.9 trên python.org](https://www.python.org/downloads/release/python-3119/).
Khi cài, bật **Add Python to PATH**. Kiểm tra:

```powershell
py -3.11 --version
```

### 2.3. Cài thư viện viewer

Đứng tại **gốc repo**, tạo môi trường Python riêng và cài thư viện:

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r .\requirements-viewer.txt
```

Chỉ cần chạy ba lệnh này một lần cho mỗi máy. `.venv` nằm trong `.gitignore`.

## 3. Cắm bo và tìm cổng COM

1. Cắm cáp USB dữ liệu từ bo vào PC. Đợi Windows nhận thiết bị.
2. Mở **Device Manager → Ports (COM & LPT)**. Ghi lại cổng hiện ra, ví dụ
   `COM4` hoặc `COM7`. Có thể kiểm tra trong PowerShell:

   ```powershell
   [System.IO.Ports.SerialPort]::GetPortNames()
   ```

3. Nếu không có cổng COM, thử cáp/cổng USB khác. Bo đã thử dùng chip
   **CH343**; nếu Windows chưa nhận chip này, cài driver USB serial từ nhà
   sản xuất bo hoặc [trang driver CH343 của WCH](https://www.wch-ic.com/downloads/CH343SER_ZIP.html)
   rồi cắm lại.

Trong mọi lệnh sau, **thay `COM4` bằng cổng của máy bạn**. Cổng COM chỉ dùng
để nạp và xem stream; không cần đổi mã nguồn khi chuyển sang máy khác.

## 4. Cấu hình firmware lần đầu

Mở **PowerShell mới** tại gốc repo. Đặt `$IdfPath` thành thư mục ESP-IDF bạn
đã cài, rồi chạy **trong cùng cửa sổ PowerShell**:

```powershell
$IdfPath = 'D:\esp\v5.5.5\esp-idf'
. (Join-Path $IdfPath 'export.ps1')
$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path '.\tools').Path
$env:IDF_PY_BUILD_JOBS = '2'
cd .\examples\ov3660_face_detect
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.bsp.esp32_s3_eye_noglib" -DDETECT_MODEL=human_face_detect set-target esp32s3
idf.py menuconfig
```

Lệnh `export.ps1` phải báo **Done! You can now compile ESP-IDF projects**.
Trong `menuconfig`, vào **Driver monitor MQTT** và điền:

| Mục | Cần điền |
| --- | --- |
| Wi-Fi SSID | Tên mạng Wi-Fi **2,4 GHz** mà bo sẽ kết nối |
| Wi-Fi password | Mật khẩu mạng trên |
| ThingsBoard MQTT URI | `mqtts://mqtt.thingsboard.cloud:8883` nếu dùng ThingsBoard Cloud |
| SNTP server for TLS clock synchronization | Giữ `pool.ntp.org`, hoặc máy chủ NTP mà mạng của bạn truy cập được |
| ThingsBoard device access token | Token của **device của bạn** trên ThingsBoard |
| Telemetry publish interval | Có thể để mặc định 5000 ms |

Nếu chỉ cần xem stream và chưa dùng MQTT, có thể để trống SSID và token;
viewer vẫn nhận ảnh qua USB, nhưng Wi-Fi/MQTT sẽ hiện offline. Lưu cấu hình
và thoát `menuconfig`. Tệp `sdkconfig` vừa tạo chứa mật khẩu/token **dạng chữ**;
repo đã bỏ qua tệp này bằng `.gitignore`. Không đưa `sdkconfig`, ảnh khuôn mặt,
video, hoặc firmware đã build cùng token thật lên GitHub.

### 4.1. MQTTS, chứng chỉ CA và nâng cấp từ bản MQTT thường

Firmware đã kèm [CA công khai của ThingsBoard](examples/ov3660_face_detect/main/certs/README.md)
trong `main/certs/tb-cloud-root-ca.pem`. CMake nhúng file này vào firmware;
bo dùng nó để kiểm tra chuỗi chứng chỉ và hostname của broker. Token device
vẫn là username MQTT; không cần tạo private key hay chứng chỉ client.

Nếu đã có `sdkconfig` từ bản cũ, chạy `idf.py menuconfig` → **Driver monitor
MQTT** và đổi URI từ cổng 1883 sang `mqtts://mqtt.thingsboard.cloud:8883`.
Giá trị cũ đã lưu không tự đổi theo default mới. Giữ SSID/mật khẩu/token
của bạn, lưu rồi **build và flash** theo mục 5. Firmware mới từ chối URI
`mqtt://` để tránh vô tình gửi token qua kết nối thường.

Sau khi Wi-Fi nhận IP, task mạng đồng bộ giờ SNTP trước lần kết nối TLS
đầu tiên để kiểm tra thời hạn chứng chỉ. Camera, nhận diện và stream COM
vẫn chạy khi đang chờ. Viewer hiện `WiFi: CONNECTED  Time: WAIT` rồi
`Time: OK`; khi broker kết nối được sẽ hiện `MQTTS: CONNECTED`, ACK tăng
và LED nháy xanh. Nếu `Time: WAIT` kéo dài, kiểm tra Internet/DNS và NTP
UDP 123; có thể đổi SNTP server trong menu rồi build/flash lại.

Nếu cần cập nhật CA, chạy ở `examples/ov3660_face_detect`:

```powershell
curl.exe -f -S -L -o .\main\certs\tb-cloud-root-ca.pem https://thingsboard.cloud/api/device-connectivity/mqtts/certificate/download
```

Sau khi thay CA phải build/flash lại; bo không tự tải CA khi khởi động.
Với server riêng, URI và CA phải thuộc server đó. Không tắt kiểm tra CA,
hostname hoặc thời hạn chứng chỉ để xử lý lỗi kết nối.

Nếu đã có `sdkconfig` riêng và muốn giữ Wi-Fi/token, **không chạy lại
`set-target`** chỉ để đổi sang TLS. Nếu chưa có cache build trong bản
nguồn mới, mở PowerShell tại gốc repo và dùng các tham số đầy đủ sau:

```powershell
$IdfPath = 'D:\esp\v5.5.5\esp-idf'
. (Join-Path $IdfPath 'export.ps1')
$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path '.\tools').Path
$env:IDF_PY_BUILD_JOBS = '2'
cd .\examples\ov3660_face_detect
idf.py '-DIDF_TARGET=esp32s3' '-DSDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_s3_eye_noglib' '-DDETECT_MODEL=human_face_detect' build
idf.py -p COM4 flash
```

Thay đường dẫn ESP-IDF và cổng COM nếu máy bạn khác. Các tham số được đặt
trong dấu nháy để PowerShell chuyển nguyên vẹn cho `idf.py`. Lệnh này dùng
`sdkconfig` đã lưu; nó không xóa Wi-Fi/token như thao tác đổi target có thể làm.

> **Chú ý:** `set-target` dùng cho lần đầu hoặc khi đổi target. Chạy lại có
> thể tạo lại `sdkconfig`, cần nhập lại Wi-Fi/token. Lần build bình thường
> không chạy lại `set-target`.

### 4.2. Chế độ local có gateway xác minh

Nếu cần kiểm tra HMAC và chống phát lại trước khi ghi telemetry vào
ThingsBoard, làm theo toàn bộ [hướng dẫn local A–Z](docs/CHAY_LOCAL_A_Z.md).
Chế độ này không dùng access token ThingsBoard trên firmware: bo kết nối
MQTTS tới gateway trên PC, còn gateway giữ token đích và chuyển tiếp mẫu đã
được xác minh tới ThingsBoard local. Mỗi máy/bo phải tự sinh khóa, mật khẩu,
CA và chứng chỉ trong `.private`; không sao chép các tệp này sang máy khác.

Không trộn `sdkconfig` Cloud với `sdkconfig` local. Sau khi chạy
`Setup-Local.ps1`, dùng `Build-Flash.ps1` để build vào `build_guard` và nạp
đúng cấu hình local. Khi quay lại Cloud, chọn URI
`mqtts://mqtt.thingsboard.cloud:8883` trong `idf.py menuconfig`, rồi
build/flash lại. Không đưa `sdkconfig`, `.private`, firmware hoặc khóa lên Git.

## 5. Build và nạp lên bo

Vẫn trong **cửa sổ PowerShell ở mục 4**, và đang đứng tại
`examples\ov3660_face_detect`, chạy:

```powershell
idf.py build
idf.py -p COM4 flash
```

Lần build đầu tải các component từ Espressif Component Registry và có thể mất
vài phút. Khi nạp xong có dòng **Hash of data verified** và bo tự khởi động
lại. Nếu COM đang bận, đóng viewer, Serial Monitor hoặc chương trình khác
trước khi flash. **Không mở `idf.py monitor` cùng lúc với viewer**: cổng này
truyền gói JPEG nhị phân.

Các lần build sau, mở PowerShell mới rồi chạy lại phần **kích hoạt ESP-IDF và
đặt `IDF_EXTRA_ACTIONS_PATH`** ở mục 4, `cd` vào dự án, sau đó dùng hai lệnh
`idf.py build` và `idf.py -p COM4 flash`. Không cần cài lại thư viện viewer.

## 6. Xem stream và kiểm tra kết quả

Sau khi nạp xong và bo khởi động, mở **PowerShell khác** tại gốc repo:

```powershell
.\.venv\Scripts\python.exe .\examples\ov3660_face_detect\driver_monitor_viewer.py --port COM4
```

Cửa sổ hiện video camera, khung mặt, FPS, trạng thái mắt/miệng, `blink_count`,
`yawn_count`, PERCLOS, Wi-Fi, MQTT và số ACK. **Ảnh và việc nhận diện xuất phát
từ bo**, PC chỉ giải mã và hiển thị. Khi đầu quay quá nghiêng, kết quả mắt
thiếu tin cậy sẽ hiện `Eyes: UNKNOWN` để tránh đếm blink giả.

Kiểm tra ThingsBoard: mở **ThingsBoard Cloud → Devices → device dùng token ở
mục 4 → Latest telemetry**. Khi viewer hiện `MQTT: CONNECTED` và `ACK` tăng,
bo đã được broker xác nhận bản tin QoS 1. Các trường chính gồm `blink_count`,
`yawn_count`, `perclos`, `alert_reasons`, `sequence` và `boot_id`. Ảnh camera
không được gửi lên ThingsBoard.

Để đóng viewer, chọn cửa sổ video và nhấn **q**. Bo vẫn chạy và gửi MQTT khi
viewer đóng, miễn là còn nguồn và Wi-Fi. Để tắt hẳn bo, ngắt nguồn USB.

## 7. Chạy hằng ngày hoặc đổi sang máy khác

Viewer hiện riêng **Video FPS** (preview nhận qua COM) và **AI FPS**
(tốc độ phân tích trên bo). Nén/gửi JPEG có task riêng, chỉ giữ ảnh mới
nhất đang chờ; `Skip` là số preview bị thay bằng ảnh mới, còn `UARTbad` là
gói truyền lỗi. `Age` chưa bao gồm thời gian truyền UART của frame đó.

Camera tự bù mục tiêu phơi sáng dựa trên độ sáng mặt, hoặc toàn cảnh khi
không có mặt. `Light: ADJUST/OK/DARK/BRIGHT` và `AE` cho biết trạng thái;
đổi môi trường có thể cần vài giây để ổn định. Cơ chế này hỗ trợ ảnh đầu
vào, không bảo đảm model đúng ở mọi ánh sáng/người. Không có tính năng tự
train model trong lúc sử dụng. Xem mục 2.10 của
[Giải thích từng file](docs/GIAI_THICH_TUNG_FILE.md) để hiểu quy tắc bù sáng.

Luồng preview mới dùng **1.500.000 baud** trên bo và viewer, phù hợp cổng
CH343 đang dùng. Cần dùng viewer đi kèm bản firmware này; một viewer cũ
giữ 921.600 baud sẽ không đọc đúng luồng mới. Lệnh mặc định không cần thêm
`--baud` vì script đã chọn đúng tốc độ.

Preset N16R8 dùng PSRAM cho cấp phát mbedTLS để chừa RAM nội cho Wi-Fi,
UART và codec. Với cấu hình cũ đã lưu, vào **Component config → mbedTLS →
Memory allocation strategy → External SPIRAM** rồi build/flash để áp dụng.
Wi-Fi/token vẫn ở cấu hình cục bộ và không được đưa vào Git.

- **Bo đã nạp firmware:** cắm nguồn USB, chờ khởi động. Không phải build hay
  flash lại. Để xem stream trên PC khác, cài Python và thư viện viewer theo
  mục 2.3, tìm cổng COM mới theo mục 3 và thay `COM4` trong lệnh mục 6.
- **Muốn đổi Wi-Fi hoặc ThingsBoard:** trên PC có ESP-IDF, kích hoạt môi trường
  như mục 4, vào thư mục dự án và chạy `idf.py menuconfig`. Sửa các mục trong
  **Driver monitor MQTT**, lưu, rồi `idf.py build` và `idf.py -p COM4 flash`.
  Chỉ nhấn RST là đủ nếu tên/mật khẩu Wi-Fi **không đổi**.
- **Muốn ghi lại stream để xem sau:** thêm `--out` và `--seconds`, ví dụ:

  ```powershell
  .\.venv\Scripts\python.exe .\examples\ov3660_face_detect\driver_monitor_viewer.py --port COM4 --out .\examples\ov3660_face_detect\recordings\test_01 --seconds 60
  ```

  Thư mục ghi chứa ảnh mặt; `.gitignore` loại `recordings` khỏi Git. Không
  mở thêm viewer khác cùng cổng COM.

## 8. Ghi video và huấn luyện lại mắt/miệng

Phần **huấn luyện chạy trên PC**. Script tạo các mảng trọng số C++ nhỏ; sau
khi build/flash, **suy luận vẫn chạy trên ESP32-S3**. Repo giữ sẵn kết quả
đã học trong `examples/ov3660_face_detect/main/driver_ml_weights.hpp`: các
mảng trọng số và tham số chuẩn hóa mà firmware dùng để phân loại mắt/miệng.
Người tải repo có thể build và chạy bằng model này ngay, không cần train lại.
Ảnh, video và nhãn huấn luyện cũ đã được bỏ khỏi bản chia sẻ.

Nếu muốn cải thiện model cho người hoặc ánh sáng khác, làm theo các bước
dưới đây để quay và gắn nhãn dữ liệu của mình. Có thể dùng một lượt video
mới cho cả mắt và miệng. Những phiên mới trong `recordings/` bị Git bỏ qua;
`training_data/` cũng được bỏ qua nếu bạn tự tạo lại thư mục này.

### 8.1. Ghi một lượt video có đủ trạng thái

Đóng viewer (nhấn **q**) để nhường COM. Tại **gốc repo**, chạy:

```powershell
.\.venv\Scripts\python.exe .\examples\ov3660_face_detect\record_training_video.py --port COM4 --max-seconds 180
```

Cửa sổ preview hiện từ camera **trên bo**. Nhấn **r** để bắt đầu hoặc tạm dừng,
**q** để lưu và kết thúc. Script in ra thư mục phiên, ví dụ
`examples\ov3660_face_detect\recordings\YYYYMMDD_HHMMSS`. Trong video, ghi
nhiều đoạn rõ ràng: mắt mở/miệng đóng, mắt nhắm/miệng đóng, mắt mở/miệng mở.
Lặp lại cả ba trạng thái ở **nửa đầu và nửa sau** của video, trong hai phòng
và ở góc quay nhẹ nếu đó là điều kiện sử dụng. Dành vài giây cho mỗi đoạn,
nhìn thấy mặt và hai mắt; bỏ qua lúc đổi tư thế. Đầu quay quá nghiêng khiến
mắt bị che sẽ không dùng để huấn luyện mắt vì firmware cũng bỏ các khung đó.

Thư mục phiên có `training_video.avi` để xem, `frames/` là ảnh gốc cho train,
`frames.jsonl` chứa thời gian `recorded_s` và tọa độ mặt. Số giây được in
ngay trên ảnh AVI; dùng **số đó** để gắn nhãn, vì FPS của video có thể thay đổi.
Các dự đoán mắt/miệng của bo trong `frames.jsonl` **không phải nhãn đúng**.

### 8.2. Gắn nhãn một lần sau khi quay

Xem toàn bộ `training_video.avi`, xác định các đoạn **chắc chắn** mắt mở,
mắt nhắm, miệng đóng, miệng mở. Sao chép mẫu và mở bằng Notepad:

```powershell
$session = '.\examples\ov3660_face_detect\recordings\YYYYMMDD_HHMMSS'
Copy-Item .\examples\ov3660_face_detect\labels.example.json (Join-Path $session 'labels.json')
notepad (Join-Path $session 'labels.json')
```

Thay `YYYYMMDD_HHMMSS` bằng tên phiên vừa ghi; **sửa toàn bộ các mốc giây
ví dụ** theo video thật. Mỗi cặp `[bắt_đầu, kết_thúc]` bao gồm đầu đoạn và
không bao gồm cuối đoạn. Chỉ đánh dấu phần ổn định, cách thời điểm đổi tư
thế một chút. `eye_open_ranges` và `eye_closed_ranges` không được chồng nhau;
`mouth_open_ranges` và `mouth_closed_ranges` cũng vậy. Mắt mở và miệng mở
cùng lúc thì hai nhãn đó được chồng nhau vì là hai phép đo khác nhau.

`validation_from_second` chia video thành phần train ở trước và phần kiểm
tra ở sau. Cần có **cả hai trạng thái của mắt và miệng ở mỗi phía**. Nên dùng
đoạn nửa sau ghi riêng trong điều kiện tương tự để kiểm tra khả năng dùng
trên khung hình chưa được train. Nếu mặt mất nét, bị che, hoặc không rõ mở/
đóng, cứ để trống đoạn đó; không gắn nhãn theo số mà bo đang dự đoán.

### 8.3. Train và xem kết quả

Tại gốc repo, cài thêm thư viện train (một lần):

```powershell
.\.venv\Scripts\python.exe -m pip install -r .\requirements-train.txt
```

Sau đó chạy, dùng đúng `$session` vừa đặt:

```powershell
.\.venv\Scripts\python.exe .\examples\ov3660_face_detect\train_driver_model.py --recording $session
```

Script chỉ lấy ảnh và nhãn trong phiên `$session` bạn đã ghi;
không cần dữ liệu huấn luyện cũ.
Nó in số mẫu của từng lớp, `balanced accuracy` và ma trận
`[[TN, FP], [FN, TP]]` riêng cho mắt và miệng. `FP` cao nghĩa là dễ báo
nhắm mắt/há miệng sai; `FN` cao nghĩa là hay bỏ sót. Nếu thiếu một trạng thái
hoặc quá ít ảnh hợp lệ, script dừng và yêu cầu ghi/gắn nhãn thêm. Kết quả tốt
trên một video ngắn vẫn chưa chứng minh độ chính xác ở mọi phòng, góc quay
và người dùng; hãy thử stream thật trước khi dùng.

Kết quả train mặc định là
`examples\ov3660_face_detect\main\driver_ml_weights.candidate.hpp`.
Firmware **chưa dùng file ứng viên** này. Khi đã xem kết quả và muốn thử
trên bo, sao lưu trọng số cũ rồi thay bằng ứng viên:

```powershell
$modelDir = '.\examples\ov3660_face_detect\main'
Copy-Item (Join-Path $modelDir 'driver_ml_weights.hpp') (Join-Path $modelDir 'driver_ml_weights.backup.hpp')
Copy-Item (Join-Path $modelDir 'driver_ml_weights.candidate.hpp') (Join-Path $modelDir 'driver_ml_weights.hpp')
```

Kích hoạt ESP-IDF theo mục 4, vào thư mục firmware rồi chạy `idf.py build`
và `idf.py -p COM4 flash` như mục 5. Đóng viewer trước khi flash. Sau đó
thử stream ở cả hai phòng, mắt mở/nhắm và miệng đóng/mở. Nếu kém hơn, chép
`driver_ml_weights.backup.hpp` trở lại `driver_ml_weights.hpp`, build và flash
lại. `candidate.hpp` và `backup.hpp` bị Git bỏ qua; `driver_ml_weights.hpp`
được theo dõi nên hãy kiểm tra thay đổi trước khi commit model mới.

## 9. Cảnh báo và giới hạn

| Tình huống | Mức mặc định |
| --- | --- |
| Nhắm mắt liên tục | Từ 1,5 giây |
| Ngáp dồn | Từ 3 lần trong 60 giây |
| PERCLOS | Từ 15% trong 60 giây; ít nhất 30 giây quan sát mắt hợp lệ |
| Không thấy mặt | Liên tục 5 giây |

Buzzer ở GPIO14 kêu theo nhịp tương ứng. LED RGB ở GPIO48 nháy xanh ngắn khi
ThingsBoard xác nhận telemetry MQTT. Bộ đếm blink/ngáp bắt đầu lại khi bo
khởi động lại. `sequence` là số thứ tự gửi trong lần khởi động; dùng cùng
`boot_id` để phân biệt các lần chạy. Kết quả thị giác là **thử nghiệm**, chưa
được kiểm định làm thiết bị an toàn khi lái xe.

## 10. Xử lý lỗi thường gặp

| Hiện tượng | Cách kiểm tra |
| --- | --- |
| `idf.py is not recognized` | Chạy `. (Join-Path $IdfPath 'export.ps1')` trong **đúng cửa sổ PowerShell** rồi thử `idf.py --version`. |
| `BSP is not defined` | Đặt `$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path '.\tools').Path` khi đang ở **gốc repo**, rồi vào thư mục dự án. |
| `cc1plus.exe: out of memory` hoặc assembler báo `memory exhausted` | Máy PC thiếu RAM khi compile ESP-DL. Đặt `$env:IDF_PY_BUILD_JOBS = '1'` trong cửa sổ ESP-IDF rồi chạy lại `idf.py build`; không cần `fullclean`. Đây là lỗi bộ nhớ lúc build trên PC, không phải dung lượng PSRAM trên bo. |
| Không thấy cổng COM | Thử cáp dữ liệu/cổng USB khác; xem Device Manager và driver CH343. |
| COM đang được dùng | Đóng viewer, trình ghi và Serial Monitor; mỗi lúc chỉ một chương trình mở COM. |
| Viewer báo thiếu `cv2`, `numpy` hoặc `serial` | Chạy lại ba lệnh ở mục 2.3, và dùng đúng `.venv\Scripts\python.exe`. |
| Viewer đứng sau khi nhấn RST | Chờ bo khởi động vài giây. Nếu chưa tự nhận lại, nhấn `q` và mở viewer lại. |
| `Corrupt JPEG data` hoặc `Bad UART` tăng | Dùng viewer trong repo này, giữ baud mặc định 1500000, thử cáp USB ngắn hơn hoặc cổng USB khác. |
| `WiFi: OFFLINE`, reason `201` | Bo không thấy SSID; kiểm tra tên mạng và băng 2,4 GHz. Nếu đổi tên/mật khẩu, cấu hình và nạp lại. |
| `Time: WAIT` khi Wi-Fi đã kết nối | Bo chưa đồng bộ được giờ để kiểm tra chứng chỉ; kiểm tra Internet/DNS/NTP UDP 123 hoặc đổi SNTP server trong menu. |
| `MQTTS: OFFLINE` khi `Time: OK` | Kiểm tra URI `mqtts://mqtt.thingsboard.cloud:8883`, token của đúng device, CA, hostname và mạng có cho TCP 8883 đi qua không. |
| `Eyes: UNKNOWN` liên tục | Đưa đủ hai mắt vào khung hình, giảm góc quay đầu, kiểm tra ánh sáng mặt. |
| Camera không lên | Xác nhận đúng OV3660 và chân camera tương thích ESP32-S3-EYE; kiểm tra nguồn USB. |

## 11. Đưa thư mục này lên GitHub

Thư mục mẫu này đã được `git init`; nếu bạn sao chép chỉ các tệp nguồn sang
thư mục khác thì chạy `git init` như bên dưới. Repo có `.gitignore` cho
`sdkconfig`, `build`, `managed_components`, `.venv`, `recordings` và
`training_data`. **Không thêm ngoại lệ để commit các tệp đó.** Model đã học
trong `driver_ml_weights.hpp` được đưa lên Git để người khác build và chạy.
Trước khi upload, tạo một repo trống trên GitHub, sau đó mở PowerShell tại
gốc thư mục:

```powershell
git init
git add .
git status --short
git commit -m "Initial ESP32-S3 driver monitor"
git branch -M main
git remote add origin 'https://github.com/TEN-CUA-BAN/TEN-REPO.git'
git push -u origin main
```

Thay URL bằng repo thật. Trước `git commit`, xem `git status --short`: **không
được có** `sdkconfig`, `build`, `managed_components`, `.venv`, `recordings`,
`training_data` hoặc video/ảnh riêng. Nếu Git yêu cầu tên/email, đặt bằng `git config
--global user.name` và `git config --global user.email` theo tài khoản của
bạn. Bạn cũng có thể chọn repo **Private** nếu chỉ muốn chia sẻ cho nhóm.

## Cấu trúc repo

```text
ESP32S3_Driver_Monitor_Git/
├── README.md                 # Hướng dẫn này
├── docs/HE_THONG_HOAT_DONG.md # Giải thích chi tiết cơ chế hệ thống
├── docs/GIAI_THICH_TUNG_FILE.md # Vai trò file/hàm, thuật toán và lệnh chạy
├── docs/CHAY_LOCAL_A_Z.md    # Chạy ThingsBoard local và MQTT guard
├── docs/KE_HOACH_CHONG_GIA_MAO.md # Phạm vi và giới hạn xác thực telemetry
├── requirements-viewer.txt  # Thư viện Python xem stream
├── requirements-train.txt   # Thư viện Python huấn luyện
├── requirements-mqtt-guard.txt # Phụ thuộc gateway và kiểm thử guard
├── LICENSE                  # Giấy phép nguồn ESP-WHO
├── server/                  # Gateway MQTT và công cụ ThingsBoard local
├── tests/security/          # Kiểm thử HMAC, replay, TLS và ACL
├── tools/                   # Script build/flash, local server và CI
├── components/              # Các component ESP-WHO dùng khi build
└── examples/ov3660_face_detect/
    ├── CMakeLists.txt
    ├── sdkconfig.bsp.esp32_s3_eye_noglib  # Cấu hình mặc định, không có mật khẩu
    ├── partitions.csv
    ├── driver_monitor_viewer.py
    ├── record_training_video.py
    ├── train_driver_model.py
    ├── labels.example.json
    └── main/               # Firmware, CA công khai và trọng số ML
```

Các thư viện lớn trong `managed_components` và công cụ ESP-IDF được cài/tải
trên từng máy; không phải commit chúng vào Git. `sdkconfig` riêng của mỗi
người được tạo khi chạy `set-target`/`menuconfig`.
