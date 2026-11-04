# Giám sát tài xế với ESP32-S3-CAM OV3660

Dự án dùng **ESP32-S3-WROOM-1 N16R8**, camera **OV3660** theo chân
**ESP32-S3-EYE**, buzzer chủ động **GPIO14, kích hoạt LOW** và LED RGB GPIO48.
Phát hiện mặt, phân loại mắt/miệng, đếm blink/ngáp, tính PERCLOS và quyết
định cảnh báo đều chạy trên ESP32-S3. Repo giữ trọng số đã học; không kèm
ảnh/video/nhãn huấn luyện cá nhân.

**Chế độ được hướng dẫn chính là local có gateway xác minh MQTT**:

```text
Camera → ESP32-S3 → Wi-Fi / MQTTS + HMAC → gateway Python trên laptop
                                                   ↓
                                  ThingsBoard + PostgreSQL trong Docker Desktop
                                                   ↓
                                     Dashboard tại http://localhost:8080
ESP32-S3 → USB/COM → viewer Python: ảnh và kết quả đã tính trên bo
```

Laptop chạy gateway trực tiếp trên Windows, ngoài Docker. Gateway nhận
MQTTS ở cổng **8885**, cung cấp giờ NTP ở **UDP 123**, kiểm tra HMAC/phiên/
sequence/độ mới rồi chuyển số liệu vào ThingsBoard. Web 8080 và MQTT 18884
của ThingsBoard chỉ được publish trên localhost; PostgreSQL dùng mạng Docker.
Ảnh camera không gửi lên ThingsBoard.

Bo vẫn nhận diện và báo buzzer khi PC tắt, nhưng **gửi dữ liệu lên ThingsBoard
local cần laptop, gateway và các container đang chạy**. Viewer không làm
suy luận trên PC.

| Bạn cần làm gì? | Tài liệu |
| --- | --- |
| Cắm bo, cài môi trường, cấu hình, build/nạp | Các bước 1–7 dưới đây và [local A–Z](docs/CHAY_LOCAL_A_Z.md) |
| Hiểu thuật toán, bộ đếm, cảnh báo | [Hệ thống hoạt động](docs/HE_THONG_HOAT_DONG.md) |
| Hiểu từng file, hàm và lệnh chạy | [Giải thích từng file](docs/GIAI_THICH_TUNG_FILE.md) |
| Hiểu lớp bảo vệ và giới hạn | [Xác thực và chống phát lại MQTT](docs/KE_HOACH_CHONG_GIA_MAO.md) |
| Demo các ô bảo mật trên dashboard đang dùng | [Kịch bản demo](docs/DEMO_BAO_MAT_VA_KIEM_THU.md) |

Phạm vi demo bảo mật là **client Python mô phỏng bên gửi → gateway MQTT**,
đối chiếu thêm dữ liệu ThingsBoard qua REST. Bo thật tiếp tục gửi song song
để xác nhận đường vận hành; không tuyên bố đã chiếm quyền/tấn công firmware
ESP32, Wi-Fi, Windows hoặc dashboard. Không triển khai chống ảnh in/video giả,
nhận dạng danh tính người lái hoặc cảm biến nhịp tim/SpO₂.

## 1. Chuẩn bị phần cứng

1. Bo ESP32-S3-CAM **flash 16 MB, PSRAM 8 MB** và OV3660, đúng chân
   ESP32-S3-EYE. Tên N16R8 không tự xác định sơ đồ chân của mọi hãng.
2. Cáp USB có truyền dữ liệu; cáp chỉ sạc không tạo cổng COM.
3. Buzzer: tín hiệu GPIO14, GND chung, nguồn đúng thông số module. Firmware
   dành cho buzzer chủ động, kích hoạt LOW. GPIO4 là SDA camera nên không
   dùng làm tín hiệu buzzer.
