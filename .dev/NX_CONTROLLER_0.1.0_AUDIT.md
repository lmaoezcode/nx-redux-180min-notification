# NX Controller 0.1.0 — kiểm chứng kế hoạch trước khi xây lại

Ngày kiểm tra: 2026-10-03–04

Commit NX Redux dùng làm mốc: `f869dc5b0a191a34012f423a0173781ea75b5a6a`

Trạng thái: **H0/H1 đạt; giới hạn endpoint H2 đã xác minh bằng boot + RAM;
generic HID một interrupt-IN + control `SET_REPORT` đã đạt trên Windows;
phục hồi tức thời đạt nhưng sau đó toàn bộ USB biến mất, nguyên nhân chưa chốt;
codec DS4/rung game thật chưa thử; H3–H4 chưa thực hiện**.

Tài liệu này không tuyên bố các tính năng mới đã hoạt động. Nó phân biệt bốn loại thông tin:

- **Đã chứng minh:** có bằng chứng từ source hiện tại, số đo đã lưu, tài liệu nhà sản xuất hoặc tài liệu upstream chính thức.
- **Đã chứng minh có điều kiện:** đúng trong một phạm vi cụ thể, không được suy rộng.
- **Mục tiêu/quyết định thiết kế:** điều dự án muốn đạt, không phải sự thật về phần cứng.
- **Chưa chứng minh hoặc sai:** phải đo lại, làm spike, hoặc loại khỏi kế hoạch.

## 1. Kết luận kiểm tra

Kế hoạch cũ có hướng kiến trúc đúng, nhưng **không được triển khai nguyên trạng**. Năm điểm phải sửa trước:

1. Không phát hành bằng VID/PID của Hori, Sony hoặc Microsoft. USB-IF quy định mỗi VID thuộc quyền sử dụng độc quyền của đơn vị được cấp và cấm dùng trái phép cả VID đã cấp lẫn chưa cấp. `0f0d:00c1`, `054c:09cc` và `045e:02fd` chỉ được coi là danh tính thử nghiệm riêng, không phải danh tính sản phẩm.
2. “DS4-compatible” không tự động đồng nghĩa Windows, SDL hoặc Steam sẽ nhận thiết bị khi nó dùng một VID/PID hợp lệ của NX Controller. Các nhánh HIDAPI thường nhận dạng loại tay cầm từ danh sách thiết bị/nhà sản xuất đã biết. Đây là cổng tương thích bắt buộc, chưa phải nền tảng đã chứng minh.
3. FunctionFS hỗ trợ nhiều endpoint ở API, nhưng kernel đang chạy chỉ cung cấp một endpoint interrupt hai hướng và matcher không cho dùng bulk thay interrupt. Descriptor DS4 kiểu interrupt IN + interrupt OUT riêng không thể bind qua autoconfiguration hiện tại; bỏ yêu cầu đó khỏi kế hoạch cho firmware này. Hướng một interrupt IN + output `SET_REPORT` qua ep0 đã chạy thật với Windows cho HID lab; chưa chứng minh codec DS4, Steam/game hay rumble. Không sửa kernel hay dùng ghi RAM để lách giới hạn.
4. Padpod chứng minh một hướng Bluetooth khả thi trên Brick Pro chạy spruceOS, không chứng minh nó hoạt động trên NX Redux. Con số 10 ms là lựa chọn phòng thủ dựa trên phép đo của tác giả Padpod; nó cần được đo lại trên radio/BlueZ của máy này. Kế hoạch “lưu bốn host” không có bằng chứng và bị loại khỏi `0.1.0`.
5. B16 là chỉ số của **Standard Gamepad mapping trên Web**, không phải vị trí bit chung của mọi giao thức. Core phải lưu nút vật lý `HOME`; từng codec mới ánh xạ nó thành Guide/PS/Home. Không thiết kế state nội bộ theo số B của trình duyệt.

Do đó, tên **NX Controller** và SemVer `0.1.0` được giữ, nhưng DS4 chỉ được chốt làm giao thức chính sau khi vượt các cổng về danh tính, nhận dạng host, output report và phục hồi hệ thống.

## 2. Bảng kiểm từng nhóm thông tin

