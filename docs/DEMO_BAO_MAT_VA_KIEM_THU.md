# Demo bảo mật MQTT và kiểm thử hệ thống

Mục tiêu: chứng minh bản tin hợp lệ được ghi vào ThingsBoard; bản tin bị
sửa, sai khóa, phát lại hoặc sai phiên bị từ chối trước khi chuyển tiếp.
Đây là kiểm thử thực tế qua TLS/MQTT và REST ThingsBoard local, không chỉ
in kết quả giả định. Không cần train hoặc nạp lại bo để chạy bài thử này.

**Cách gọi đúng:** kiểm thử chống sửa/chèn/phát lại telemetry ở gateway.
Gói tấn công do client Python mô phỏng bên gửi trên laptop tạo; ESP32 thật
tiếp tục vận hành ký/gửi mẫu hợp lệ. Không gọi là tấn công đã chiếm ESP32
hay tấn công dashboard. Đọc REST ThingsBoard là đối chiếu dữ liệu sau gateway,
không phải bài khai thác lỗ hổng web/database.

Client thử có tài khoản MQTT riêng: sửa payload/sai HMAC kiểm tra lớp chữ ký
sau khi vào broker; các ca sequence/thời gian dùng khóa thử đúng để kiểm tra
lớp freshness/replay độc lập. Không giả định đã phá TLS để bắt gói trên Wi-Fi,
không brute-force khóa và không gửi khai thác vào firmware ESP32.

## Demo trực tiếp trên dashboard đang dùng — chọn cách này khi trình bày

Có thể xem đồng thời viewer tổng hợp bằng `tools/Start-Viewer.ps1 -Port COM4`.
Tab Audit gateway và bốn counter đọc dữ liệu local của gateway; ô sequence
đã lưu đọc REST ThingsBoard và hiển thị tuổi mẫu. Counter local có thể đổi
trước dashboard, vì dashboard chờ bản tin hợp lệ tiếp theo của bo. Dữ liệu
cũ phải mang nhãn CŨ; chữ VERIFIED cũ không phải bằng chứng đang nhận mới.

Để các ô **Gói bị từ chối / HMAC không hợp lệ / Phiên-sequence / Gói trùng**
trên dashboard thật tăng ngay trong buổi demo, chạy:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Start-Local.ps1
.\tools\Test-LiveSecurity.ps1
```

1. Mở dashboard **BTL IoT · Giám sát tài xế**, xem bốn ô bảo mật và chụp số
   hiện tại trước lượt thử.
2. Script thêm danh tính demo tạm vào gateway thật **8885**, khởi động lại
   riêng gateway để nạp cấu hình. Bo tự nối lại; ThingsBoard/PostgreSQL và
   viewer không bị tắt.
3. Nhấn Enter ở console để bắt đầu. Các gói thật được gửi vào chính gateway
   này: mẫu chuẩn cách ly, gói trùng, sửa PERCLOS, sai khóa, tái dùng sequence,
   timestamp cũ và phiên cũ.
4. Đợi mẫu hợp lệ kế tiếp của ESP32. Gateway gắn bộ đếm thật vào mẫu đó,
   chuyển ThingsBoard; script đọc REST để xác nhận dashboard đã có giá trị mới.
5. Script chờ Enter để bạn xem/chụp dashboard. Nhấn Enter cuối để thu hồi
   danh tính demo và khởi động lại gateway về danh sách thiết bị bình thường.

Mỗi lượt thành công làm tăng tối thiểu:

| Ô dashboard thật | Mức tăng do bài demo |
| --- | ---: |
| Gói bị từ chối | +5 |
| HMAC không hợp lệ | +2 |
| Phiên / sequence bị loại | +2 |
| Gói trùng đã bỏ qua | +1 |

Các số là **bộ đếm tích lũy**, không tự trở về 0 sau demo. Gói trùng của bo
thật có thể làm ô cuối tăng thêm ngoài +1. Lượt kiểm chứng đầu tiên đã đổi
`0 / 0 / 0 / 14` thành `5 / 2 / 2 / 15`.

Script không POST số counter vào ThingsBoard hoặc sửa counter bằng SQL.
Các quyết định HMAC/replay của gateway thực sự làm tăng bộ đếm; chính mẫu
thật kế tiếp đưa chúng lên dashboard. Danh tính demo có cờ **demo_only do
quản trị viên cấu hình trên server**: mẫu chuẩn được xác minh và lưu với
trạng thái `demo`, không vào hàng đợi chuyển tiếp. Vì vậy thử gói trùng vẫn
qua bộ xác minh thật mà blink/yawn/PERCLOS giả không lọt vào device vận hành.
Cờ do client gửi trong payload không thay đổi chính sách này.

Đọc bằng chứng mới nhất:

```powershell
Get-Content .\.private\live_security_latest.json
```

Báo cáo ghi cổng 8885, bộ đếm trước/sau, các giá trị đã đọc lại từ ThingsBoard
và số mẫu demo đã chuyển tiếp phải bằng 0. Khóa tạm/backup cấu hình nằm trong
`.private/live_security_...`; không chiếu file `identity.json`/`before-config.json`.

Chạy không chờ Enter:

```powershell
.\tools\Test-LiveSecurity.ps1 -NoPause
```

Nếu tắt cưỡng bức cả terminal/PC giữa chừng, danh tính tạm có thể chưa thu hồi.
Dùng đúng thư mục lượt đó để dọn rồi nạp lại danh sách thiết bị:

```powershell
.\.venv\Scripts\python.exe .\tests\security\demo_live_gateway.py cleanup `
  --folder 'D:\ESP32S3_Driver_Monitor_Ready\.private\live_security_THU_MUC_CUA_LUOT_DO'
.\tools\Restart-Gateway.ps1
```