4. Mạng cho ESP32 phải có **2,4 GHz**. PC và bo phải liên lạc được với nhau
   qua LAN; dùng cùng hotspot là cách dễ kiểm tra. Mạng khách cách ly thiết
   bị có thể chặn kết nối dù cùng tên Wi-Fi.

## 2. Mã nguồn và phần mềm PC

Hướng dẫn dùng Windows 10/11 và PowerShell, đã build với **ESP-IDF 5.5.5**.
Dùng lại Python/Git/Docker/ESP-IDF sẵn có; chỉ cài phần còn thiếu.
Mã nguồn, môi trường mới, cache và dữ liệu server của cấu hình này đặt trên D.

### 2.1. Tải repo trên máy mới

```powershell
cd D:\
git clone 'https://github.com/Kanj3eNguyen/BTL_IOT.git' ESP32S3_Driver_Monitor_Ready
cd D:\ESP32S3_Driver_Monitor_Ready
```

Có thể tải ZIP và giải nén thay cho clone. Dùng đường dẫn ngắn, không dấu/
khoảng trắng. Các ví dụ dùng `D:\ESP32S3_Driver_Monitor_Ready`; đổi đường dẫn
nếu bạn đặt tên khác. Máy đã có dự án thì dùng thư mục đó, không clone đè.

### 2.2. ESP-IDF, Python và Docker Desktop

- Cài ESP-IDF 5.5.5 và toolchain theo [hướng dẫn Windows chính thức](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/windows-setup.html).
  Ví dụ hiện dùng IDF `D:\esp\v5.5.5\esp-idf`, tools `D:\Espressif`, Python IDF
  `D:\Espressif\tools\python\v5.5.5\venv`. Chỉ tải source IDF chưa đủ để build.
- Các script PC dùng Python 3.11. Môi trường build IDF tách khỏi thư viện
  gateway/viewer. Không cài OpenCV hay phụ thuộc gateway vào Python IDF.
- Local server dùng Docker Desktop với Linux containers/WSL 2. Trước khi
  tải images, cấu hình đĩa Docker trên D theo [local A–Z](docs/CHAY_LOCAL_A_Z.md).
  `Setup-Local.ps1` kiểm tra storage, không tự cài Docker Desktop hoặc chuyển đĩa.

### 2.3. Thư viện viewer

Nếu Python đã có `cv2`, `numpy`, `serial`, launcher dùng lại; không cần cài.
Viewer tổng hợp cần thêm Tkinter, thuộc thành phần Tcl/Tk của bộ cài Python
Windows; không cài bằng `pip install tkinter`. Có thể dùng `-Classic` nếu
chỉ cần cửa sổ OpenCV cũ.
Trên máy mới chưa có môi trường xem ảnh, đứng ở gốc repo:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\requirements-viewer.txt
```

`Setup-Local.ps1` sẽ cài thêm phụ thuộc gateway vào `.venv` khi cần. Các lệnh
quay/train ở mục 8 dùng môi trường này; nếu chỉ xem, `Start-Viewer.ps1` cũng
có thể chọn Python khác đã đủ thư viện. `.venv` không được commit.

## 3. Cắm bo và tìm COM

Cắm cáp dữ liệu, mở **Device Manager → Ports (COM & LPT)** hoặc chạy:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

Máy đã thử dùng USB–UART CH343 và COM4. Thay COM4 bằng cổng thực tế của máy
bạn. Nếu chưa nhận CH343, lấy driver từ nhà sản xuất bo hoặc [WCH](https://www.wch-ic.com/downloads/CH343SER_ZIP.html).
COM dùng để nạp/xem stream; telemetry dùng Wi-Fi. Mỗi lúc chỉ một chương
trình mở COM: đóng viewer/recorder/Serial Monitor trước khi nạp.

## 4. Cấu hình lần đầu và đổi Wi-Fi

Trên máy đã có `sdkconfig`, giữ cấu hình đó; chỉ sửa bằng menuconfig khi
cần. Trên clone mới, lệnh dưới cấu hình target/BSP/model và mở menu, không
cần chạy `set-target` để xóa rồi tạo lại dự án.

Mở PowerShell tại gốc repo. Thay các đường dẫn công cụ nếu máy bạn khác:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
$ProjectRoot = (Get-Location).Path
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\tools\python\v5.5.5\venv'
$env:PYTHONPATH = $null
. 'D:\esp\v5.5.5\esp-idf\export.ps1'
$env:IDF_EXTRA_ACTIONS_PATH = Join-Path $ProjectRoot 'tools'
cd (Join-Path $ProjectRoot 'examples\ov3660_face_detect')
python (Join-Path $env:IDF_PATH 'tools\idf.py') -B build_guard `
  -D IDF_TARGET=esp32s3 -D BSP=esp32_s3_eye_noglib `
  -D DETECT_MODEL=human_face_detect `
  -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_s3_eye_noglib menuconfig