| Thông tin trong kế hoạch | Kết quả | Bằng chứng / sửa lại |
|---|---|---|
| Brick Pro dùng A133P, Linux 4.9, USB 2.0, BT 4.2 + EDR 2.1, hai cần Hall JF15 và một motor rung | Đã chứng minh | Trang sản phẩm chính thức của TrimUI ghi rõ các mục này. Đây vẫn là thông số marketing; model driver/chip và topology thực tế sẽ được đọc lại trên máy. |
| L2/R2 là cò số | Đã chứng minh trên firmware đang chạy | Evdev khai báo chúng là `ABS_Z/ABS_RZ` dải 0–255, nhưng phép đo phần cứng ngày 2026-10-04 chỉ nhận `0` và `255`. Core lưu trạng thái số 0/1; codec cần trường analog chuyển hai đầu mút thành 0/255. Không quảng cáo 256 mức. |
| 52 Hz là giới hạn phần cứng | Sai, đã loại | A/B trực tiếp ngày 2026-10-04 đo daemon stock đạt 49,5–52,4 cập nhật/giây trên từng trục, còn binary 8 ms đạt 88,6–93,8 Hz/trục. Vì vậy khoảng 52 Hz chủ yếu là nhịp của input daemon stock, không phải trần cố định của joystick, UDC hay USB. |
| Brick Pro đạt tối đa 100 Hz | Chưa chứng minh | A/B mới chỉ chứng minh đường evdev với binary 8 ms đạt gần 90–94 cập nhật/giây trên từng trục. Chưa biết trần ADC/I2C thật sự và chưa đo host-visible/end-to-end latency. |
| Giữ calibration hiện có và không thêm deadzone mặc định lần nữa | Đúng với backend evdev hiện tại, cần đo runtime | NX đã có quy trình calibration I2C ghi min/max/center/deadzone cho `trimui_inputd`; `usbgamepad` hiện chỉ scale range evdev, không thêm deadzone riêng. Core mới nhận canonical state đã hiệu chỉnh; profile mặc định phải là identity/zero-extra-deadzone. Tùy chọn deadzone/curve là transform có chủ đích sau đó, không được âm thầm áp lần hai. |
| Bản vá input 8 ms là backend chính thức | Không phù hợp để chốt | Ba binary có checksum xác định và 8 ms là thí nghiệm hữu ích, nhưng đây vẫn là patch binary không có source sở hữu. Chỉ dùng có điều kiện, chạy tạm trong RAM, đúng checksum và phục hồi stock; backend lâu dài cần được sở hữu/kiểm toán được. |
| Hàng đợi tối đa đúng 32 cạnh nút | Tham số thiết kế chưa được chứng minh | Giữ nguyên tắc latest-state cho analog và journal riêng cho cạnh nút, nhưng giới hạn phải có cả số phần tử lẫn tuổi tối đa. Bắt đầu với 32 chỉ là giá trị thử; fault test quyết định con số cuối. Overflow phải neutral/resync thay vì phát backlog cũ. |
| USB 4 ms và Bluetooth 10 ms là tốc độ đã chốt | Mục tiêu khởi đầu | Log hiện tại cho thấy userspace gửi khoảng 237–238 report/s lúc idle, phù hợp gần 4 ms nhưng không nói host nhận state mới nhanh như vậy. Bluetooth 10 ms đến từ phép đo Padpod. Cả hai phải được sweep; USB còn phải mã hóa `bInterval` đúng theo speed. |
| Tăng polling gây lỗi USB/MTP/UMS | Chưa có bằng chứng | Các lỗi USB đã thấy trùng với luồng chuyển mode/phục hồi còn rủi ro. Không có phép thử cô lập chứng minh 4 ms hoặc 8 ms gây lỗi nhận USB. Không tiếp tục lặp lại quan hệ nhân quả này. |
| Padpod là bằng chứng gần phần cứng nhất | Đã chứng minh có điều kiện | Commit ghim `0c37f1f...` được viết riêng cho Brick Pro, có PC/Xbox/PS4 Bluetooth và xử lý rumble. README nói rõ phát triển/kiểm thử trên spruceOS; build stock dùng cùng binary và chưa được thử. Không có chứng cứ NX Redux. |
| XR829 buộc mọi chế độ Bluetooth chạy 10 ms | Suy rộng sai | Source Padpod ghi phép đo khoảng 118 report/s khi PC đồng thời phát audio và thấy hàng đợi tăng ở 125/s cộng report theo thay đổi. Đây là phép đo upstream, không phải định luật phần cứng. Dùng 10 ms làm điểm bắt đầu an toàn rồi sweep có đo queue age/p95/p99. |
| Bluetooth lưu tối đa bốn host | Không có bằng chứng, đã loại | Padpod lưu một host cuối cho mỗi mode. `0.1.0` chỉ cần một host gần nhất cho mỗi identity đã được chứng minh; multi-host chuyển sang backlog. |
| Bluetooth audio chắc chắn không thể chạy cùng controller | Chưa chứng minh, sửa thành chính sách | Việc takeover `bluetoothd` có thể làm gián đoạn BlueALSA và plugin input giữ PSM HID. `0.1.0` có thể chủ động tạm dừng audio với cảnh báo và phục hồi chính xác, nhưng không được gọi đó là giới hạn phần cứng trước khi thử coexistence. |
| Bluetooth controller không ảnh hưởng Wi-Fi | Chưa chứng minh; có cảnh báo ngược lại trong NX | Source NX tg5040 ghi nhận `bluetoothd+hciattach` làm Wi-Fi giảm từ khoảng 330 KB/s xuống 2 KB/s trên xradio combo. Đây là phép đo lịch sử trong source, chưa có raw report ở audit này. H3 phải đo Wi-Fi/BT coexistence và chọn chính sách cảnh báo hoặc tạm dừng Wi-Fi có phục hồi. |
| FunctionFS có thể cấp interrupt IN + OUT riêng và ep0 cho DS4 trên kernel hiện tại | Hai interrupt endpoint bị bác bỏ; một IN + control OUT đã đạt | Boot image, config runtime, code RAM và bảng endpoint RAM khớp: chỉ một `ep4-int`; matcher yêu cầu `type_int`, không fallback bulk. Spike vendor-defined một IN đã enumerate; Windows gửi hai Output report qua ep0 và thiết bị nhận đúng. Đây chưa phải DS4 USB/rung. |
| NX Controller chắc chắn xin được PID từ pid.codes | Chưa chứng minh | pid.codes yêu cầu repo source công khai và license OSS/OSHW; dự án software chạy trên phần cứng thương mại có thể bị yêu cầu giải trình thêm. Không chọn trước một PID chưa được cấp và không mô tả pid.codes như chứng nhận USB-IF. |
| Có thể quay lại `hid.usb0` configfs | Bị bác bỏ trên firmware đã thử | Log thực tế cho thấy tạo `functions/hid.usb0` thất bại. Source/tài liệu cũ cũng cho thấy nhánh configfs HID không sẵn sàng. Phải dò lại config kernel hiện tại; FunctionFS vẫn là hướng mặc định cho spike. |
| Native USB XInput có thể làm qua FunctionFS hiện tại | Ngoài phạm vi, chưa khả thi | Parser FunctionFS Linux 4.9 có giới hạn descriptor class mà đường XUSB không khớp. Kế hoạch đúng khi không hứa native USB XInput; nếu theo hướng này sẽ là dự án kernel/gadget riêng. |
| DS4 USB và Bluetooth chỉ khác transport | Sai, đã loại | Linux ghi USB input `0x01/64 byte`, USB output `0x05/32 byte`; Bluetooth input/output `0x11/78 byte`, cùng CRC và feature report khác đường USB. Phải là hai codec/framing path được test riêng dù dùng chung canonical state. |
| Điền trung tính cho gyro/touchpad là đủ để không quảng cáo chúng | Chưa đúng | Nếu giả VID/PID Sony, host có thể mặc định coi là tay Sony chính hãng và bật capability tương ứng. Với tay bên thứ ba, SDL có capability report riêng, nhưng việc probe vẫn phụ thuộc nhận dạng vendor/type. UI trung thực không giải quyết được nhận dạng của host. |
| Steam Input biến mọi tay generic thành XInput và trả rung đầy đủ | Sai khi nói tuyệt đối | Valve mô tả emulation input thành Xbox cho game. Microsoft phân biệt XInput/DirectInput và đường vibration của XInput. Không có bằng chứng Steam sẽ gửi rumble trở lại mọi generic HID tự tạo. Phải test rung bằng game thật cho từng identity/transport. |
| Home phải là B16 trong state nội bộ | Sai, đã sửa | State lưu `HOME` vật lý. Web Standard Gamepad có thể trình bày nó là B16; DS4/Xbox/generic HID có vị trí/report khác. |
| F1 đổi layout và F2 tắt màn hình là kiến trúc phím tắt cuối cùng | Quá cứng, đã sửa | Đây là hành vi hiện có đã dùng được, không phải hệ thống hiện đại. Bản xây lại dùng action router theo ngữ cảnh và cho phép cấu hình tap/hold/double/chord. |
| Tắt màn hình chắc chắn tiết kiệm được một mức pin cụ thể | Chưa đo | Người dùng đã xác nhận F2 tắt/bật panel mà controller vẫn chạy. Mức tiết kiệm, nhiệt và ảnh hưởng sạc chưa có phép đo điện năng; chỉ giữ đây là tính năng và mục tiêu đo, không quảng cáo con số. |
| Có thể hiển thị pin/sạc và điều khiển LED trạng thái | Phần cứng có, integration chưa chứng minh | TrimUI xác nhận pin và nhiều vùng RGB; NX có battery/LED code. H0 phải xác nhận node runtime và owner. Protocol battery report, LED màu/nhịp và restore chính xác phải test riêng; không để app tranh LED với OSD/power state. |
| Brick Pro có rung điều khiển qua GPIO 227 và node voltage | Đã chứng minh trong NX source, cần xác nhận runtime | NX hiện có backend `/sys/class/motor/voltage` + GPIO 227 và master switch Motor. Tài liệu `BRICKPRO.md` nói cap 2,5 V đã lỗi thời; code hiện cap Brick Pro ở 1,7 V, còn `keymon` vẫn pulse 2,0 V. Cần hợp nhất thành một motor service và đo trên máy. |
| Lấy motor mạnh hơn trong hai motor host | Quyết định thiết kế, không phải sự thật | Hợp lý làm default cho motor đơn, nhưng phải cho phép mixer/scale và giới hạn an toàn dùng chung. Không chép PWM GPIO của spruceOS sang NX Redux. |
| `0.1.0-alpha.1`, `beta.1`, `rc.1` đúng SemVer | Đã chứng minh | Cú pháp prerelease đúng SemVer. Quy tắc “breaking config trong 0.x tăng MINOR” là chính sách riêng của dự án, không phải yêu cầu bắt buộc của SemVer. |
| Mục tiêu ≥90 Hz USB, ≥80 Hz Bluetooth | Mục tiêu kiểm thử | Đây là ngưỡng sản phẩm, không phải thông số phần cứng. Giữ làm acceptance target nhưng không dùng để mô tả khả năng trước khi đo. |