Các mục bên dưới mô tả **bộ kiểm thử tách riêng cổng 8886** để hồi quy giao
thức và kiểm tra lưu dữ liệu. Cách đó không làm counter dashboard thật tăng.

## 1. Chuẩn bị trước buổi demo

1. Cắm nguồn bo đã nạp firmware local. PC và bo dùng đúng mạng đã cấu hình;
   IP PC phải khớp địa chỉ gateway/SNTP và chứng chỉ trên bo.
2. Mở Docker Desktop; dự án nằm trong **Containers → driver-monitor-local**.
3. Mở PowerShell ở gốc repo, bật server:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Start-Local.ps1
```

4. Mở [ThingsBoard local](http://localhost:8080). Tài khoản riêng của máy
   nằm trong `.private\thingsboard\DANG_NHAP.txt`; không chiếu mật khẩu.
5. Mở dashboard **BTL IoT · Giám sát tài xế**, chọn khoảng thời gian
   **Realtime – last 1 hour**. Xác nhận dữ liệu thật có `VERIFIED` và sequence
   tăng; một chữ VERIFIED từ hôm trước chưa chứng minh thiết bị đang gửi.
6. Có thể mở stream ở một cửa sổ PowerShell riêng:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Start-Viewer.ps1 -Port COM4
```

Viewer cần `WiFi: CONNECTED`, `Time: OK`, `Guard: READY`, `TB seq` tăng.
Giữ viewer mở được trong lúc kiểm thử vì bài thử bảo mật không mở COM4.
Muốn xác nhận nguồn dữ liệu, bạn chớp mắt/há miệng và quan sát số đo của bo;
độ chính xác nhận diện là bài kiểm thử khác với tính toàn vẹn MQTT.

## 2. Những thành phần được dùng khi demo

| Thành phần | Địa chỉ/vai trò |
| --- | --- |
| ESP32 thật | Gửi gateway thật ở IP PC, TLS cổng 8885 |
| Gateway thử | Chỉ lắng nghe `127.0.0.1:8886`, database riêng |
| Thiết bị ThingsBoard thật | `ESP32S3 Driver Monitor - Verified` |
| Thiết bị ThingsBoard thử | Tên ngẫu nhiên `MQTT security test xxxxxxxx` |
| ThingsBoard | Web `localhost:8080`, MQTT nội bộ `127.0.0.1:18884` |
| Dữ liệu mẫu của bài thử | blink=2, yawn=0, PERCLOS=0.1, sequence=1 |

