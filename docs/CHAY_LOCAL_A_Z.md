# Chạy ThingsBoard local và chống sửa/phát lại MQTT từ A đến Z

Bản này dùng PC làm cả máy chủ. Chỉ nhận diện bằng camera; không có nhịp
tim/SpO₂ hoặc model chống ảnh in/video giả. Nhận diện mắt/miệng vẫn chạy
trên ESP32-S3. Gateway trên PC xác minh HMAC và chống replay trước khi
chuyển số liệu vào ThingsBoard.

## 1. Sơ đồ và nơi lưu

```text
ESP32-S3 + OV3660
 → Wi-Fi LAN → mqtts://IP_PC:8885
 → gateway Python: kiểm tra HMAC, phiên, sequence và độ mới
 → ThingsBoard local (MQTT localhost:18884)
 → trình duyệt: http://127.0.0.1:8080
```

- Thư mục mã nguồn hiện tại: `D:\ESP32S3_Driver_Monitor_Ready`.
- ThingsBoard và PostgreSQL chạy trong **Docker Desktop**, nhóm Compose
  **driver-monitor-local** ở trang **Containers**.
- Trên máy hiện tại, đĩa Docker Desktop nằm ở
  `D:\ThingsBoard_Local\docker-data\disk\docker_data.vhdx`.
  Volume `driver-monitor-local_postgres-data` giữ database trong đĩa này.
- `.private` trong gốc repo chứa khóa, CA, tài khoản, database xác minh và log.
- Công cụ Python/Git/WSL đã có trên C vẫn dùng được; không cần cài bản trùng.
- Script luôn chỉ định context `desktop-linux` và chỉ quản lý nhóm của dự án.
  Các container Docker Desktop của dự án khác được giữ nguyên.
- Gateway là Python chạy nền trên Windows, ngoài Docker; không phải một
  phần cứng khác. Docker Desktop dùng WSL2 nên có tiến trình `VmmemWSL`.

PC phải bật, có giờ đúng, và cùng mạng Wi-Fi/LAN với bo khi cần gửi dữ liệu.
Tắt server không làm mất chức năng nhận diện/cảnh báo cục bộ trên bo, nhưng
telemetry sẽ không tới ThingsBoard cho tới khi server chạy trở lại.

## 2. Chuẩn bị lần đầu

Cần Windows có Docker Desktop dùng WSL 2, Python 3.11, ESP-IDF 5.5.x và bo ESP32-S3/OV3660
đúng sơ đồ ESP32-S3-EYE. Xem phần cài ESP-IDF, chân camera và buzzer trong
[README chính](../README.md). Images/database/build cần nhiều GB trên D;
tính thêm dung lượng ESP-IDF/toolchain nếu máy chưa có. Nên có vài GB RAM
khả dụng lúc dựng server. Build ESP-DL có thể tốn nhiều RAM; mặc định script
dùng một job, có thể chọn `-Jobs 2` hoặc `4` khi đủ bộ nhớ.