```

Trong **Driver monitor MQTT**, điền **Wi-Fi SSID/password** cho mạng 2,4 GHz.
Lưu bằng S và thoát Q. Clone mới giữ tùy chọn local guard tắt ở bước bootstrap;
setup sẽ sinh CA/khóa rồi bật nó. Máy đang chạy guard thì giữ guard đang bật.
Không cần nhập token ThingsBoard trên bo. File `sdkconfig` chứa bí mật dạng
chữ và bị Git bỏ qua; không sửa các bản sinh trong `build_guard/config`.

`export.ps1` chỉ kích hoạt công cụ, không tự chuyển bạn khỏi System32 tới
repo. `set-target` có thể fullclean/tạo lại sdkconfig nên không lặp lại khi
chỉ đổi Wi-Fi. Save trong menu chưa đổi bo: phải build/nạp ở mục 5.

## 5. Setup local, build và nạp

Quay về gốc repo, lấy IPv4 của Wi-Fi PC **sau khi PC đã nối đúng mạng**:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
Get-NetIPAddress -InterfaceAlias 'Wi-Fi' -AddressFamily IPv4
```

Dùng IP thực tế đó, không dùng IP VMware/VPN/localhost. Ví dụ nếu PC nhận
`172.20.10.4`, setup lần đầu:

```powershell
.\tools\Setup-Local.ps1 -HostIP 172.20.10.4
.\tools\Build-Flash.ps1 -Port COM4
```

Setup tạo CA/chứng chỉ IP SAN, khóa/mật khẩu riêng, cấp tài khoản/device
ThingsBoard, tạo dashboard, cấu hình guard trong sdkconfig và bật server.
Token ThingsBoard chỉ gateway giữ. Xem [local A–Z](docs/CHAY_LOCAL_A_Z.md)
để hiểu marker database, vị trí đĩa và từng tham số.

Build dùng `build_guard`, bật ccache và mặc định **1 job / baud 460800**.
Đã thử thành công trên bo của dự án với **4 job / baud 921600**:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Build-Flash.ps1 -Port COM4 -Jobs 4 -Baud 921600
```

RAM ít thì giảm `-Jobs 1` hoặc `2`, tạm dừng server khi build. Nếu baud cao
lỗi truyền thì dùng `-Baud 460800`. `-BuildOnly` chỉ biên dịch. Giữ thư mục
build/cache để lần sau chỉ làm phần đổi; không fullclean thường xuyên.
Thông báo `Hash of data verified` xác nhận dữ liệu ghi flash; nó chưa xác
nhận Wi-Fi/MQTT. `Build-Flash.ps1` chuyển cwd tới thư mục ví dụ, nên các lệnh
`tools` kế tiếp cần `cd` về gốc repo.

## 6. Xem stream và dashboard

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Start-Viewer.ps1 -Port COM4
```