Thiết bị thử dùng khóa HMAC và mật khẩu MQTT riêng, sinh cho mỗi lượt.
Gateway thử có MQTT client ID riêng để tránh tranh chấp kết nối upstream
với gateway thật. Chương trình không dùng phiên hoặc khóa của bo làm mục
tiêu tấn công. Các số liệu mẫu chỉ được đưa vào device thử.

## 3. Chạy demo có hướng dẫn

Mở PowerShell thứ hai tại gốc repo:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
.\tools\Test-Security.ps1 -Demo
```

Chương trình tự chọn Python `.venv` của gateway, không phụ thuộc Python
đang kích hoạt trong terminal ESP-IDF. Không tự cài thư viện mới.

Trình tự trên màn hình:

1. Chạy **24 unit test**, kết thúc bằng `OK`.
2. Tạo gateway thử và device thử; in tên chính xác của device.
3. Dừng ở lời nhắc **Nhấn Enter để chạy các tình huống**.
4. Bạn mở ThingsBoard → **Entities → Devices** (hoặc **Devices** ở menu),
   tìm tên `MQTT security test ...` vừa in → mở → **Latest telemetry**.
   Lúc này chưa có số liệu là đúng: bài thử chưa gửi bản tin hợp lệ.
5. Quay lại PowerShell, nhấn Enter. Các tình huống chạy tự động và in
   `[PASS 01]` đến `[PASS 12]` cùng giải thích tiếng Việt.
6. Cuối bài thử chương trình lại chờ Enter. **Chưa nhấn Enter lúc này**:
   giữ device thử để xem/chụp bằng chứng trên ThingsBoard.

Không cần sửa file nguồn, chép token hoặc tự gõ payload chứa khóa khi demo.
Chương trình tự tạo và gửi các gói kiểm thử thật.

## 4. Từng tình huống và cách giải thích

| Mốc | Thao tác chương trình thực sự làm | Kết quả mong đợi | Điều được chứng minh |
| --- | --- | --- | --- |
| 01 | Nối TLS bằng CA của server, dùng tài khoản MQTT thử và hello có HMAC | `TLS_AND_AUTHENTICATED_SESSION` | Kết nối hợp lệ và nhận được session đã ký |
| 02 | Gửi mẫu sequence=1, blink=2, PERCLOS=0.1 với HMAC đúng | `ACCEPTED` | Bộ xác minh nhận mẫu hợp lệ vào outbox |
| 03 | Đọc lại device thử qua REST | `REAL_THINGSBOARD_PERSISTED` | Dữ liệu thực sự đã được ThingsBoard lưu, không chỉ nhận MQTT PUBACK |
| 04 | Gửi lại nguyên byte bản tin vừa gửi | `DUPLICATE` | Không tạo thêm một bản chuyển tiếp từ retry/phát lại giống hệt |
| 05 | Giải mã payload, đổi PERCLOS 0.1 thành 0.99, giữ MAC cũ | `BAD_MAC` | Nội dung bị sửa không vượt qua HMAC |
| 06 | Ký bản tin bằng khóa HMAC sai | `BAD_MAC` | Mật khẩu MQTT đúng chưa đủ để tạo dữ liệu được xác minh |
| 07 | Dùng khóa thử đúng nhưng tái sử dụng sequence=1 với blink=999 | `REPLAY_OR_SEQUENCE_CONFLICT` | HMAC hợp lệ vẫn phải vượt qua kiểm tra sequence |
| 08 | Ký gói có timestamp cũ 60 giây | `STALE_OR_FUTURE` | Loại bản tin quá cũ |
| 09 | Xin phiên mới, rồi gửi lại mẫu thuộc phiên trước | `SESSION_INVALID` | Gói cũ không dùng được trong phiên mới |
| 10 | Gửi vào downlink và topic ThingsBoard trực tiếp | `PUBLISH_ACL` | Client không được gửi qua đường topic trái quyền; bộ xác minh không nhận thêm mẫu |
| 11 | Thử kết nối bằng mật khẩu MQTT sai | `WRONG_MQTT_PASSWORD_REJECTED` | Broker từ chối client sai thông tin xác thực |
| 12 | Kiểm tra outbox, bộ đếm chuyển tiếp, latest và lịch sử ThingsBoard | `ATTACKS_NOT_PERSISTED_TO_THINGSBOARD` | Các gói sai không được lưu thành số liệu mới |

Các dòng `[PASS]` chỉ được in sau khi điều kiện kiểm thử đã đạt. Nếu có
sai khác, script dừng với lỗi thay vì tiếp tục báo thành công.

Gói trùng có thể do MQTT QoS 1 gửi lại bình thường. `DUPLICATE` là quyết
định loại bản trùng, không tự kết luận có kẻ tấn công. Bài thử chủ động
gửi gói đó để chứng minh hệ thống không ghi thêm bản mới.

## 5. Chứng minh trên ThingsBoard

Khi chương trình đang chờ Enter ở cuối, quay lại device thử → Latest telemetry,
nhấn Refresh nếu cần. Phải còn các giá trị:

| Key | Giá trị |
| --- | --- |
| `integrity_status` | `VERIFIED` |
| `auth_sequence` | `1` |
| `blink_count` | `2` |
| `yawn_count` | `0` |
| `perclos` | `0.1` (tương ứng 10%, vì telemetry dùng thang 0–1) |

Sau thao tác sửa gói, không xuất hiện PERCLOS `0.99` và blink `999`.
Bài thử còn đọc lịch sử qua REST và yêu cầu **chỉ một điểm** cho mỗi key
`auth_sequence` và `perclos` trong khoảng thử, không chỉ nhìn giá trị latest.

Trong báo cáo/console phải có:

```text
ACCEPTED = 1
FORWARDED = 1
DUPLICATE = 1
BAD_MAC = 2
REPLAY_OR_SEQUENCE_CONFLICT = 1
STALE_OR_FUTURE = 1
SESSION_INVALID = 1
```

`SESSION=2` là hai lần cấp phiên hợp lệ, không phải lỗi. Kiểm tra mật khẩu
và ACL diễn ra ở broker nên không phải mọi lần bị chặn đều có counter trong
bộ xác minh HMAC.

Counter tấn công của device **thật** không tăng do bài thử này. Counter
trong mẫu đầu tiên của device thử cũng được chụp trước các lần tấn công.
Để trình bày các quyết định loại gói, dùng console và báo cáo/log gateway
thử; không chờ dashboard thật hiện counter của bài thử tách riêng.

## 6. Bằng chứng nên lưu

Chụp ba màn hình:

1. Dashboard/stream thật: trạng thái kết nối và sequence đang tăng.
2. Console: các dòng PASS và counter sau các tình huống tấn công.
3. Device thử: latest telemetry vẫn sequence=1, blink=2 và PERCLOS=0.1.

Báo cáo mới nhất nằm ở:

```powershell
Get-Content .\.private\security_latest.json
```

Mỗi lượt còn có thư mục `.private\mqtt_test_YYYYMMDD_HHMMSS` chứa
`result.json`, `gateway.log`, database và audit log của gateway thử.
`result.json` và console không in mật khẩu/khóa. **Không chiếu hoặc đưa
`config.json` trong thư mục thử lên Git** vì nó chứa credentials.

Để xem log đúng lượt vừa chạy:

```powershell
$report = Get-Content .\.private\security_latest.json -Raw | ConvertFrom-Json
Get-Content (Join-Path $report.report_folder 'gateway.log') -Tail 40
```

Muốn chạy tự động không chờ Enter, ví dụ trước buổi báo cáo:

```powershell
.\tools\Test-Security.ps1 -Demo -NoPause
```

Trong chế độ này device thử được xóa ngay khi kết thúc; chỉ còn báo cáo
local. Khi terminal không có stdin tương tác, chương trình cũng bỏ qua
các lời nhắc Enter để tránh bị treo.

## 7. Kết thúc demo

Quay lại console và nhấn Enter ở lời nhắc cuối. Chương trình dừng gateway
thử ở cổng 8886 và xóa device thử trên ThingsBoard. Gateway thật, dashboard
thật, viewer và số liệu thiết bị thật tiếp tục hoạt động. Báo cáo trên D
được giữ để đối chiếu.

Nếu dừng cưỡng bức cả terminal/PC, việc dọn có thể chưa hoàn tất. Kiểm tra
đúng device có tên `MQTT security test ...` và đúng tiến trình gateway với
đường dẫn config thuộc `.private/mqtt_test_...`; đừng tắt/xóa device vận hành.

## 8. Bài unit test chứng minh thêm điều gì?

Ngoài demo qua mạng, bộ unit test kiểm tra: giữ mốc sequence khi restart
gateway, reset bo phải dùng phiên mới, hello retry không hạ sequence,
hello cũ không khôi phục phiên đã nghỉ, phiên hết hạn, timestamp ở tương
lai, tuổi phép đo, JSON trùng key/sai cú pháp, cờ valid/PERCLOS và ngăn
client tự chèn `integrity_status`. Các tình huống thời gian được mô phỏng
bằng clock kiểm thử; không cần chờ 5 phút hoặc chỉnh giờ Windows để demo.

Unit test không tương đương thử mất điện thực tế hoặc chứng nhận bảo mật.
Kiểm thử tích hợp dùng MQTT client Python tạo thông điệp theo giao thức của
bo; phần này không phải firmware bị tấn công trực tiếp qua UART.

## 9. Những điều nên nói đúng khi trình bày

Bạn có thể nói:

> Hệ thống kiểm tra HMAC, phiên, sequence và độ mới trước khi chuyển
> telemetry sang ThingsBoard. Trong các tình huống thử, bản tin hợp lệ
> được lưu còn bản tin sai bị loại; đã kiểm tra cả REST và lịch sử lưu.

Không gọi `VERIFIED` là chứng minh người thật hoặc model nhận diện chính
xác tuyệt đối. Bản này không chống ảnh in/video giả, không có cảm biến
nhịp tim/SpO₂ và chưa bật Secure Boot/mã hóa flash/eFuse.

Nếu người tấn công lấy được khóa, họ có thể ký một mẫu giả với phiên và
sequence mới; các kiểm tra trên không xác minh được sự thật của phép đo.
NTP bootstrap chưa có xác thực riêng. Kiểm thử hiện tại không phải kiểm
thử tải/DoS, Wi-Fi hay một cuộc pentest toàn bộ PC/ThingsBoard. Đây là phạm
vi bảo vệ và bằng chứng cụ thể, không phải tuyên bố hệ thống an toàn tuyệt đối.

Xem thêm [cơ chế và từng file](KE_HOACH_CHONG_GIA_MAO.md) và
[hướng dẫn local A–Z](CHAY_LOCAL_A_Z.md).

## 10. Tách ba lớp kiểm thử để giải thích khi demo

Nếu localhost:8080 báo connection refused, ThingsBoard chưa chạy hoặc chưa
sẵn sàng: khởi động Docker Desktop/Start-Local và thử lại sau khi web mở
được. Lỗi này không phải kết quả BAD_MAC hay bằng chứng gói giả đã bị chặn.
Khi chỉ cần kiểm tra TLS/MQTT/verifier mà chưa có ThingsBoard, có thể dùng
gateway thử ở chế độ verify-only từ gốc repo:

```powershell
cd D:\ESP32S3_Driver_Monitor_Ready
$env:PYTHONPATH = "$PWD;$PWD\.venv\Lib\site-packages"
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:TEMP = "$PWD\.private\tmp"
$env:TMP = $env:TEMP
.\.venv\Scripts\python.exe .\tests\security\run_mqtt_integration.py
```

Cách này chạy 10 kiểm tra mạng/protocol, không chuyển dữ liệu vào ThingsBoard
và không chứng minh việc lưu REST/history. Bộ đầy đủ 12 kiểm tra cần
`--thingsboard` (hoặc Test-Security.ps1). Báo cáo ghi danh sách checks thực
sự đã chạy; không lấy kết quả verify-only để báo đủ 12 kiểm tra.

| Lớp | Cách tạo tình huống | Bằng chứng và giới hạn |
| --- | --- | --- |
| Xác thực MQTT | Gửi yêu cầu kết nối bằng mật khẩu sai | Broker từ chối. Có trong bộ hồi quy 8886, không phải ca của live demo 8885 và không bắt buộc tăng counter verifier. |
| HMAC | Đăng nhập bằng danh tính thử hợp lệ; sửa PERCLOS nhưng giữ MAC cũ hoặc ký bằng khóa sai | BAD_MAC tăng; mẫu đó không được chuyển tiếp. Giả định bên gửi đã qua lớp đăng nhập nhưng không tạo được chữ ký đúng. |
| Sequence/session | Dùng khóa thử đúng; gửi lại nguyên mẫu, ký nội dung khác với sequence cũ, hoặc gửi lại mẫu sau khi đổi phiên | DUPLICATE / REPLAY_OR_SEQUENCE_CONFLICT / SESSION_INVALID; không thêm mẫu chuyển tiếp. Khóa thử được cấp có chủ đích để lớp HMAC không che mất kết quả kiểm tra replay. |

Đây là các giả định kiểm thử độc lập về chức năng. Không khẳng định ba lớp
lưu bí mật độc lập: sdkconfig và config gateway hiện chứa cả mật khẩu lẫn
khóa HMAC. Người đọc trọn các file này có thể lấy cả hai. Khi có cả quyền
MQTT và khóa HMAC, người giả mạo có thể xin phiên và ký gói mới với sequence
lớn hơn. Sequence/session bảo vệ chống phát lại, không xác thực sự thật của
camera sau khi danh tính thiết bị đã bị sao chép.

Không kết luận chỉ từ việc dashboard đứng yên: kiểm tra lý do ở gateway,
outbox (script chỉ đọc để đối chiếu), danh tính và lịch sử ThingsBoard trong
khoảng thử. Gói bị loại không tạo mẫu mới; audit và counter từ chối vẫn được
lưu. Counter là tích lũy, phải so trước/sau chứ không mong về 0 sau mỗi lượt.

## 11. Máy ảo cùng LAN: phương án mở rộng, chưa có runner từ xa

Có thể dùng máy ảo làm client thử qua LAN để tách bên gửi và bên nhận rõ
hơn. Với mạng Bridged, chọn card LAN/Wi-Fi đang nối gateway và xác nhận máy
ảo tới được IP laptop/cổng TCP 8885. Khả năng bridge qua Wi-Fi và khả năng
hai thiết bị liên lạc còn tùy nền tảng, hotspot/router và cách ly client.
Ví dụ kiểm tra trong máy ảo Windows, thay IP bằng IP laptop thực tế:

```powershell
Test-NetConnection -ComputerName 192.168.1.186 -Port 8885
```

Chỉ kết quả TcpTestSucceeded chưa chứng minh TLS, tài khoản hoặc HMAC đúng.
Máy ảo dùng CA công khai hiện có `.private/mqtt_guard/ca.pem`; không phải
tạo CA mới cho mỗi client. CA giúp xác minh gateway, không cấp quyền MQTT,
không ký được telemetry và không thay mật khẩu/khóa HMAC. Không chép
ca-key.pem, server-key.pem hoặc tài khoản quản trị ThingsBoard sang VM.

Để thử lớp HMAC/replay, nên cấp danh tính demo_only riêng và thu hồi sau
lượt; tài khoản mới phục vụ cách ly dữ liệu, không phải yêu cầu của TLS.
Dùng danh tính thật của ESP32 có thể giành client ID, thay phiên hoặc ảnh
hưởng số liệu. Không dùng chung danh tính thật trong buổi demo vận hành.

**Trạng thái mã hiện tại:** demo_live_gateway.py kết nối localhost và đọc
SQLite/REST trên laptop; run_mqtt_integration.py tự chạy gateway thử local.
Chưa có runner máy ảo, chưa có lệnh export bundle hay bộ đối chiếu từ xa.
Không chép nguyên `.private` hoặc chạy nguyên script local trên VM. Cần
tách phần gửi MQTT ở VM và phần đối chiếu/thu hồi ở laptop trước khi có
thể báo cáo đã kiểm thử qua hai máy. Các kết quả hiện có là thử trên laptop.

TLS mã hóa đường truyền. Việc client chủ động sửa JSON rồi gửi qua kết nối
TLS của chính nó kiểm tra tầng ứng dụng; không chứng minh đã giải mã/sửa
thành công kết nối TLS ESP32 đang dùng. Replay ở đây là dùng lại thông điệp
ứng dụng, không phải phát lại các gói TCP/TLS thô bắt được trên mạng.