## 3. Những gì source NX hiện tại thực sự đã làm tốt

Không nên vứt bỏ toàn bộ dự án cũ. Các phần sau là bằng chứng/fixture có giá trị:

- FunctionFS HID một endpoint IN đã enumerate và hoạt động với Windows trong profile PC.
- Tìm `TRIMUI Player1` theo tên/capability thay vì cố định `event3` và đã xử lý `SYN_DROPPED`.
- Mapping D-pad, hai cần, L2/R2 số và Home đã có regression tests.
- F1 đổi Xbox/Nintendo chỉ thay mapping; F2 điều khiển màn hình; MENU+SELECT thoát.
- Có thống kê tách `source_frames` và USB reports, là nền tốt để mở rộng telemetry.
- NX đã có motor backend, master switch, strength profile và worker bảo đảm tắt motor khi thoát.
- `nx_usb_mode.sh` đã bắt đầu gom logic MTP/UMS và có kiểm tra an toàn tốt hơn stock.

Nhưng các phần này chỉ nên được giữ làm **oracle/test**, không bắt buộc giữ cấu trúc monolith hiện tại:

- `usbgamepad.c` đang trộn input, descriptor, USB lifecycle, màn hình, rumble và UI state.
- Gadget hiện chỉ khai báo một endpoint IN; chưa thể nhận output report từ host.
- Cùng `bInterval=4` được dùng cho full-speed và high-speed. Theo USB, ở full-speed đó là 4 ms; ở high-speed giá trị 4 biểu diễn `2^(4-1)` microframe, tức 1 ms. Phải chọn theo speed, không coi cùng số là cùng chu kỳ.
- Profile PC đang dùng VID/PID Hori `0f0d:00c1`; profile cũ dùng `0525:a4ad`. Danh tính phát hành chưa được giải quyết.
- Phục hồi thường gọi `/bin/setusbconfig adb`, không phục hồi chính xác trạng thái USB trước phiên.
- Các hằng số motor đang lệch nhau giữa tài liệu, platform và keymon.

## 4. Cổng kiểm chứng phần cứng bắt buộc

### Phát hiện bổ sung từ thẻ đang gắn, chỉ đọc

Ngày 2026-10-03, Windows nhận thẻ ở `G:`. Không file nào trên thẻ bị sửa trong lần kiểm tra này.

- Bản đang cài là launcher `polling-test-v12-20261002`, dùng input daemon 8 ms có kiểm tra MD5, profile PC/HORIPAD và FunctionFS IN-only.
- Log có các phiên 8 ms và 4 ms cũ. Khi có chuyển động, `source_frames`, số thay đổi từng axis, `encoded_changes`, `usb_state_changes` và `usb_reports` khác nhau rất lớn; lúc đứng yên vẫn có khoảng 237–238 USB report/s. Một đoạn 8 ms ghi `source_frames=144 Hz`, tổng từng axis khoảng 143, `usb_state_changes=98 Hz`, `usb_reports=310 Hz`; đoạn khác ghi `source_frames=224,7 Hz`, từng axis 45–67 Hz, `usb_state_changes=163,8 Hz`, `usb_reports=363 Hz`. Các số này không phải polling rate đầu-cuối và không thể được dùng thay cho GamepadLA. Chúng xác nhận cần tách metric theo tầng.
- Có nhiều `write error: Device or resource busy`, và một số phiên FunctionFS đã bind nhưng UDC không attach nên `usb_reports=0`. Đây là bằng chứng lifecycle/UDC còn race hoặc ownership conflict; chưa chỉ ra thủ phạm duy nhất.
- Log USB Storage lúc 00:41 ghi `umount: can't unmount /mnt/SDCARD: Device or resource busy`, nhưng luồng cũ vẫn tiếp tục sang `usb_device.sh` và filesystem check. Check trong phiên đó báo `files corrupted 40, files fixed 0`; cuối luồng mount lại cũng báo `Device or resource busy`. Đây là log trước wrapper mới. Báo cáo `.local/usb-recheck-20261003/USB-RECHECK.md` ghi lần CHKDSK chỉ-đọc về sau exit 0 và không thấy lỗi filesystem, nên không được nói thẻ hiện vẫn hỏng.

Kết luận an toàn: **không chạy USB Storage thêm và không chạy benchmark ghi lớn trước khi sửa xong luồng thoát**. Không tự chạy repair trên bản duy nhất của thẻ; sao lưu dữ liệu quan trọng trước các thử nghiệm tiếp theo. Log chứng minh workflow cũ không fail-closed và filesystem từng được báo có lỗi metadata; nó không chứng minh workflow đó tạo ra mọi lỗi, cũng không chứng minh lỗi còn tồn tại sau đợt phục hồi. Wrapper mới đã thêm chốt đầu vào nhưng báo cáo audit vẫn chỉ ra launcher cha còn giữ current directory trên SD và thứ tự thoát `fsck`/unbind/mount chưa an toàn.

Các lỗi GPIO 360 trong log thuộc `usb_device.sh` stock và là thêm một dấu hiệu script stock không khớp hoàn toàn phần cứng hiện tại. Bản xây lại không được coi `setusbconfig`/`usb_device.sh` là transaction đáng tin mà không kiểm tra hậu điều kiện.

Không bắt đầu viết DS4 hoàn chỉnh trước khi năm cổng nhỏ sau có kết quả.

### H0 — inventory chỉ đọc

Chạy `scripts/probes/brickpro-controller-audit.sh` qua ADB. Script chỉ đọc:

- kernel config/module và `usb_f_*`/FunctionFS;
- UDC, speed, trạng thái, gadget hiện hành và endpoint/mount;
- tên/capability input, phiên bản/hash input daemon;
- chip/driver Bluetooth, BlueZ, hciattach, BlueALSA và process ownership;
- node motor, backlight, LED, battery;
- trạng thái MTP/UMS/ADB và mount.

Nó không bind/unbind USB, không bật/tắt radio, không rung, không tắt màn hình và không đọc link key.

### H1 — input vật lý

Build rồi chạy `scripts/probes/brickpro-input-probe.c` từ `/tmp`, không `EVIOCGRAB` và không ghi input. Trong ít nhất 20 giây người thử xoay cả hai cần đủ vòng và bấm mọi phím. Cổng đạt:

- binary AArch64 static đã chuẩn bị: `.local/brickpro-input-probe-aarch64`;
- SHA-256 probe v2: `637807641F4DF0A1C3D52EBEDEF570DC60ACB66245CE7C3B9678B056F876BB6A`;
- trước khi chạy trên máy phải kiểm tra lại checksum này; chỉ chép tạm vào `/tmp` và xóa theo reboot, không thay daemon hay file firmware;
- xác nhận chính xác key/ABS capability, min/max/fuzz/flat/resolution;
- xác nhận L2/R2 được khai báo là axis nhưng chỉ phát hai đầu mút 0/255, tương đương cò số;
- xác nhận HOME, F1/F2, volume, L3/R3 và không có control ẩn;
- đo `SYN_REPORT`, event từng trục, `SYN_DROPPED` và khoảng frame;
- lặp với stock và binary 8 ms đúng checksum, không cài vĩnh viễn.