Mở PowerShell tại gốc repo. Dùng lại Python/Docker Desktop đã có:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
python --version
docker desktop status
docker --context desktop-linux info
```

Máy mới cần cài Docker Desktop theo tài liệu chính thức và khởi động engine.
Trước khi tải images, chọn **Settings → Resources → Advanced → Disk image
location** trên ổ D. Nếu phiên bản Docker hiện tại không có tùy chọn này,
cần chuyển đĩa theo hướng dẫn tương ứng của Docker trước khi chạy setup.
Script kiểm tra vị trí lưu trên D, không tự sửa Windows hoặc cài Docker Desktop.
Trên máy hiện tại đã xác nhận thư mục đĩa mặc định liên kết tới D; không cần
chuyển lần nữa. Tham số `-Storage` chỉ chọn chỗ lưu phụ trợ, không tự di chuyển
đĩa Docker Desktop.

Dự án cần có `examples\ov3660_face_detect\sdkconfig` với Wi-Fi 2,4 GHz
đúng. Người mới tạo nó bằng bước ESP-IDF/menuconfig trong README. Không
chép mật khẩu/token của người khác vào Git. Nếu máy hiện tại đã cấu hình
và nạp bo thì giữ sdkconfig đang có, không chạy `set-target` lại để tránh
mất các cấu hình riêng.

## 3. Lấy IP của PC và setup server

```powershell
Get-NetIPAddress -AddressFamily IPv4 -InterfaceAlias 'Wi-Fi'
```

Dùng địa chỉ IPv4 của mạng mà bo cũng đang kết nối, ví dụ `172.20.10.4`.
Đây là ví dụ, không phải IP cố định bắt buộc cho mọi máy/mạng.
Không dùng IP VMware, VPN hoặc `127.0.0.1` làm địa chỉ server cho bo.

Chạy một lần:

```powershell
.\tools\Setup-Local.ps1 -HostIP 172.20.10.4
```

Lệnh này:

1. Dùng Python có sẵn, tạo môi trường phụ thuộc gateway trong `.venv` nếu cần.
2. Kiểm tra Docker Desktop, context `desktop-linux` và vị trí đĩa trên D.
3. Sinh khóa HMAC 32 byte, mật khẩu MQTT, CA và chứng chỉ server có IP SAN.
4. Ghi các tham số guard vào sdkconfig riêng; giữ backup trước đổi trong `.private`.
5. Tải ThingsBoard CE 4.3.1.8 và PostgreSQL 18, khởi tạo database lần đầu.
6. Tạo device `ESP32S3 Driver Monitor - Verified`, cấp token chỉ gateway biết.
7. Đổi mật khẩu demo và lưu tài khoản local; khởi động gateway và server.

Không chạy INSTALL_TB thủ công trên database đã có dữ liệu. Script dùng
mốc `database_initialized` để tránh khởi tạo lại. Không xóa mốc này để chữa
lỗi và không dùng `docker compose down -v` nếu cần giữ dữ liệu.

## 4. Build và nạp firmware

Cắm USB dữ liệu, xem cổng trong Device Manager. Đóng viewer/Serial Monitor
trước khi nạp. Với máy đang dùng COM4:

```powershell
.\tools\Build-Flash.ps1 -Port COM4
```

Mặc định script dùng ESP-IDF `D:\esp\v5.5.5\esp-idf`, tools `D:\Espressif`
và môi trường Python IDF `D:\Espressif\tools\python\v5.5.5\venv`. Nếu máy
khác đã có bộ công cụ ở nơi khác, truyền đường dẫn có sẵn, không cần cài lại:

```powershell
.\tools\Build-Flash.ps1 -Port COM7 -IdfPath 'D:\esp\v5.5.5\esp-idf' `
  -ToolsPath 'D:\Espressif' -PythonEnv 'D:\Espressif\tools\python\v5.5.5\venv'
```

- Build vào `examples\ov3660_face_detect\build_guard`, không sửa thư mục backup.
- SDK configuration nằm ở thư mục dự án, không sửa `build_guard\config\sdkconfig.h/json`.
- Biên dịch lần đầu lâu vì cần build ESP-IDF và ESP-DL. Lần sau chỉ build phần đổi.
- Chỉ build, chưa nạp: thêm `-BuildOnly`.
- Nếu thiếu RAM lúc build, dừng server bằng Stop-Local, build/nạp rồi bật lại.
- Firmware chứa khóa riêng, không upload `.bin`, `sdkconfig` hoặc `build_guard` lên Git.

### Build/nạp nhanh hơn

Script giữ `build_guard` để chỉ biên dịch phần thay đổi và bật ccache.
Giữ thư mục build/cache giữa các lần làm việc; chỉ dùng `set-target` khi
thiết lập lần đầu hoặc thật sự đổi chip. `fullclean` sẽ khiến lần build tiếp
theo phải làm lại toàn bộ.

Mặc định dùng 1 job và baud 460800 để phù hợp với máy đang ít RAM.
Trên bo/máy của dự án đã build/nạp thành công bằng 4 job và baud 921600:

```powershell
.\tools\Build-Flash.ps1 -Port COM4 -Jobs 4 -Baud 921600
```

