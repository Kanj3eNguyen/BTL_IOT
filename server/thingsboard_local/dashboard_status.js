/* Native ThingsBoard latest widget; displays verified telemetry, no inference. */
self.onInit = function () {
    self.dmTimer = setInterval(function () { render(); }, 1000);
    render();
};
self.onDataUpdated = function () { render(); };
self.onResize = function () { render(); };
self.onDestroy = function () { if (self.dmTimer) clearInterval(self.dmTimer); };
self.typeParameters = function () { return {maxDatasources: 1, singleEntity: true}; };

function render() {
    var root = self.ctx.$container[0];
    var grid = root.querySelector('.dm-grid');
    if (!grid) return;
    var data = {};
    (self.ctx.data || []).forEach(function (item) {
        if (item.data && item.data.length) data[item.dataKey.name] = item.data[item.data.length - 1][1];
    });
    function yes(v) { return v === true || v === 'true' || v === 1 || v === '1'; }
    function number(v, decimals) {
        return v !== null && v !== undefined && v !== '' && isFinite(Number(v)) ? Number(v).toFixed(decimals || 0) : '—';
    }
    var measured = Number(data.last_verified_at || 0);
    var age = measured ? Math.max(0, (Date.now() - measured) / 1000) : null;
    var fresh = age !== null && age <= 20;
    var verified = fresh && data.integrity_status === 'VERIFIED';
    var present = fresh && yes(data.driver_present);
    var eyes = present && yes(data.eyes_valid);
    var mouth = present && yes(data.mouth_valid);
    var perclos = fresh && yes(data.perclos_valid);
    var ready = perclos && yes(data.perclos_window_ready);
    var mask = Number(data.alert_reasons_mask || 0);
    var banner = root.querySelector('.dm-live');
    banner.textContent = verified ? 'ĐANG NHẬN DỮ LIỆU ĐÃ XÁC THỰC' : measured ? 'DỮ LIỆU CŨ · KIỂM TRA KẾT NỐI' : 'ĐANG CHỜ DỮ LIỆU TỪ BO';
    banner.className = 'dm-live ' + (verified ? 'dm-live-ok' : 'dm-live-wait');
    root.querySelector('.dm-age').textContent = age === null ? 'Chưa có mẫu xác minh' : 'Cập nhật ' + Math.floor(age) + ' giây trước · Chu kỳ gửi 5 giây';
    grid.innerHTML = '';
    function card(label, value, note, state) {
        var element = root.ownerDocument.createElement('div');
        element.className = 'dm-card dm-' + (state || 'neutral');
        [['dm-label', label], ['dm-value', value], ['dm-note', note || '']].forEach(function (part) {
            var node = root.ownerDocument.createElement('div');
            node.className = part[0]; node.textContent = String(part[1]); element.appendChild(node);
        });
        grid.appendChild(element);
    }
    if (self.ctx.settings.securityOnly) {
        card('Gói bị từ chối', number(data.guard_rejected_count), 'Tổng quyết định từ chối tại gateway', 'warn');
        card('HMAC không hợp lệ', number(data.guard_bad_mac_count), 'Nội dung hoặc khóa không khớp', Number(data.guard_bad_mac_count) ? 'danger' : 'ok');
        card('Phiên / sequence bị loại', number(data.guard_replay_rejected_count), 'Không tự kết luận có tấn công', 'warn');
        card('Gói trùng đã bỏ qua', number(data.guard_duplicate_count), 'Không tạo thêm một mẫu dữ liệu', 'neutral');
        return;
    }
    card('Nhận diện khuôn mặt', !fresh ? 'Chưa có dữ liệu mới' : present ? 'Có mặt' : 'Vắng mặt',
         yes(data.face_holdover) ? 'Tọa độ mặt đang được giữ tạm' : 'Quan sát camera trên ESP32-S3', present ? 'ok' : 'warn');
    card('Cảnh báo hiện tại', fresh ? (data.alert_reasons || 'Chưa xác định') : 'Chưa có dữ liệu mới', 'Ngưỡng được đánh giá trên bo', fresh && mask ? 'danger' : fresh ? 'ok' : 'warn');
    card('Trạng thái mắt', !eyes ? 'Không quan sát được' : yes(data.eyes_closed) ? 'Nhắm mắt' : 'Mở mắt', 'Chỉ kết luận khi eyes_valid', eyes ? (yes(data.eyes_closed) ? 'warn' : 'ok') : 'neutral');
    card('Thời gian nhắm mắt', eyes ? number(data.eye_closed_duration, 1) + ' s' : '—', 'Cảnh báo từ 1,5 giây', eyes && Number(data.eye_closed_duration) >= 1.5 ? 'danger' : 'neutral');
    card('PERCLOS · 60 giây', perclos ? number(Number(data.perclos) * 100, 1) + ' %' : 'Chưa đủ dữ liệu',
         'Cảnh báo ≥15% khi cửa sổ đã sẵn sàng', ready && Number(data.perclos) >= .15 ? 'danger' : ready ? 'ok' : 'warn');
    card('Chớp mắt trong cửa sổ', number(data.blink_window_count), 'Cửa sổ đếm 60 giây', 'neutral');
    card('Tổng chớp mắt', number(data.blink_count), 'Bộ đếm trong lần khởi động hiện tại', 'neutral');
    card('Tổng số lần ngáp', number(data.yawn_count), 'Ngáp trong 60s: ' + number(data.yawn_window_count) + ' · Ngưỡng 3 lần', fresh && Number(data.yawn_window_count) >= 3 ? 'warn' : 'neutral');
    card('Cửa sổ PERCLOS', ready ? 'Sẵn sàng' : fresh ? 'Đang thu thập' : 'Chưa có dữ liệu mới',
         'Thời gian quan sát hợp lệ: ' + number(data.perclos_observed_s, 1) + ' s', ready ? 'ok' : 'warn');
    card('Khả năng quan sát mắt', eyes ? 'Hợp lệ' : 'Không đủ dữ liệu', 'Không phải độ chính xác của mô hình', eyes ? 'ok' : 'neutral');
    card('Trạng thái miệng', !mouth ? 'Không quan sát được' : yes(data.mouth_open) ? 'Há miệng' : 'Đóng miệng', 'Chỉ kết luận khi mouth_valid', mouth ? 'ok' : 'neutral');
    card('Xác thực gói MQTT', verified ? 'Đã xác thực' : fresh ? 'Chưa xác thực' : 'Dữ liệu cũ', 'HMAC · Phiên · Sequence · Độ mới', verified ? 'ok' : 'warn');
    card('Sequence đã xác minh', number(data.auth_sequence), 'Theo phiên của gateway', 'neutral');
    card('Boot ID', data.boot_id === undefined ? '—' : data.boot_id, 'Đổi khi ESP32 khởi động lại', 'neutral');
    card('Thời gian chạy bo', data.uptime_ms === undefined ? '—' : number(Number(data.uptime_ms) / 1000) + ' s', 'Tính từ lần khởi động ESP32', 'neutral');
    var lighting = {OK: 'Ổn định', ADJUST: 'Đang điều chỉnh', DARK: 'Thiếu sáng', BRIGHT: 'Quá sáng'};
    card('Ánh sáng camera', fresh ? (lighting[data.lighting_state] || data.lighting_state || 'Chưa xác định') : 'Chưa có dữ liệu mới',
         'Độ sáng: ' + (data.face_luma !== null && data.face_luma !== undefined ? number(data.face_luma) : number(data.scene_luma)) + '/255', 'neutral');
}