Kết quả H0/H1 trên máy thật ngày 2026-10-04:

- Kernel `4.9.191`; `CONFIG_USB_F_FS=y`, `CONFIG_USB_CONFIGFS_F_FS=y` nhưng
  `CONFIG_USB_CONFIGFS_F_HID` bị tắt. Vì vậy lỗi tạo `hid.usb0` đã được giải
  thích bằng cấu hình kernel, không phải lỗi câu lệnh. FunctionFS vẫn cần H2.
- UDC `5100000.udc-controller` đang chạy high-speed; cấu hình bình thường có
  `ffs.adb` và `ffs.mtp`.
- `TRIMUI Player1` là uinput `045e:028e`, có bốn trục cần, D-pad hat,
  `ABS_Z/ABS_RZ`, Home, Menu, F1/F2, Volume, L3/R3 và các nút mặt/vai. VID/PID
  nội bộ này không phải danh tính USB phát hành của NX Controller.
- L2/R2 chỉ phát `0` và `255`; chúng là cò số dù được khai báo bằng ABS axis.
- Stock, 20,0116 giây xoay liên tục hai cần: 4.098 `SYN_REPORT`, tổng 204,8
  frame/s; từng trục 49,5–52,4 Hz; không `SYN_DROPPED`.
- Binary 8 ms đúng SHA-256, 20,0021 giây cùng thao tác: 7.297 `SYN_REPORT`,
  tổng 364,8 frame/s; từng trục 88,6–93,8 Hz; không `SYN_DROPPED`.
- Một thay đổi trục được phát trong một frame riêng, nên tốc độ tổng khoảng
  205/365 frame/s không phải polling rate mỗi trục. Con số so với GamepadLA là
  khoảng 50 Hz stock và 90–94 Hz ở binary 8 ms.
- Home đến evdev dưới dạng `KEY_HOMEPAGE`, đồng thời `keymon` dùng cạnh nhả để
  bật OSD. Controller context phải chuyển quyền Home có snapshot/restore; chỉ
  map Home trong HID report là chưa đủ.
- Bluetooth đang soft-block trong inventory; BlueZ là 5.78. H0 chưa bật radio
  nên không chứng minh controller/profile runtime.
- Motor node đang đặt 1,7 V, GPIO 227 ở 0; pin và nhiều kênh LED sysfs tồn tại.
- Thử hot-swap cho thấy khởi động daemon stock bằng tiến trình nền gắn với ADB
  có thể chết khi shell đóng. Supervisor phải dùng cơ chế daemon hóa thực sự
  và xác nhận cả liveness lẫn sự kiện input sau restore; chỉ thấy PID/event node
  là chưa đủ. Máy đã được reboot và xác nhận lại bằng đường boot stock trước
  phép đo stock hợp lệ ở trên.

### H2 — USB FunctionFS tối thiểu

Một binary lab riêng, chưa có UI/profile:

Trạng thái ngày 2026-10-04: **cổng parser/endpoint-file đã đạt**. Binary static
SHA-256 `076659F19D176820687DA47614F72EBB95FE7214B6F64CB229AED95D2C49B46B`
đã ghi descriptor HID FunctionFS gồm interrupt-IN và interrupt-OUT; kernel chấp
nhận, tạo/mở được `ep1` và `ep2`, trả mã 0, sau đó dọn mount/function sạch mà
UDC vẫn `configured` với ADB/MTP. Kết quả này chưa chứng minh endpoint được UDC
cấp khi bind, Windows enumerate, host ghi OUT packet hay game-driven rumble.

Binary live-probe hiện tại có SHA-256
`0703B8B90505AFD1EE2369D07FDC1466F48A95481FC2C0297486D803BF9CF719`;
mapper regression tests vẫn đạt. Nó thêm profile lab-only có IN 8 byte, OUT 32
byte và chỉ log tối đa 32 byte OUT dưới dạng hex; tuyệt đối chưa điều khiển motor.
Các lỗi đã xác định trong những probe đầu:

- Ghi UDC báo lỗi trong khi kernel đã log disconnect; sau đó UDC lại có tên.
  Không được diễn giải errno riêng lẻ thành trạng thái cuối, hoặc tự kết luận
  đó là unbind bất đồng bộ. Phải đọc hậu điều kiện và kiểm soát các writer.
- `bcdDevice=0xff01` bị ConfigFS bác vì không phải BCD hợp lệ; đã đổi thành
  `0x9901`. Đây là lỗi descriptor, không phải giới hạn endpoint.
- Probe dùng `usb_null` để khóa gadget đã làm mất USB và cần reboot. Giả định
  OTG manager là thủ phạm duy nhất tự bind ConfigFS **không có bằng chứng**.

Device-tree đọc trực tiếp ngày 2026-10-04 xác nhận `compatible` là
`allwinner,sunxi-otg-manager`, `usb_port_type=2` (OTG), `usb_detect_type=1`
(VBUS/ID detect), `usb_det_vbus_gpio="axp_ctrl"`. Tài liệu Tina Linux USB của
Allwinner xác nhận `otg_role` là node trạng thái, còn `usb_null`, `usb_device`
và `usb_host` là action node khi đọc. Các thông tin này không chứng minh cơ chế
auto-bind ConfigFS. Loại bỏ khẳng định trước rằng VBUS/OTG tự viết lại UDC:
binary ADB stock có một writer thực sự, như bằng chứng dưới đây. Không dùng
action đổi role làm khóa cấu hình gadget.

State machine hiện tại đã bỏ `usb_null` khỏi mọi đổi gadget: chỉ ghi rỗng
ConfigFS `UDC`, chấp nhận lỗi ghi của BSP chỉ sau khi đọc thành công và xác
nhận hậu điều kiện rỗng/`none`, rồi mới sửa function/link. `usb_device` chỉ
được kích như đường recovery nếu `otg_role` chưa là device hoặc UDC class chưa
sẵn sàng. Ma trận fixture wrapper/coordinator/diagnostic đã qua toàn bộ với
exit 0, trong đó có lỗi UDC không đọc được (phải fail closed), LUN bận, mount,
swap, backing sai và bind/ADB thiếu. Binary AArch64 build `-Werror` và mapper
regression test cũng đạt. Tuy nhiên fixture không mô phỏng writer trong ADB
stock nên **không chứng minh lifecycle thật đã an toàn**.

Lần H2 kế tiếp xác nhận một rủi ro độc lập ở tầng launcher: BusyBox
`start-stop-daemon -b` của firmware này chỉ đưa process ra nền, không tạo session
mới và không tách các file descriptor terminal ADB. Điều này cho phép HUP ảnh
hưởng orchestrator/watchdog khi ADB mất. Không coi HUP là nguyên nhân duy nhất
của mọi phiên mất USB nếu chưa thu được exit/signal của đúng tiến trình.

Launcher native `brickpro-detached-launch-aarch64` đã được dựng static với
SHA-256 `B196BEDDE80C11BB6BCCB398FE00007B01976861A83B72EAE2B2E8EAAD92B607`.
Nó double-fork, gọi `setsid()`, bỏ SIGHUP và chuyển ba standard descriptor sang
`/dev/null` trước khi exec. Smoke test không đổi USB đã xác nhận PPID 1, session
riêng, `tty_nr=0`, cả ba fd trỏ `/dev/null`, sống qua HUP và hoàn tất; launcher
này đã vượt smoke test; nó không tự bảo đảm transaction USB phục hồi được.