`-Jobs` tăng số tác vụ biên dịch song song; `-Baud` chỉ tăng tốc truyền khi
nạp firmware, không tăng FPS camera. Tốc độ thực tế phụ thuộc RAM, USB và
cáp; máy khác cần chọn mức phù hợp, không mặc định 4 job luôn nhanh hơn.
Nếu lỗi truyền/nạp ở tốc độ cao, đổi về `-Baud 460800`.

Nếu RAM đang thiếu, có thể tạm dừng dịch vụ trong lúc build:

```powershell
.\tools\Stop-Local.ps1
.\tools\Build-Flash.ps1 -Port COM4 -Jobs 2 -Baud 921600
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Start-Local.ps1
```

Build-Flash chuyển cwd vào thư mục ví dụ; cần về gốc repo trước lệnh tools
kế tiếp như trên. Chọn `-Jobs 1` nếu gặp lỗi thiếu bộ nhớ. Docker Desktop vẫn có thể dùng
RAM nền sau khi dừng container, nên xem RAM khả dụng trong Task Manager
trước khi tăng số job.

## 5. Khởi động những lần sau

Từ lúc cắm dây vào bo:

1. Cắm USB dữ liệu. Bo có firmware rồi sẽ tự khởi động; không cần nạp lại
   chỉ vì rút/cắm nguồn. Nếu đổi cổng trên PC thì chỉ đổi `--port` ở viewer.
2. Bo cần mạng 2,4 GHz; PC cần cùng LAN có thể tới bo và IP PC chưa đổi.
   PC không nhất thiết cùng băng tần nếu router nối các băng trong cùng LAN.
3. Mở PowerShell tại gốc repo và bật server:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Start-Local.ps1
```

Mở **Docker Desktop → Containers → driver-monitor-local** và bấm mũi tên
để thấy `thingsboard-1`, `postgres-1`. Cả hai phải đang chạy; PostgreSQL có
healthcheck `healthy`. Bạn có thể xem log ThingsBoard bằng cách chọn container.
Chỉ bấm Start trong Docker Desktop sẽ bật container, còn gateway Python vẫn
cần `Start-Local.ps1` để bo gửi telemetry đã xác minh.

Kiểm tra tương đương bằng lệnh:

```powershell
docker --context desktop-linux compose --env-file .\.private\thingsboard\compose.env `
  -f .\server\thingsboard_local\compose.yml ps
```

4. Mở http://127.0.0.1:8080, đăng nhập bằng thông tin đã lưu:

```powershell
Get-Content .\.private\thingsboard\DANG_NHAP.txt
```

5. Vào **Entities → Devices → ESP32S3 Driver Monitor - Verified → Latest telemetry**.
   Các trường blink/yawn/PERCLOS và `auth_sequence` sẽ cập nhật khi bo gửi.
   Chỉ khi quan sát hợp lệ mới có giá trị mắt/miệng/PERCLOS tương ứng.

Để xem giao diện quản trị, vào **Dashboards → BTL IoT ·
Giám sát tài xế**. Dashboard local có 16 ô trạng thái, biểu đồ blink/ngáp/PERCLOS,
lịch sử cảnh báo, counter xác thực MQTT và bảng mẫu đã xác minh. Dữ liệu tới
ThingsBoard local tại localhost:8080.

