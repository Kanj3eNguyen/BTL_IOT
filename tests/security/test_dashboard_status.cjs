/* Run with the existing Node.js; no browser/package installation required. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../server/thingsboard_local/dashboard_status.js'), 'utf8');
let now = 1800000000000;
const element = () => ({children: [], textContent: '', className: '', appendChild(child) {this.children.push(child);},
  set innerHTML(value) {this.children = [];}});
const grid = element(), live = element(), age = element();
const root = {ownerDocument: {createElement: element}, querySelector: name => ({'.dm-grid': grid, '.dm-live': live, '.dm-age': age}[name])};
const self = {ctx: {$container: [root], data: [], settings: {securityOnly: false}}};
vm.runInNewContext(source, {self, Date: {now: () => now}, setInterval: () => 1, clearInterval() {}});
function render(data) {
  self.ctx.data = Object.entries(data).map(([name, value]) => ({dataKey: {name}, data: [[now, value]]}));
  self.onDataUpdated();
  return Object.fromEntries(grid.children.map(card => [card.children[0].textContent, card.children[1].textContent]));
}
const data = {last_verified_at: now, integrity_status: 'VERIFIED', driver_present: true, eyes_valid: true,
  eyes_closed: false, mouth_valid: true, mouth_open: false, perclos_valid: true, perclos_window_ready: true,
  perclos: .15, blink_count: 9, yawn_count: 2, eye_closed_duration: 0, alert_reasons: 'Bình thường'};
let cards = render(data);
assert.equal(Object.keys(cards).length, 16);
assert.equal(cards['PERCLOS · 60 giây'], '15.0 %');
assert.equal(cards['Trạng thái mắt'], 'Mở mắt');
assert.equal(cards['Xác thực gói MQTT'], 'Đã xác thực');
cards = render({...data, eyes_valid: false, mouth_valid: false, perclos_valid: false});
assert.equal(cards['Trạng thái mắt'], 'Không quan sát được');
assert.equal(cards['Trạng thái miệng'], 'Không quan sát được');
assert.equal(cards['PERCLOS · 60 giây'], 'Chưa đủ dữ liệu');
now += 30000;
cards = render(data);
assert.equal(cards['Xác thực gói MQTT'], 'Dữ liệu cũ');
assert.equal(cards['Nhận diện khuôn mặt'], 'Chưa có dữ liệu mới');
assert.equal(live.className, 'dm-live dm-live-wait');
self.ctx.settings.securityOnly = true;
cards = render({...data, guard_rejected_count: 3, guard_duplicate_count: 2});
assert.equal(Object.keys(cards).length, 4);
assert.equal(cards['Gói bị từ chối'], '3');
assert.equal(cards['Gói trùng đã bỏ qua'], '2');
console.log('Dashboard: 16 live cards, validity, PERCLOS scaling, stale-data expiry and 4 security cards passed.');