#### Bằng chứng ownership bổ sung ngày 2026-10-04

Các dữ liệu dưới đây ở `.local/controller-h2-evidence-20261004/`.

1. Chu kỳ shell 05:33:48: MTP → ADB thành công, ADB → MTP thất bại vì UDC
   còn bound. Watchdog 05:34:13 cũng thất bại, nhưng vẫn ghi “finished” và xóa
   token. Log này **không phải bằng chứng phục hồi thành công**. Boot ID mới
   `6932e5dc-b767-44c9-8f0c-024c20c94b4e` và kết nối ADB/MTP bình thường là sau
   reboot, không được ghi công cho watchdog. Xem `checkpoint-053348.txt` và
   `recovery-through-053417.txt`.
2. `/bin/adbd` lấy trực tiếp từ firmware đang chạy có SHA-256
   `E979F5C69A5C0547E83766814661FEF6C0BD129BD37B0E27ED86E05B944883DE`.
   Binary còn symbol. Trong `usb_ffs_open_thread`, sau khi mở ep0/ep1/ep2:
   `0x413e7c` lấy đường `/sys/kernel/config/usb_gadget/g1/UDC`, `0x413e88` gọi
   `open`, `0x413e98` lấy `g_udc_controller`, và `0x413ebc` gọi `write` với tên
   controller. `usb_ffs_kick` đóng endpoints, đặt fd = -1 và signal điều kiện
   mở lại. Đây là bằng chứng ADB stock tự bind sau reopen, không chỉ giữ fd.
   Script `scripts/probes/inspect-adbd-usb.py` chỉ đọc binary; không patch nó.
3. Probe native `brickpro-usb-owner-probe.c` chỉ giữ dừng đúng PID/start-time/
   executable của ADB, không `killall`, có guardian độc lập, log trong `/tmp`,
   không thay role/link/identity. Pause-only 300 ms đạt (`pause-01.log`).
4. Unbind nguyên gadget khi ADB dừng, giữ 300 ms đạt: ghi UDC trả 0, đọc xác
   nhận rỗng, 26 quan sát không có rebind; guardian bind lại tên cũ và resume
   ADB. UDC trở lại `configured`; ADB PID 1640 và MTP PID 2310 giữ nguyên;
   VID/PID `18d1:d002`, bcdDevice `0409`, hai link và boot ID không đổi
   (`unbind-01.log`). Windows báo USB Composite Device, ADB Interface và WPD
   NX Redux đều `OK`. Chưa đo truyền file MTP, không gọi đây là kiểm thử đầy đủ.
5. Chín fixture chỉ dùng file giả/tiến trình con trong RAM đạt, gồm worker/
   guardian chết sau pause/unbind và cleanup chỉ khi đã unbound
   (`guardian-selftest.txt`). Đây là fault test logic, **không chứng minh kernel
   không treo hoặc phục hồi được khi đổi functions thật**.

Kết luận có phạm vi: auto-bind trong ADB stock đã được chứng minh bằng binary;
pause ADB cho phép một unbind/rebind ngắn hoạt động trên đúng firmware này.
Không suy rộng thành lifecycle controller, DS4 hoặc mọi lỗi USB đã được sửa.

#### Live OUT mới: chưa đạt, không lặp lại trước khi có log độc lập

`brickpro-functionfs-out-probe.c` dùng descriptor/ep0 lab hiện có nhưng không
chạy main cũ, không đọc/inject input hay motor. Nó giữ hai link ADB/MTP, giữ
toàn bộ identity, thêm tạm một FunctionFS HID. Guardian thừa kế endpoints trước
khi sửa gadget. Interval lab được sửa riêng: full-speed 4, high-speed 6, đều
tương ứng 4 ms. Profile PC cũ chưa thay descriptor.

Binary đã thử có SHA-256
`AD5568EEDF6E5AAC4E97F31B340A15553614B6F64A686B218A22CBDEE3CFECE8`.
Windows writer hết 20 giây với `found=false timeout=true`
(`windows-out-01.txt`); ADB không trở lại sau thời hạn guardian. Windows chỉ
còn các instance lịch sử của Brick ở trạng thái `Unknown`, không có thiết bị
đang present để đọc mã lỗi hiện tại. **Không gán các lỗi descriptor cached cũ
cho bài này và không kết luận phần cứng thiếu endpoint OUT.**

Đã lấy log qua SSH tại IP do người dùng cung cấp, rồi sao chép raw log về
`ffs-out-01.log` khi ADB trở lại. Nó xác nhận:

- Lab bind trả errno 524; dmesg có `failed to start g1: -524`. Chưa gửi được IN
  report nào và chưa nhận OUT, không có bằng chứng game-driven rumble.
- Worker và guardian đều đã thoát, ADB đã resume, MTP còn chạy. Lỗi cleanup
  độc lập do ConfigFS trả relative symlink nhưng probe so sánh với literal
  absolute target. Link/function/mount lab còn sót; sau khi endpoints đóng,
  bind tiếp trả -19. `guardian_unavailable_fallback` cũng là tên log sai:
  guardian thực tế đã trả lỗi restore, không biến mất.
- Đã xác minh UDC rỗng và canonical target đúng function do probe tạo, giữ
  dừng đúng ADB PID/start-time trong transaction, tháo link, unmount/rmdir đúng
  FunctionFS lab, bind lại tên controller cũ rồi resume ADB. Không gọi setter
  stock, đổi role, reboot, UMS hoặc sửa file trên thẻ. Boot ID, ADB/MTP PID,
  identity và hai link trước phiên được giữ; UDC đã `configured` lại.

Source probe đã sửa canonical-link và tên lỗi, thêm regression absolute/
relative/foreign-target và cleanup retry idempotent. Probe OUT cùng các shell
live/cycle/recovery cũ vẫn bị khóa mặc định. Chưa chạy lại live sau sửa.

#### Endpoint kernel thật: giới hạn đã xác minh, không phải trần của mạch

Không dùng source BSP tham chiếu để kết luận thay cho máy thật. Đã xác minh
`/dev/by-name/boot` trỏ `/dev/mmcblk0p4`, 24 MiB, rồi dùng ADB pull chỉ đọc
phân vùng này. Không ghi boot, rootfs, RAM kernel hoặc thẻ nhớ.

- `brickpro-boot.img`: SHA-256
  `47F3B765086D2FD53EFBD672D23E01861C1D80722608709495309843332137AB`.
- Kernel là AArch64 Image không nén, 11.876.360 byte, load `0x40080000`.
  Config nhúng khớp byte-for-byte `/proc/config.gz` đang chạy; kallsyms cho
  `_stext` tại `0xffffff8008080800`.
- Đọc đúng 156 byte code `usb_gadget_ep_match_desc` từ **RAM kernel, chỉ đọc**,
  trong dải `Kernel code` đã đối chiếu `/proc/iomem`, không đọc MMIO. Code khớp
  Image; SHA-256 `33734CF92B9578C4FCDEE8492780EEC195B9AEC1F3A8719699D7E6DC7819CD15`.