Link cụ thể của máy được lưu trong `.private/thingsboard/dashboard.json`.
Nếu cần tạo dashboard cho bộ dữ liệu local đã setup trước đó:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\.venv\Scripts\python.exe -m server.thingsboard_local.dashboard
```

Lệnh bình thường giữ các chỉnh sửa dashboard hiện có. Chỉ thêm `--replace`
khi muốn ghi lại bố cục tự sinh. Các ô hợp lệ/chưa đủ dữ liệu và tuổi bản tin
được xét riêng, không dùng điểm model như độ chính xác. Source widget ở
`server/thingsboard_local/dashboard_status.js` và `.css`; template/layout
trong `dashboard.py`. Bản JSON export riêng của máy nằm ở `.private/thingsboard`.

Server thường cần vài phút cho lần khởi động đầu. Chạy lại Start-Local
không xóa database/khóa. Nó không tự mở một gateway thứ hai nếu cổng 8885
đang có tiến trình lắng nghe; nếu cổng đó do ứng dụng khác chiếm thì kiểm tra
trước, không tự tắt ứng dụng không liên quan.

## 6. Stream camera trên PC

Dùng launcher tự chọn Python đã có thư viện `cv2`, `numpy`, `serial`:

```powershell
.\tools\Start-Viewer.ps1 -Port COM4
```

Máy mới chưa có thư viện viewer: tạo/kích hoạt môi trường Python mong muốn
và cài `requirements-viewer.txt`. Không cần cài lại nếu Python hiện tại đã có.
Gateway và viewer chạy đồng thời được: gateway dùng mạng, viewer dùng COM.
Launcher kiểm tra Python có sẵn và dùng thư viện của đúng môi trường đó,
tách khỏi terminal ESP-IDF; không tự cài gì. Dùng `-Python 'D:\...\python.exe'`
nếu muốn chọn môi trường cụ thể. Đừng cài OpenCV vào Python ESP-IDF chỉ để
chạy viewer, vì các ràng buộc thư viện của môi trường build khác gateway/viewer.

Viewer hiển thị `Guard: READY` khi có phiên xác thực; `TB seq` là sequence
đã được gateway báo ThingsBoard PUBACK. LED GPIO48 nháy xanh khi có ACK
ứng dụng hợp lệ. `MQTT connected` hoặc PUBACK broker đầu vào tự nó chưa
chứng minh dữ liệu đã sang ThingsBoard.

Nhấn `q` để đóng viewer; chỉ một chương trình mở COM tại một thời điểm.

Viewer mặc định là cửa sổ tổng hợp Tkinter với ba vùng camera/ESP32/gateway,
hai tab sự kiện ESP32 và audit gateway. Tkinter thuộc Python Windows, không
cài bằng pip. `-Classic` trên Start-Viewer mở lại cửa sổ OpenCV trước đây.

Gateway xuất `.private/runtime/gateway-health.json` mỗi giây; sau 6 giây
không có snapshot mới, viewer đánh dấu cũ/dừng. File chỉ chứa metadata đã
chọn, không có token/mật khẩu/HMAC key/payload. Test gateway cổng 8886 không
ghi đè health của gateway thật. Server/serial được đọc bởi thread riêng,
REST được đọc lại khoảng 3 giây sau mỗi lượt gọi, có timeout và không chặn luồng ảnh.

Các counter ở vùng gateway lấy từ health thật, còn “TB đã lưu sequence”
lấy REST và hiển thị tuổi mẫu. API đọc được không có nghĩa bo đang gửi:
một mẫu cũ VERIFIED vẫn phải hiện CŨ. Viewer tự nối lại khi COM trở lại;
không mở thêm Serial Monitor cùng COM. “Sự kiện ESP32” là chuyển trạng thái
trong JSON, không phải console ESP-IDF đầy đủ khi UART đang truyền JPEG.

## 7. Dừng và làm tiếp hôm sau

```powershell
.\tools\Stop-Local.ps1
```

Lệnh dừng gateway, ThingsBoard và PostgreSQL; giữ nguyên Docker Desktop,
các dự án khác, volume, số liệu đã lưu và khóa. Đóng viewer bằng `q`, rồi có thể rút USB.
Hôm sau chỉ cần Start-Local và cắm bo. Không cần setup/train/build lại
nếu chưa thay cấu hình hoặc mã nguồn.

## 8. Đổi mạng hoặc IP PC

Đóng viewer bằng Q để nhường COM; kết nối laptop vào mạng mới trước. Kích
hoạt môi trường ESP-IDF và mở menuconfig theo [mục 4 README](../README.md#4-cấu-hình-lần-đầu-và-đổi-wi-fi),
vào Driver monitor MQTT, sửa Wi-Fi SSID/password, S lưu và Q thoát. Giữ
guard bật. Kiểm tra IPv4 của card mạng thực tế, không lấy IP Docker/VPN:

```powershell
Get-NetIPAddress -InterfaceAlias 'Wi-Fi' -AddressFamily IPv4
```

- Đổi Wi-Fi của bo: sửa SSID/password trong `idf.py menuconfig`, rồi build/nạp.
- PC và bo cần cùng mạng có thể liên lạc trực tiếp. Chỉ thấy `WiFi: CONNECTED`
  trên bo chưa chứng minh bo tới được gateway. Nếu bo dùng hotspot mới nhưng
  PC vẫn ở Wi-Fi cũ, hãy chuyển PC sang mạng của bo rồi lấy lại IPv4 của PC.
  Mạng khách có cách ly thiết bị cũng có thể chặn liên lạc dù cùng SSID.
- PC đổi IP: chạy lại provision để cập nhật server, CA/chứng chỉ và sdkconfig:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Stop-Local.ps1
.\.venv\Scripts\python.exe -m server.mqtt_guard.provision --host IP_MOI_CUA_PC --configure-board
.\tools\Build-Flash.ps1 -Port COM4
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Start-Local.ps1
```

