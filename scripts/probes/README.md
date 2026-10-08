# NX Controller hardware probes

Các công cụ trong thư mục này phục vụ các cổng kiểm chứng phần cứng trước khi
xây lại NX Controller. H0/H1 là chỉ đọc hoặc chỉ đổi input daemon trong RAM;
H2 có công cụ lifecycle USB riêng và luôn cần phục hồi có giới hạn.

**Cập nhật 2026-10-04:** các script `brickpro-usb-out-live.sh`,
`brickpro-usb-coordinator-cycle.sh`, `brickpro-usb-out-recovery.sh` bị khóa với
exit 2 vì live recovery đã thất bại. Không chạy chúng trên thiết bị.
`brickpro-functionfs-out-probe.c` cũng khóa live mặc định; không dựng với
`NX_ENABLE_UNVERIFIED_USB_OUT_LIVE` trước khi giải quyết lỗi bind và có đường
log/recovery độc lập (SSH trên Wi-Fi tin cậy). Không coi fixture pass là giấy
phép chạy lại USB transition.

Kernel thật đã được đối chiếu bằng Image trong boot + một đoạn code/bảng
endpoint RAM chỉ đọc: chỉ một endpoint interrupt, không có bulk→interrupt
fallback. Probe hai interrupt IN/OUT không thể bind trên firmware này.
Hướng một interrupt IN + control `SET_REPORT` đã được kiểm chứng ngày
2026-10-04 bằng HID vendor-defined lab trên Windows và đúng firmware đang chạy.
Windows đọc neutral input, `HidD_SetOutputReport` và `WriteFile` đều tạo
`SET_REPORT` 32 byte trên ep0; payload sai bị ứng dụng thiết bị bỏ qua. Đây là
bằng chứng cho transport output qua control, **không phải** bằng chứng DS4,
Steam/game hay rung thật.

## Phạm vi an toàn

- `brickpro-controller-audit.sh` chỉ đọc thông tin kernel, USB gadget, evdev,
  Bluetooth, motor, màn hình, pin và process.
- `brickpro-input-probe.c` chỉ mở evdev để đọc; không dùng `EVIOCGRAB`, không
  inject event và không ghi thiết bị.
- `brickpro-input-ab.sh` chỉ chạy binary 8 ms đã khóa checksum từ `/tmp`, rồi
  phục hồi daemon stock bằng cơ chế daemon nền của hệ thống. Đây là công cụ
  thử nghiệm có thay đổi process tạm thời, không phải inventory chỉ đọc.
- Các probe H0/H1 không bind/unbind UDC, đổi radio, chạy motor, tắt màn hình,
  đọc Bluetooth link key hoặc sửa dữ liệu trên thẻ nhớ.
- `brickpro-usb-owner-probe.c --selftest` chỉ dùng file giả và các tiến trình con
  do nó tạo. Kiểm thử cả worker/guardian chết, PID/start-time và link tương đối/
  tuyệt đối; không chạm USB/input/motor/radio/thẻ.
- `brickpro-usb-owner-probe.c --pause-only` và `--unbind` là bài thật, không phải
  inventory: giữ dừng đúng ADB instance 300 ms, có guardian độc lập và giữ log
  trong `/tmp`. Bản đã đo cho phép unbind/rebind nguyên ADB/MTP; không chứng minh
  đổi descriptor/function hoặc USB OUT an toàn.
- `inspect-adbd-usb.py` phân tích chỉ đọc bản sao stock ADB, không thực thi/patch
  binary. Nó chứng minh `usb_ffs_open_thread` ghi lại UDC sau khi mở endpoints.
- `inspect-kernel-usb.py` chỉ phân tích bản sao boot/kallsyms/config và các
  snapshot nhỏ đã thu chỉ đọc. Nó không tự mở `/dev/mem`, chạy kernel, patch
  RAM/firmware hay ghi lên thẻ. Địa chỉ snapshot phải được đối chiếu Image,
  `/proc/iomem` và firmware checksum trước khi thu; không dùng địa chỉ của
  firmware khác. Cờ claimed không phải dấu hiệu endpoint rảnh sau bind/reset.