- Đọc đúng 888 byte bảng bảy endpoint trong dải `Kernel data`, cộng hai
  scalar 8 byte của gadget ops/match callback. Name/ops/caps khớp Image;
  gadget ops trỏ `0xffffff800883a678`, `match_ep` là NULL, không có callback
  thay thế matcher. Chuỗi linked list runtime chứa đúng sáu endpoint cấp cho
  function, ep0 riêng; không có alias interrupt khác ngoài bảng. Không dump
  RAM tổng thể hoặc vùng key/pairing.

| Endpoint khai báo trong kernel đang chạy | Loại được matcher chấp nhận | Hướng | Max packet |
|---|---|---|---|
| ep0 | control | IN/OUT | 64 |
| ep1in-bulk, ep2in-bulk | bulk | IN | 512 |
| ep1out-bulk, ep2out-bulk | bulk | OUT | 512 |
| ep3-iso | isochronous | IN hoặc OUT | 1024 |
| ep4-int | interrupt | IN hoặc OUT, một allocation | 512 |

Khác Linux 4.9 upstream: tại `0xffffff80084a3cbc`, kernel này chỉ kiểm bit
`type_int` (bit 3), không kiểm `type_bulk` thay thế. `usb_ep_autoconfig_ss`
loại endpoint đã claimed và đặt claimed sau khi chọn. `sunxi_udc_probe` khởi
tạo đúng bảy endpoint. Vì vậy một descriptor cần hai allocation interrupt
riêng không thể nhận đủ endpoint kể cả bỏ ADB/MTP; với MTP còn bound, endpoint
interrupt đã có descriptor/driver data. Cờ claimed đọc ở runtime bằng 0 sau
composite bind/reset **không có nghĩa endpoint đang rảnh**.

`__ffs_func_bind_do_descs` của Image gọi `usb_ep_autoconfig` ở
`0xffffff80084aa2a0`; NULL đi tới `0xffffff80084aa2f4` trả `-0x20c`, tức -524.
Đây là ràng buộc cấp endpoint trực tiếp tương ứng với lỗi bind của probe,
không phải bằng chứng hỏng mạch, không phải lỗi polling, và không có nghĩa
interrupt OUT đơn lẻ hoặc output qua control không được hỗ trợ.

Nguồn tái lập: `scripts/probes/inspect-kernel-usb.py`,
`kernel-usb-disassembly.txt`, `kernel-live-endpoints.txt`,
`live-epmatch-code.bin`, `live-endpoints.bin`, `live-gadget-ops.bin`,
`live-match-ep.bin` trong thư mục evidence của H2.