Launcher tìm Python đã đủ thư viện, tách khỏi môi trường ESP-IDF; có thể
chỉ định `-Python 'D:\duong-dan\python.exe'`. Ảnh và kết quả lấy từ bo.
Nhấn Q trong cửa sổ video để đóng/nhường COM.

Mặc định mở **viewer tổng hợp** trong một cửa sổ:

- Camera và tốc độ Video/AI, tuổi ảnh, số gói UART/ảnh nguồn lỗi.
- ESP32: Wi-Fi, đồng bộ giờ, MQTT/session, sequence/PUBACK/ACK, RAM, TLS,
  mặt/mắt/miệng, blink/ngáp/PERCLOS và buzzer.
- Gateway: PID/health, MQTT tới ThingsBoard, outbox, bốn bộ đếm bảo mật;
  REST cho biết sequence đã lưu và tuổi bản tin.
- Hai tab log: sự kiện ESP32 suy ra từ JSON serial và quyết định trong audit
  gateway. Đây không phải toàn bộ log ESP-IDF thô: UART đang truyền JPEG nhị phân.

Đọc serial, đọc health/audit và gọi REST ở các thread riêng. Mất COM sẽ tự
thử nối lại; dữ liệu health/telemetry cũ được đánh dấu, không giữ trạng thái
xanh như đang nhận mới. Viewer không start/stop gateway và không chạy suy luận.
Nút **Mở dashboard** mở trình duyệt local. Đóng viewer vẫn giữ server/bo chạy.

Giao diện dùng Tkinter có sẵn trong thành phần Tcl/Tk của Python Windows,
không có package pip Tkinter cần cài. Nếu muốn cửa sổ OpenCV cũ:

```powershell
.\tools\Start-Viewer.ps1 -Port COM4 -Classic
```