- `brickpro-functionfs-out-probe.c` giữ identity và link ADB/MTP, thêm tạm HID
  IN/OUT; chưa bind thành công. Lần live trả -524, rồi cleanup so sánh literal
  absolute/relative symlink sai nên bỏ sót function. Đã lấy log qua SSH, dọn
  đúng function tạm và bind lại USB không reboot. Sửa canonical-link chỉ có
  regression tests, chưa là bằng chứng live OUT đạt.
- `brickpro-functionfs-control-probe.c` là spike thay thế: một interrupt IN,
  Output report qua ep0, không input thật/motor/radio/SD. Binary live chỉ được
  dựng có chủ đích cho boot ID đã audit; mặc định source khóa live. Probe có
  guardian, khóa restore riêng, exact PID/start-time và phục hồi link/identity.
  `windows-hid-control-probe.c` chỉ mở đúng serial lab duy nhất, xác minh
  VID/PID/version/usage/report lengths rồi mới gửi hai mẫu đã biết. Lượt live-02
  đạt `output_mask=3`, lifecycle/restore tức thời đều 0; hậu kiểm lúc đó thấy lại
  đúng Composite/ADB/WPD stock. Một hậu kiểm muộn hơn thấy toàn bộ USB Code 45;
  chưa biết cáp bị rút hay UDC tự mất, nên lifecycle dài hạn vẫn chưa đạt. Lượt
  live-01 phát hiện hai lỗi harness (timeout dùng
  chung cờ dừng và tạo ConfigFS link bằng target tương đối); đã phục hồi thủ
  công qua SSH không reboot, sửa source và vượt lại 12 fault fixtures trước
  live-02. Không dùng probe này như launcher sản phẩm.

Stock ADB trên firmware đã thử không trả exit code của remote shell cho PC.
Mỗi harness phải thu `probe_rc`/journal và hậu điều kiện riêng; host ADB exit 0
không đủ xác nhận bài test pass. Không đọc Bluetooth link key vào báo cáo.

Regression inspector có thể chạy bằng
`python scripts/tests/test-kernel-usb-inspection.py EVIDENCE_DIRECTORY` với
thư viện Capstone đã có. Nó cần bộ snapshot H2 tương ứng, không tự thu dữ liệu
hay đụng thiết bị; các negative test chỉ đổi byte trong mock trên PC. Không
đưa boot image hoặc snapshot kernel RAM vào ZIP phát hành ứng dụng.

## Binary đã chuẩn bị

`../../.local/brickpro-input-probe-aarch64` là ELF AArch64 static được build từ
source hiện tại.

SHA-256:

```text
637807641F4DF0A1C3D52EBEDEF570DC60ACB66245CE7C3B9678B056F876BB6A
```

Khi ADB kết nối, binary chỉ được chép vào `/tmp`. Chạy ít nhất 20 giây trong lúc
xoay đủ vòng cả hai cần và bấm mọi điều khiển vật lý. Không dùng USB Storage hay
USB Gamepad trong lúc thu inventory; giữ máy ở chế độ ADB/MTP thông thường.

Mọi bài đo cần thao tác tay phải chờ người dùng bấm **Bắt đầu kiểm tra**, sau
đó mới đếm một giây và bắt đầu. Bài tự động chỉ cần thông báo rồi tự đếm một
giây; không gọi người dùng. Kết quả không có thao tác cần thiết phải bị loại,
không được diễn giải thành lỗi input.

`brickpro-detached-launch.c` tồn tại vì BusyBox `start-stop-daemon -b` trên
firmware này không tách tiến trình khỏi terminal ADB. Launcher native thực hiện
double-fork, `setsid()`, bỏ SIGHUP và chuyển stdin/stdout/stderr sang
`/dev/null` trước khi chạy H2 orchestrator. Smoke test đã xác nhận tiến trình
có PPID 1, session riêng, không có tty, sống qua HUP và hoàn tất. H2 không còn
dùng action `usb_null`; launcher vẫn cần thiết vì ConfigFS unbind làm ADB tạm
thời biến mất.

Kết quả chỉ là bằng chứng phần cứng/runtime của đúng phiên firmware đang chạy.
Nó chứng minh generic HID control-output trên Windows cho identity lab, nhưng
không tự chứng minh Steam/game, codec DS4, USB rumble hay Bluetooth rumble;
các phần đó vẫn thuộc các cổng H2-H4 riêng.