Thay `IP_MOI_CUA_PC` bằng IPv4 thật trước khi chạy; không gõ nguyên chuỗi ví dụ.

Chứng chỉ và URI phải khớp IP, vì vậy đổi IP PC cần nạp lại CA/URI vào bo.
Khóa HMAC/mật khẩu hiện có được giữ khi provision lại. Không sao chép
`.private` sang máy khác để chia sẻ chung danh tính; máy mới nên cấp khóa riêng.

Provision tự sinh chứng chỉ: cùng IP và đủ file/marker thì giữ bộ hiện có;
đổi IP hoặc thiếu các file được kiểm tra thì sinh lại cả CA và chứng chỉ
server. HMAC/mật khẩu của cùng device vẫn được giữ. CA là chứng chỉ công
khai để client tin gateway, không phải mật khẩu MQTT hoặc khóa ký HMAC.
CA mặc định 3650 ngày, chứng chỉ server 365 ngày; nhánh giữ lại hiện chưa
tự kiểm tra hạn/gia hạn. Xem [README chứng chỉ](../examples/ov3660_face_detect/main/certs/README.md).

## 9. Kiểm thử chống giả mạo

Phạm vi là tính toàn vẹn/xác thực/freshness/replay của telemetry ở đầu vào
gateway. Các gói tấn công do **client Python mô phỏng bên gửi của giao thức
ESP32** tạo. Bo thật đã được kiểm tra vận hành ký/gửi/ACK, nhưng demo không
khai thác firmware ESP32, Wi-Fi/Windows hay tấn công web dashboard. REST là
bước đối chiếu dữ liệu được lưu phía sau gateway.

Để demo trên chính gateway 8885 và thấy bốn ô bảo mật của dashboard thật tăng:

```powershell
.\tools\Test-LiveSecurity.ps1
```

Script dùng danh tính tạm, cách ly mẫu demo khỏi dữ liệu vận hành và khởi động
lại riêng gateway lúc thêm/thu hồi danh tính; bo tự kết nối lại. Các counter
audit tăng thật và được giữ sau demo. Xem hướng dẫn chi tiết được liên kết dưới.

Để chạy hồi quy trên gateway riêng 8886 và giữ device thử cho bạn xem trên web:

```powershell
.\tools\Test-Security.ps1 -Demo
```

Theo [hướng dẫn demo từng bước](DEMO_BAO_MAT_VA_KIEM_THU.md) để biết các
tình huống, giá trị mong đợi, bằng chứng và cách kết thúc bài thử.