Mở [ThingsBoard local](http://localhost:8080), đăng nhập bằng thông tin riêng
trong `.private/thingsboard/DANG_NHAP.txt` rồi chọn **Dashboards → BTL IoT ·
Giám sát tài xế**. Device vận hành tên **ESP32S3 Driver Monitor - Verified**.
Link dashboard cụ thể của máy nằm ở `.private/thingsboard/dashboard.json`.

| Quan sát | Ý nghĩa |
| --- | --- |
| WiFi CONNECTED | Bo đã nhận IP từ Wi-Fi; chưa chứng minh tới được PC |
| Time OK | Bo đã đồng bộ giờ để kiểm tra chứng chỉ |
| Guard READY | Đã nhận phiên có HMAC hợp lệ từ gateway |
| `mqtt_acked` tăng | Broker đầu vào đã PUBACK; chưa chứng minh đã chuyển ThingsBoard |
| `TB seq` / `guard_forwarded_count` tăng, LED nháy xanh | Bo đã nhận ACK ứng dụng có HMAC sau PUBACK của ThingsBoard |
| Latest telemetry có timestamp mới và VERIFIED | REST/UI đã thấy mẫu được lưu; VERIFIED không chứng minh model đúng |

Dashboard dùng counter tích lũy; demo live làm counter tăng thật. Để demo:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Test-LiveSecurity.ps1
```

Đọc [kịch bản demo](docs/DEMO_BAO_MAT_VA_KIEM_THU.md) trước khi trình bày:
client Python tạo gói tấn công mô phỏng, gateway thật kiểm tra; đây không
phải bài tấn công giao diện dashboard hoặc chiếm quyền ESP32. Bộ hồi quy
riêng dùng `Test-Security.ps1 -Demo`, cổng 8886 và device ThingsBoard thử.

Ba lớp được thử riêng là quyền đăng nhập MQTT, tính toàn vẹn HMAC và chống
phát lại bằng sequence/session. Gói trùng không tạo thêm mẫu; gói sai bị
chặn ở gateway trước khi chuyển ThingsBoard, không chỉ bị ẩn trên dashboard.
Sequence/session không ngăn người đã có đủ mật khẩu và khóa HMAC tạo một
gói giả mới hợp lệ. Mật khẩu và HMAC hiện cùng nằm trong cấu hình phần mềm.
Xem [giả định và giới hạn](docs/KE_HOACH_CHONG_GIA_MAO.md#ba-lớp-kiểm-thử-và-ý-nghĩa-thực-tế).

Phương án dùng máy ảo cùng LAN được giải thích trong [tài liệu demo](docs/DEMO_BAO_MAT_VA_KIEM_THU.md#11-máy-ảo-cùng-lan-phương-án-mở-rộng-chưa-có-runner-từ-xa).
Hiện chưa có runner từ xa; các script đang chạy trên laptop. Máy ảo có thể
dùng CA công khai hiện có, không cần sinh CA riêng cho từng client.

## 7. Chạy hằng ngày, đổi mạng hoặc máy

- Bo đã có firmware: cắm nguồn, không cần build/train lại. Bật server:

  ```powershell
  cd D:\ESP32S3_Driver_Monitor_Ready
  .\tools\Start-Local.ps1
  ```

- Tắt riêng dịch vụ dự án: `Stop-Local.ps1`, đóng viewer bằng Q, rồi rút
  nguồn bo. Container/volume và khóa được giữ; Docker Desktop có thể vẫn
  chiếm RAM trong VmmemWSL. Quit Docker Desktop khi không cần các container.
- Đổi Wi-Fi: sửa menuconfig theo mục 4, build/nạp. Nếu PC đổi IP, cần cập
  nhật URI/SNTP/chứng chỉ bằng provision và nạp lại theo [local A–Z](docs/CHAY_LOCAL_A_Z.md).
  RST chỉ chạy lại firmware đang có, không tự lấy cấu hình mới từ PC.
  Provision giữ mật khẩu/HMAC cho device hiện có, nhưng sinh lại cả CA và
  chứng chỉ khi IP đổi; cùng IP và đủ file thì dùng lại. Xem [cơ chế CA](examples/ov3660_face_detect/main/certs/README.md).
- Chuyển sang PC chỉ xem COM: dùng Python có thư viện viewer, đổi cổng COM.
  Chuyển **cả server** sang PC khác còn cần setup riêng, khóa/CA/IP phù hợp,
  cấu hình lại bo; không chỉ đổi COM.
- Preview hiện Video FPS và AI FPS riêng. UART dùng **1500000 baud**, khác
  baud nạp 460800/921600. `Age` chưa gồm thời gian truyền UART. Bộ lọc ảnh
  nguồn, checksum và bỏ preview cũ giúp hạn chế lỗi; không bảo đảm loại được
  mọi kiểu nhiễu ảnh/JPEG.
- Bù sáng tự điều chỉnh AE theo mặt/toàn cảnh. Nó không tự train và không
  bảo đảm đúng ở mọi phòng/góc/người. Xem mục 2.10 của [giải thích từng file](docs/GIAI_THICH_TUNG_FILE.md).
- Ghi preview để xem lại: dùng Python đã có requirements viewer. Ví dụ với
  môi trường `.venv` đã cài ở mục 2.3:

  ```powershell
  cd D:\ESP32S3_Driver_Monitor_Ready
  .\.venv\Scripts\python.exe .\examples\ov3660_face_detect\driver_monitor_viewer.py --port COM4 --out .\examples\ov3660_face_detect\recordings\test_01 --seconds 60
  ```

  Dữ liệu có ảnh mặt, chỉ giữ cục bộ trong `recordings/` đã bị Git bỏ qua.

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

Các lệnh quay/train dùng `.venv` có đủ thư viện xử lý ảnh và huấn luyện.
Chỉ khi muốn huấn luyện lại, chuẩn bị một lần ở gốc repo:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
if (-not (Test-Path .\.venv\Scripts\python.exe)) { python -m venv .venv }
.\.venv\Scripts\python.exe -m pip install -r .\requirements-train.txt
```

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

Dùng môi trường đã chuẩn bị đầu mục 8, chạy với đúng `$session` vừa đặt:

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

Đóng viewer, quay về gốc repo rồi chạy `tools/Build-Flash.ps1 -Port COM4`
như mục 5 để dùng đúng `build_guard` và cấu hình guard. Sau đó
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

Buzzer ở GPIO14 kêu theo nhịp tương ứng. LED RGB ở GPIO48 nháy xanh khi bo
nhận ACK ứng dụng có HMAC từ gateway sau PUBACK của ThingsBoard; PUBACK
chưa phải bằng chứng database đã commit. Bộ đếm blink/ngáp bắt đầu lại khi
bo khởi động lại. `boot_id` số trong dữ liệu hành vi và `sequence` giúp
đọc các lần chạy; bảo vệ replay dùng thêm boot nonce, session ID và sequence
trong phong bì đã ký. Kết quả thị giác là **thử nghiệm**, chưa
được kiểm định làm thiết bị an toàn khi lái xe.

## 10. Xử lý lỗi thường gặp

| Hiện tượng | Cách kiểm tra |
| --- | --- |
| `idf.py is not recognized` | Kích hoạt ESP-IDF theo mục 4 trong đúng PowerShell; có thể gọi `python (Join-Path $env:IDF_PATH 'tools\idf.py')` như ví dụ. |
| `BSP is not defined` | Đặt `$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path '.\tools').Path` khi đang ở **gốc repo**, rồi vào thư mục dự án. |
| `cc1plus.exe: out of memory` hoặc assembler báo `memory exhausted` | Chạy Build-Flash với `-Jobs 1`, dừng server tạm thời nếu thiếu RAM; không fullclean. Đây là RAM PC, không phải PSRAM trên bo. |
| Không thấy cổng COM | Thử cáp dữ liệu/cổng USB khác; xem Device Manager và driver CH343. |
| COM đang được dùng | Đóng viewer, trình ghi và Serial Monitor; mỗi lúc chỉ một chương trình mở COM. |
| Viewer báo thiếu `cv2`, `numpy` hoặc `serial` | Chạy `tools/Start-Viewer.ps1 -Port COM4` để chọn Python đã có thư viện. Chỉ cài requirements-viewer.txt theo mục 2.3 khi chưa có môi trường viewer; không cài vào Python ESP-IDF. |
| Viewer tổng hợp thiếu `tkinter` | Bổ sung thành phần Tcl/Tk qua bộ cài Python đang dùng hoặc chọn Python có Tkinter bằng -Python; -Classic mở viewer OpenCV cũ. Không pip install tkinter. |
| Gateway xanh nhưng dữ liệu ThingsBoard mang nhãn CŨ | Gateway còn chạy nhưng chưa có mẫu bo mới, hoặc REST đang không đọc được. Kiểm tra Wi-Fi/time/MQTT/ACK và tuổi telemetry; trạng thái server không thay cho trạng thái bo. |
| Viewer đứng sau khi nhấn RST | Chờ bo khởi động vài giây. Nếu chưa tự nhận lại, nhấn `q` và mở viewer lại. |
| `Corrupt JPEG data` hoặc `Bad UART` tăng | Dùng viewer trong repo này, giữ baud mặc định 1500000, thử cáp USB ngắn hơn hoặc cổng USB khác. |
| `WiFi: OFFLINE`, reason `201` | Bo không thấy SSID; kiểm tra tên mạng và băng 2,4 GHz. Nếu đổi tên/mật khẩu, cấu hình và nạp lại. |
| `Time: WAIT` khi Wi-Fi đã kết nối | Kiểm tra PC/bo cùng LAN, IP SNTP đúng IP PC hiện tại, gateway đang mở UDP 123; Wi-Fi connected chưa chứng minh tới được gateway. |
| Guard WAIT hoặc MQTT offline khi Time OK | Kiểm tra URI `mqtts://IP_PC:8885`, CA/IP/thời hạn chứng chỉ và TCP8885; sau khi MQTT nối còn cần hello/grant có HMAC. |
| Guard READY nhưng TB seq đứng | Kiểm tra container ThingsBoard, MQTT localhost18884, token chỉ ở gateway và log gateway. |
| Lỗi cryptography khi export ESP-IDF | Không trộn PYTHONPATH của gateway vào IDF. Build-Flash tự xóa PYTHONPATH trước kích hoạt IDF; không cài lại package ngẫu nhiên vào Python IDF. |
| Mắt KHÔNG RÕ hoặc `Eyes: UNKNOWN` trong classic | Đưa đủ hai mắt vào khung hình, giảm góc quay đầu, kiểm tra ánh sáng mặt. |
| Camera không lên | Xác nhận đúng OV3660 và chân camera tương thích ESP32-S3-EYE; kiểm tra nguồn USB. |

## 11. Đưa thư mục này lên GitHub

Thư mục mẫu này đã được `git init`; nếu bạn sao chép chỉ các tệp nguồn sang
thư mục khác thì chạy `git init` như bên dưới. Repo có `.gitignore` cho
`sdkconfig`, `.private`, `build`, `build_guard`, `managed_components`, `.venv`, `recordings` và
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
được có** `sdkconfig`, `.private`, `build_guard`, `build`, `managed_components`,
`.venv`, `recordings`, `training_data`, khóa, CA local của máy hoặc firmware
chứa bí mật. Nếu Git yêu cầu tên/email, đặt `git config user.name` và
`git config user.email` riêng trong repo. Repo clone đã có origin thì kiểm
tra `git remote -v` thay vì thêm origin lần nữa.

## Cấu trúc repo

```text
ESP32S3_Driver_Monitor_Ready/
├── README.md                 # Hướng dẫn này
├── docs/HE_THONG_HOAT_DONG.md # Giải thích chi tiết cơ chế hệ thống
├── docs/GIAI_THICH_TUNG_FILE.md # Vai trò file/hàm, thuật toán và lệnh chạy
├── docs/CHAY_LOCAL_A_Z.md    # Chạy ThingsBoard local và MQTT guard
├── docs/KE_HOACH_CHONG_GIA_MAO.md # Phạm vi và giới hạn xác thực telemetry
├── docs/DEMO_BAO_MAT_VA_KIEM_THU.md # Kịch bản live 8885 và hồi quy riêng 8886
├── requirements-viewer.txt  # Thư viện Python xem stream
├── requirements-train.txt   # Thư viện Python huấn luyện
├── requirements-mqtt-guard.txt # Phụ thuộc gateway và kiểm thử guard
├── LICENSE                  # Giấy phép nguồn ESP-WHO
├── server/                  # Gateway MQTT và công cụ ThingsBoard local
├── tests/security/          # Kiểm thử HMAC, replay, TLS và ACL
├── tests/monitor/           # Kiểm thử health/counter/log của viewer tổng hợp
├── tools/                   # Script build/flash, local server và CI
├── components/              # Các component ESP-WHO dùng khi build
└── examples/ov3660_face_detect/
    ├── CMakeLists.txt
    ├── sdkconfig.bsp.esp32_s3_eye_noglib  # Cấu hình mặc định, không có mật khẩu
    ├── partitions.csv
    ├── driver_monitor_viewer.py
    ├── driver_system_monitor.py # Camera + ESP32 + gateway/ThingsBoard
    ├── record_training_video.py
    ├── train_driver_model.py
    ├── labels.example.json
    └── main/               # Firmware, CA công khai và trọng số ML
```

Các thư viện lớn trong `managed_components` và công cụ ESP-IDF được cài/tải
trên từng máy; không commit chúng vào Git. `sdkconfig` riêng được tạo/cập
nhật theo bước cấu hình tại mục 4 rồi được provision điền tham số guard.