**Thay đổi bắt buộc cho H2:** thử một interrupt IN, report descriptor vẫn khai
báo Output report, nhận dữ liệu `SET_REPORT` trên ep0. SDL2 ghi rõ
[`SDL_hid_write`](https://wiki.libsdl.org/SDL2/SDL_hid_write) dùng control endpoint
nếu không có OUT endpoint; bảng USB của
[HIDAPI](https://github.com/libusb/hidapi/wiki/Known-Issues) cũng mô tả fallback
`WriteFile` → `SET_REPORT`. Đây là bằng chứng hướng API host, **chưa chứng minh**
Windows/Steam/game nhận identity của chúng ta hoặc gửi rung thật theo DS4.
Main cũ chỉ consume/discard SET_REPORT, chưa có output decoder; không được
gọi đó là rung đã hoạt động. Không đổi endpoint bulk thành giả interrupt hay
patch kernel/RAM để né matcher. Kernel khác là hướng riêng cần scope mới.

MTP và controller không được cùng chiếm endpoint interrupt. Spike đã tháo tạm
MTP bằng snapshot/restore đúng daemon/link, giữ SSH độc lập, chỉ gửi neutral
input và không dùng USB Storage. Kết quả chi tiết ở mục ngay dưới.

1. Giữ identity thử nghiệm rõ ràng là lab-only — đã làm.
2. Một interrupt IN + Output report và `SET_REPORT` qua ep0 — đã đạt cho HID lab.
3. Windows xác nhận usage/report lengths; Brick Pro ghi setup/payload — đã đạt.
4. Neutral input và hai output packet giả lập — đã đạt; 100 chu kỳ chưa chạy.
5. High-speed thực tế đã xác nhận; sweep interval/speed và DS4 codec chưa chạy.

Control output generic đã được Windows dùng, nên giới hạn một endpoint không
còn tự nó chặn hướng DS4. DS4 vẫn phải vượt handshake/feature/output codec,
Steam/game và rung thật; không hạ tiêu chí stable. Bluetooth vẫn phải vượt
H3/H4 độc lập.

#### Control `SET_REPORT` live: đạt có điều kiện ngày 2026-10-04

`brickpro-functionfs-control-probe.c` dùng HID vendor-defined lab, một
interrupt-IN 8 byte và Output report 32 byte qua ep0. Nó không đọc input thật,
không gọi motor, không đụng radio hoặc dữ liệu thẻ. Host probe chỉ mở thiết bị
có đúng serial phiên `nxep0.<pid>.<monotonic>`, VID/PID lab, `bcdDevice=9903`,
usage `ff00:0001`, input length 9 và output length 33.

Lượt đầu bind/enumerate được nhưng chỉ sống khoảng một giây và phục hồi thiếu
MTP/identity. Hai lỗi harness được xác định từ log, không quy cho phần cứng:

- SIGALRM dùng chung handler với SIGTERM làm vòng probe tự dừng sau timeout I/O;
- target tương đối đọc từ ConfigFS không thể truyền ngược nguyên xi cho
  `symlink(2)`, vì kernel phân giải nó từ cwd của tiến trình. Phục hồi dùng
  canonical absolute function path rồi xác minh target.

Máy được phục hồi qua SSH không reboot: đúng PID/start-time ADB 1640/612,
MTP 2310/942 và input 2122/836; đúng hai link, `18d1:d002`, `bcdDevice=0409`,
product/serial stock, UDC configured high-speed. Sau sửa, binary SHA-256
`7EC640908E9BEA88E08A7B076D3C426C5EC53DCAC3A18CAB2CEB1309C2C7FF60`
vượt 12 fault fixtures rồi chạy live-02:

- Windows xác minh đúng HID lab, đọc neutral input 9 byte;
- `HidD_SetOutputReport` gửi phase A; `WriteFile` gửi phase B;
- Brick Pro nhận cả hai setup `21/09`, `wValue=0200`, `wIndex=0`,
  `wLength=32`, payload khớp; một payload C sai bị log và bỏ qua;
- `output_mask=3`, `lifecycle_rc=0`, `probe_rc=0`;
- guardian tháo lab, tạo lại link MTP, phục hồi ba thuộc tính, bind UDC và
  resume đúng hai process. Windows hậu kiểm tức thời thấy Composite/ADB/WPD
  đều `OK`.

Sau các unit test offline tiếp theo, một hậu kiểm muộn thấy `adb devices` rỗng
và **mọi** instance `18d1:d002` đều `Present=false`, Code 45; cổng SSH 22 cũng
không nghe vì listener do phiên audit bật đã được chủ động tắt trước đó. Chưa
có dữ liệu để phân biệt cáp/cổng bị rút với UDC tự mất sau phục hồi, nên không
quy nguyên nhân cho probe cũng không gọi lifecycle dài hạn đã đạt. Giữ máy
không reboot, cắm lại cáp và lấy dmesg/journal RAM là bước bắt buộc kế tiếp.
Xem `windows-delayed-disconnect.json` và `windows-delayed-disconnect-time.txt`.

Bằng chứng ở `.local/controller-ep0-evidence-20261004/`, nổi bật
`host-live-02.txt`, `nx-ep0-live-02.log`, `final-device-state.txt` và
`windows-final-state.json`. Đây chỉ chứng minh đường generic HID output qua
control endpoint. Nó chưa chứng minh lifecycle dài hạn, DS4 feature handshake,
packet/CRC, Home, Steam/game-driven rumble, motor backend hoặc độ bền nhiều chu kỳ.

#### Nền codec DS4 USB: mới đạt unit test, chưa tích hợp transport

Đã thêm `ds4_codec.c/.h` tách khỏi lifecycle/UI. Mapper công khai enum nút
canonical theo vị trí vật lý thay vì để codec biết B-number hoặc bit riêng của
HORIPAD. Golden tests hiện xác nhận:

- input USB 64 byte, report ID `0x01`, bốn trục, đủ tám hướng hat;
- Xbox/Nintendo face layout, L1/R1, cò số đồng thời thành bit + `0/255`,
  Share/Options/L3/R3 và HOME/PS;
- pin/cable/counter trung tính, không tự quảng cáo touch/gyro;
- decoder output USB 32 byte ID `0x05`, cùng dạng control 31 byte khi report ID
  nằm trong `wValue`; từ chối sai ID/length và chỉ đánh dấu motor/LED/blink khi
  valid flags tương ứng bật.

Mapper cũ, codec mới và toàn binary AArch64 đều build `-Werror`; unit tests
đều đạt. Đây là chuẩn bị mã, không phải chứng cứ host đã nhận DS4: descriptor,
feature report `0x03/0x12/0x02`, identity hợp lệ, SDL/Steam detection và live
output `wValue=0x0205` vẫn phải qua spike riêng. Decoder chưa nối motor.

#### Hồi quy và một lỗi harness cần loại bỏ

Chín case guardian/worker chết + kiểm tra link absolute/relative/foreign đều
đạt sau sửa (`guardian-selftest-after-link-fix.txt`). Ma trận wrapper/coordinator/
diagnostic trên PC cũng đạt lại với exit 0. Cả hai là fixture, không thay thế
kiểm thử transition mới trên thiết bị.

Stock ADB firmware này không truyền exit code của remote shell: probe khóa
trả 2 nhưng host ADB trả 0, `adb features` rỗng. Harness phải thu marker
`probe_rc` hoặc journal + hậu điều kiện riêng, không dùng chỉ `$LASTEXITCODE`
của ADB để gọi bài test thành công. `live-gate-test.txt` ghi `probe_rc=2`,
không tạo journal. Windows sau phục hồi báo cả Composite/ADB/WPD đang present
đều OK (`windows-after-recovery.json`).

Cuối phiên:

- Chín regression của kernel inspector đạt, bao gồm wrong boot/config/base,
  live code/caps/list/callback mismatch. Dữ liệu lỗi chỉ thay trong mock trên PC,
  không sửa snapshot hoặc thiết bị (`kernel-inspection-regression.txt`).
- Bản IN+OUT từng chạy lỗi trong `/tmp` được thay bằng bản khóa, sau khi xác
  nhận không có process probe và kiểm tra đúng SHA bản cũ/mới. SHA mới:
  `53DC7AEC4472399F505F361E64BF59B126AA19B674B2DD3D8675EEFC1EF6B916`.
  Nó trả `probe_rc=2`, không tạo journal, không tiếp quản USB
  (`live-gate-current-test.txt`); bản cũ/raw log được giữ trên PC để đối chiếu.
- ADB/stock input/MTP giữ PID 1640/2122/2310; UDC configured high-speed, boot ID
  và identity giữ nguyên. Windows Composite/ADB/WPD đang present đều OK
  (`final-device-state.txt`, `windows-final-state.json`). Đây là trạng thái
  nhận diện, không phải benchmark hay thử truyền file MTP.
- Đóng phiên SSH của agent, rồi dùng service stop đúng listener PID/start-time
  đã xác minh. Không sửa SSH-on-boot và không còn listener TCP 22. Một phiên
  SSH cũ khác PID 11752 được giữ nguyên, không kill process ngoài phiên agent;
  UI dùng `pidof sshd` nên có thể còn hiện On tới khi client cũ đóng. Không gọi
  init `shutdown` vốn kill cả client.

### H3 — Bluetooth tối thiểu trên NX Redux

Một phiên lab có snapshot/restore process và radio:

1. Xác nhận BlueZ/version, XR829/driver và plugin nào sở hữu PSM 17/19.
2. Thử generic Classic HID với một host, pairing chỉ mở có thời hạn.
3. Sweep 20/15/12/10/8 ms; ghi queue age, throughput, p95/p99 và độ trễ tăng theo thời gian.
4. Thử có/không Bluetooth audio và tải Wi-Fi có kiểm soát để phân biệt giới hạn radio/coexistence với xung đột daemon/chính sách.
5. Nếu controller mode tạm dừng audio hoặc Wi-Fi, UI phải báo trước và supervisor phải phục hồi đúng trạng thái trước phiên.
6. Kill worker và supervisor ở từng bước; radio/audio/Wi-Fi/pairing phải trở lại đúng trạng thái trước phiên.

### H4 — danh tính và nhận dạng host

Đây là cổng sản phẩm, không chỉ là thủ tục:

- xin PID hợp lệ (ví dụ pid.codes nếu dự án đáp ứng điều kiện open-source, hoặc quyền dùng VID/PID từ chủ sở hữu);
- test identity đó trên Windows Device Manager, Game Controllers, Chromium Gamepad API, SDL, Steam và các game mục tiêu;
- xác nhận Home/Guide và output rumble thực tế;
- nếu SDL/Steam cần nhận dạng mới, gửi mapping/upstream support trước khi gọi protocol là ổn định.

Không có kết quả H4 thì chỉ được phát hành alpha thử nghiệm, không gọi là `0.1.0` stable.

## 5. Kiến trúc xây lại đã hiệu chỉnh

```text
evdev/input daemon
        │
        ▼
physical state + monotonic timestamps ──► diagnostics
        │
        ├──► action router ──► UI / screen / profile / transport / quit
        │
        ▼
profile + remap + calibration
        │
        ▼
protocol codec (generic HID / protocol đã vượt H4)
        │                         ▲
        ▼                         │
USB FunctionFS hoặc BT L2CAP ──► output/feature decoder
                                  │
                                  ▼
                         feedback service
                         motor / LED / battery

supervisor bao quanh toàn phiên: snapshot → takeover → heartbeat → rollback
```

Các nguyên tắc không thay đổi:

- Canonical state dùng tên/vị trí vật lý, không dùng B-number hay report bit của một protocol.
- Analog giữ state mới nhất; cạnh nút được giữ riêng với giới hạn rõ ràng. Khi tràn: log, neutral và resync, không phát lại backlog kéo dài.
- UI không được chặn input/send loop và ngừng redraw khi màn hình tắt.
- Mỗi codec có golden packet/control/CRC tests riêng.
- Mọi transition mapping/profile/menu phát neutral/release và có release gate để không kẹt nút.
- Mỗi transport chỉ có một host chủ động; đổi transport phải neutral, dừng motor rồi mới ngắt.
- Supervisor quản PID + start time thuộc phiên, không `killall`; restore trạng thái trước phiên thay vì mặc định ADB/MTP/Bluetooth bật.
- USB controller mode dùng chung coordinator với MTP/UMS; không có pak nào tự ý tranh UDC.
- Rumble dùng motor service chung với NX, master Motor switch và cap duy nhất; mất heartbeat/transport/worker là tắt ngay.

## 6. Hệ thống phím tắt mới

F1/F2 hiện tại trở thành **action mặc định**, không còn bị hard-code sâu trong mapper. Action router phân biệt ngữ cảnh `NextUI`, `Controller`, `Quick settings` và `Pairing`, đồng thời hỗ trợ tap/hold/double/chord với ưu tiên rõ ràng.

Đề xuất mặc định cho Controller:

| Thao tác | Action mặc định | Quy tắc |
|---|---|---|
| F1 tap | đổi Xbox/Nintendo layout | chỉ đổi mapping, không reconnect |
| F1 hold | chọn profile nhanh | release/neutral trước khi mở |
| F2 tap | tắt/bật màn hình | transport không bị ảnh hưởng |
| F2 hold | đổi chính sách auto-screen | không đổi brightness của phiên ngoài app |
| MENU tap | quick settings | MENU không truyền sang game |
| MENU + F1 | pairing/reconnect | chỉ hợp lệ ở Bluetooth; yêu cầu hold để tránh nhầm |
| MENU + F2 | đổi transport | có confirm nếu đang kết nối; neutral + stop rumble |
| MENU + SELECT hold 1,5 s | thoát | giữ làm emergency exit |
| HOME | luôn gửi cho host khi đã kết nối | không mở OSD NX trong controller context |
| Volume ± | âm lượng cục bộ | mặc định không gửi media key; có thể cấu hình sau |

Không chốt double-tap thành action mặc định cho tới khi đo khả năng phát hiện chính xác và độ trễ mà cửa sổ double-tap gây ra. Shortcut config được lưu theo schema có version và migration; UI hiển thị xung đột trước khi lưu.

## 7. Chiến lược protocol đã sửa

1. **Generic HID với identity hợp lệ** là baseline input/fallback có thể phát hành sau khi được Steam/SDL/game mục tiêu nhận. Nó không được hứa rung trước khi output path được host thực tế sử dụng.
2. **DS4 USB/Bluetooth** là nhánh experimental cho tới khi H2, H3 và H4 đều đạt. Không phát hành bằng cách giả Sony ID; không giả capability touchpad/gyro mà máy không có.
3. **HORIPAD hiện tại** chỉ giữ làm regression oracle/lab compatibility. Không phải identity stable.
4. **Xbox Bluetooth/USB XInput** ở backlog. Code Padpod cho thấy một đường Bluetooth, nhưng dùng Microsoft identity và chưa phải bằng chứng NX Redux hoặc cơ sở phát hành hợp lệ.
5. Nếu không tìm được protocol sạch vừa được Windows/Steam nhận vừa trả rumble, phải nói rõ giới hạn ở alpha; không hạ tiêu chí stable một cách âm thầm.

## 8. Version và thứ tự triển khai mới

| Mốc | Điều kiện bắt buộc |
|---|---|
| `0.1.0-dev.N` | H0–H4, probe/spike; không gọi là release dùng hàng ngày |
| `0.1.0-alpha.1` | core mới, action router, supervisor/restore, generic USB identity hợp lệ, regression mapping/Home |
| `0.1.0-alpha.2` | USB protocol đã vượt H2/H4, output report và game-driven rumble thật |
| `0.1.0-alpha.3` | Bluetooth đã vượt H3/H4, pairing/reconnect một host, rumble và exact restore |
| `0.1.0-beta.1` | UI/profile/remap/calibration/screen/pin/LED/diagnostics/migration |
| `0.1.0-rc.1` | feature freeze; toàn bộ acceptance suite và recovery fault injection |
| `0.1.0` | USB + Bluetooth + Home + game-driven rumble đều đạt trên Windows/Steam bằng identity phát hành |

`0.1.0` vẫn là mục tiêu đúng. Việc thêm `dev.N` giúp nghiên cứu giao thức không bị trình bày nhầm là alpha gần hoàn thiện.

## 9. Kiểm thử chấp nhận đã hiệu chỉnh

Giữ các game và bài test cũ, nhưng báo cáo phải có thêm:

- identity, USB speed, descriptor dump và mapping source của host;
- raw `SYN_REPORT`, event/trục, unique canonical states, report queued, report completed và host-visible changes;
- số liệu raw lẫn lọc, median/p95/p99/max, outlier rule và queue age theo thời gian;
- input latency và rumble-return latency là hai phép đo riêng;
- test stock input daemon và 8 ms A/B; không dùng 4 ms làm mặc định;
- Home được kiểm tra ở Windows Game Controllers, Chromium, Steam Overlay và ít nhất hai game;
- rumble phải phát từ game qua cả USB và Bluetooth, sau đó thử mất cáp/link/worker/supervisor;
- 100 vòng USB, 50 lần chuyển transport và phiên 2 giờ chỉ chạy sau khi spike lifecycle đạt; trước đó dùng vòng ngắn để tránh lặp lỗi UDC/UMS chưa an toàn;
- MTP/UMS/ADB được test như trạng thái độc lập; restore đúng trạng thái ban đầu, không chỉ kiểm tra “ADB chạy lại”.

Các ngưỡng 90 Hz USB và 80 Hz Bluetooth được giữ là **mục tiêu**, không phải kết luận. Nếu host-visible rate thấp hơn source rate, phải định vị tầng gây giới hạn trước khi tối ưu.

## 10. Nguồn chính

- [Thông số chính thức TrimUI Brick Pro](https://trimui.net/products/trimui-brick-pro)
- [USB-IF: quyền sử dụng VID](https://www.usb.org/developers)
- [Điều kiện xin PID từ pid.codes](https://pid.codes/howto/)
- [Linux FunctionFS documentation](https://docs.kernel.org/usb/functionfs.html)
- [Linux 4.9 FunctionFS source](https://github.com/torvalds/linux/blob/v4.9/drivers/usb/gadget/function/f_fs.c)
- [Allwinner Tina Linux USB development guide](https://tina.100ask.net/SdkModule/Linux_USB_DevelopmentGuide-02/)
- [Linux PlayStation HID driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)
- [SDL2 PS4 HIDAPI driver](https://github.com/libsdl-org/SDL/blob/SDL2/src/joystick/hidapi/SDL_hidapi_ps4.c)
- [W3C Gamepad specification](https://www.w3.org/TR/gamepad/)
- [BlueZ ProfileManager](https://github.com/bluez/bluez/blob/master/doc/org.bluez.ProfileManager.rst)
- [BlueZ HID input server](https://github.com/bluez/bluez/blob/master/profiles/input/server.c)
- [Microsoft: XInput và DirectInput](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-and-directinput)
- [Valve: Steam Input gamepad emulation](https://partner.steamgames.com/doc/features/steam_controller/steam_input_gamepad_emulation_bestpractices)
- [Padpod, đối chiếu tại commit `0c37f1f...`](https://github.com/duyquang6/padpod/tree/0c37f1f684638a8ff7a66c1980fcf16f03aaa452)
- [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html)

Padpod tại commit đã ghim dùng GPL-3.0-only kèm điều khoản bổ sung về credit, tên và modified versions. NX Controller không nên sao chép/port source hoặc descriptor từ đó một cách ngầm định. Dùng Linux/SDL/BlueZ và đặc tả làm nguồn triển khai độc lập; nếu thật sự lấy mã Padpod thì phải giữ đầy đủ attribution, source và điều khoản đi kèm.