Tài liệu demo tách rõ đăng nhập MQTT, HMAC và sequence/session. Việc loại
gói trước chuyển tiếp giúp bảo vệ lịch sử dữ liệu, không chỉ giao diện.
Không suy ra hệ thống chống được gói giả mới sau khi lộ cả mật khẩu và khóa.
Phương án máy ảo dùng chung CA được mô tả ở đó, nhưng chưa có runner từ xa.

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
$env:TEMP = "$PWD\.private\tmp"
$env:TMP = $env:TEMP
.\.venv\Scripts\python.exe -m unittest discover -s tests/security -v
.\.venv\Scripts\python.exe .\tests\security\run_mqtt_integration.py --thingsboard
```

Bài thử tích hợp dùng device test, cổng 8886 và database riêng, không reset
phiên thật của ESP32. Nó kiểm tra TLS, xác thực MQTT, ACL, HMAC bị sửa,
khóa sai, gói trùng, số thứ tự xung đột, timestamp cũ và phiên cũ. Khi thêm
`--thingsboard`, nó đọc lại telemetry qua REST để xác nhận chỉ mẫu hợp lệ
được lưu; device test được xóa khi kết thúc. Log và báo cáo ở `.private/mqtt_test_*`.

Không dùng công cụ test để tự chèn giá trị giả vào device vận hành.

## 10. Tra lỗi

| Hiện tượng | Kiểm tra |
| --- | --- |
| Không mở được web | Chờ server khởi động; xem trạng thái compose và log; kiểm tra RAM |
| Guard WAIT nhưng Wi-Fi nối được | PC/gateway đang bật, IP đúng, UDP 123 lấy giờ, TCP8885 tới được |
| Wi-Fi nối nhưng Time WAIT, MQTT OFFLINE | Kiểm tra PC và bo cùng mạng, IP SNTP khớp IP PC hiện tại; firmware chờ lấy giờ trước khi mở MQTT TLS |
| Không đồng bộ giờ | Đồng hồ PC đúng; UDP 123 không bị tường lửa chặn và không bị dịch vụ khác chiếm |
| TLS lỗi | CA/IP/hostname/hạn chứng chỉ và đồng hồ; không tắt xác minh TLS |
| Guard READY nhưng TB seq không tăng | ThingsBoard đang chạy, token đích khớp, xem gateway-error.log |
| BAD_MAC | Khóa/key ID trên bo và server phải cùng bản; không in khóa ra log |
| SESSION_INVALID sau restart | Bo sẽ xin phiên mới; nếu kéo dài kiểm tra kết nối/hello trong log |
| DUPLICATE | Gói giống bản đã nhận, không chuyển tiếp lần hai; thường là retry QoS |
| Không có mắt/miệng/PERCLOS | Kiểm tra các cờ valid và khả năng nhìn thấy mặt, độc lập với HMAC |
| COM bị chiếm | Đóng viewer/monitor/trình ghi trước khi flash hoặc mở viewer khác |
| cc1plus out of memory | Dừng server tạm thời, build một job; không fullclean không cần thiết |

Log gateway: `.private\runtime\gateway-error.log`. Đọc không chứa khóa:

```powershell
Get-Content .\.private\runtime\gateway-error.log -Tail 30
```

Nếu Windows chặn kết nối tới Python, chỉ cho phép đúng Python đang dùng
và đúng các cổng TCP8885/UDP 123 trên mạng sử dụng; không tắt toàn bộ firewall.
Start-Local dùng Python có sẵn được tìm qua PATH và thư viện gateway ở D.

## 11. Chia sẻ Git và giới hạn bảo vệ

Git giữ mã nguồn, trọng số mắt/miệng và hướng dẫn. `.private`, sdkconfig,
`.venv`, build/build_guard, managed_components, video và khóa không được commit.
Clone mới phải tự sinh khóa/chứng chỉ/token và cấu hình Wi-Fi của mình.

VERIFIED nghĩa thông điệp vượt qua HMAC/phiên/thứ tự/độ mới; không có nghĩa
model mắt/miệng chính xác tuyệt đối hoặc người trước camera là người thật.
Bản này không chống firmware bị chiếm quyền hoặc khóa/token đích bị lộ.
Đọc [cơ chế từng lớp và từng file](KE_HOACH_CHONG_GIA_MAO.md) để hiểu chi tiết.

Tài liệu chính thức: [Docker Desktop Containers](https://docs.docker.com/desktop/use-desktop/container/),
[ThingsBoard Docker](https://thingsboard.io/docs/installation/docker/).
