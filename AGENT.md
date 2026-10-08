# NX Redux — Tổng quan kiến trúc và hướng dẫn làm việc

Tài liệu này là bản đồ kỹ thuật một-file cho repository NX Redux hiện tại. Mục tiêu là giúp người hoặc agent mới hiểu nhanh hệ thống chạy như thế nào, mã nào sở hữu chức năng nào, phải build/cài/kiểm thử ở đâu, và những bất biến an toàn không được phá vỡ.

Phạm vi của tài liệu là kiến trúc và các API/điểm vào quan trọng. Repository có hàng nghìn hàm `static`; liệt kê từng hàm nhỏ sẽ làm tài liệu khó tra cứu và nhanh lỗi thời. Khi tài liệu và mã khác nhau, ưu tiên theo thứ tự: mã nguồn đang dùng → Makefile/script đóng gói → tài liệu `.dev/` → ghi chú trong file này.

## 1. Dự án là gì

NX Redux là custom firmware giao diện tối giản cho máy chơi game cầm tay TrimUI. Đây là fork GPL-3.0 của NextUI, mà NextUI kế thừa từ MinUI. Hệ thống giữ trải nghiệm chọn game và chơi nhanh, đồng thời thêm Settings viết bằng C, Game Switcher, OSD, PortMaster, RetroAchievements, netplay, trình phát nhạc/video, đồng bộ thiết bị và nhiều công cụ khác.

Các thiết bị được đóng gói riêng:

| `DEVICE` | Platform build | Màn hình | Ghi chú |
|---|---|---:|---|
| `brick` | `tg5040` | 1024×768 | TrimUI Brick |
| `brickpro` | `tg5040` | 1024×768 | TrimUI Brick Pro; một số tài liệu stock gọi phần cứng là TG4040 nhưng dùng target `tg5040` của dự án |
| `smartpro` | `tg5040` | 1280×720 | TrimUI Smart Pro |
| `smartpros` | `tg5050` | 1280×720 | TrimUI Smart Pro S |
| desktop | `desktop` | cửa sổ SDL | Dùng để lặp UI nhanh, không thay thế kiểm thử phần cứng |

Không dùng gói build của model này cho model khác. Cùng platform không có nghĩa là cùng asset OSD, độ phân giải hoặc cấu hình thiết bị.

## 2. Bản đồ repository

```text
.
├── Makefile                     # điều phối build/package/deploy ở máy host
├── Makefile.toolchain           # chạy device build trong Docker toolchain
├── Makefile.native              # desktop build
├── workspace/
│   ├── Makefile                 # thứ tự build các component
│   ├── all/                     # mã dùng chung cho mọi platform
│   ├── tg5040/                  # platform glue cho Brick/Brick Pro/Smart Pro
│   ├── tg5050/                  # platform glue cho Smart Pro S
│   └── desktop/                 # backend và fake SD cho desktop
├── skeleton/
│   ├── BASE/                    # nội dung người dùng/SD dùng chung, emulator assets
│   └── SYSTEM/                  # `.system`, pak, script boot, OSD và binary phụ
├── scripts/                     # đóng gói, patch binary, test host/E2E, benchmark
├── toolchains/                  # clone/cache toolchain; không phải nguồn sản phẩm
├── releases/                    # zip phát hành tạo ra; không sửa trực tiếp
├── .dev/                        # tài liệu chuyên sâu theo chủ đề
├── .tmp*/                       # bản sao lưu, điều tra, tải về tạm; không được đóng gói
└── Ruffle-Handheld/             # add-on độc lập đang nằm trong working tree; không thuộc build chính nếu chưa được nối vào Makefile/skeleton
```

Quy tắc nguồn dữ liệu:

- Sửa C/C++ ở `workspace/`, không sửa binary đã build.
- Sửa nội dung sẽ nằm trên thẻ ở `skeleton/`, không sửa `build/` hay `releases/`.
- `build/`, `releases/`, `toolchains/`, cache Zig và `.tmp*` là output/cache/scratch.
- Dữ liệu trong `.tmp/sd-recovery-*` là bản sao phục hồi của người dùng. Không xem đó là nguồn chính và không xóa nếu chưa được người dùng cho phép.
- Working tree có thể đang chứa thay đổi chưa commit của người dùng. Không reset, checkout đè hoặc “dọn sạch” thay đổi ngoài phạm vi công việc.

## 3. Kiến trúc ở mức cao

```text
Stock boot / updater
        │
        ▼
`.system/<platform>/paks/MinUI.pak/launch.sh`
        │ khởi động daemon, OSD, audio/input, tạo launch loop
        ▼
`nextui.elf` ── người dùng chọn ROM/pak ──► ghi lệnh vào `/tmp/next`
        │                                      │
        │ thoát sạch                           ▼
        └────────────────────────────── launch loop `eval` lệnh
                                               │
                         ┌─────────────────────┴────────────────────┐
                         ▼                                          ▼
                 `minarch.elf` + core                        Tool/standalone pak
                         │                                          │
                         └──────────── thoát về launch loop ─────────┘
                                               │
                                               ▼
                                         chạy lại `nextui.elf`
```

`launch.sh` của `MinUI.pak` là supervisor. `nextui.elf` không trực tiếp thay thế chính nó bằng game; nó chuẩn bị lệnh launch, ghi `/tmp/next`, rồi thoát. Supervisor đọc và chạy lệnh, dọn trạng thái, sau đó mở lại NextUI.

Các file điều khiển quan trọng:

| Đường dẫn | Ý nghĩa |
|---|---|
| `/tmp/nextui_exec` | Còn tồn tại thì supervisor tiếp tục launch loop |
| `/tmp/next` | Một lệnh pak/game chờ supervisor chạy |
| `/tmp/last.txt` | Trạng thái điều hướng tạm |
| `/tmp/resume_slot.txt` | Slot cần resume ở lần mở game tiếp theo |
| `/tmp/poweroff` | Yêu cầu shutdown an toàn qua `poweroff_next` |
| `/tmp/reboot` | Yêu cầu reboot |
| `/tmp/nextui_open` | Yêu cầu NextUI mở một pak từ tiến trình ngoài |
| `/tmp/show_osdd`, `/tmp/hide_osdd` | Hiện/ẩn OSD |

Không giết `nextui.elf` tùy tiện. Launcher phân biệt thoát để chạy pak, thoát để tắt máy và crash bằng các marker trên. Giết sai lúc có thể làm máy tắt hoặc bỏ qua dọn dẹp.

## 4. Luồng boot, update và launch

### 4.1 Cài đặt/update

- `workspace/<platform>/install/boot.sh` chuẩn bị môi trường platform và khởi động hệ thống.
- `workspace/<platform>/install/update.sh` chạy khi cài `MinUI.zip`, không chạy ở mọi lần boot.
- `skeleton/` được tập hợp vào `build/BASE/MinUI-<device>.zip` trong bước package.
- Mỗi update có thể thay toàn bộ `.system`; dữ liệu người dùng phải sống ngoài `.system`, chủ yếu trong `.userdata`, `Roms`, `Saves`, `Collections` và các pak cộng đồng ở `Tools`/`Emus`.

### 4.2 Supervisor

`skeleton/SYSTEM/<platform>/paks/MinUI.pak/launch.sh` thực hiện các việc chính:

1. Thiết lập biến đường dẫn, thư viện và PATH.
2. Khởi động các daemon nền như input, `keymon`, `audiomon`, music service và OSD.
3. Chuẩn bị OSD overlay theo thiết bị.
4. Khởi động MTP khi platform hỗ trợ.
5. Chạy vòng `nextui.elf → /tmp/next → pak/game → nextui.elf`.
6. Phục hồi CPU sau khi pak kết thúc.
7. Chuyển shutdown qua `poweroff_next` để đồng bộ và unmount thẻ trước khi cắt nguồn.

Trên nhánh hiện tại, launcher tg5040 chỉ khôi phục USB mode sau `USB Storage.pak` hoặc `USB Gamepad.pak`. Game bình thường không được phép tháo/gắn lại MTP khi thoát.

### 4.3 Mở ROM

Quy tắc bắt buộc và đầy đủ về bố trí, đọc, chuyển đổi, sắp xếp và sửa chữa
game/ROM nằm tại `.agents/rules/games-and-roms.md`; `AGENTS.md` yêu cầu mọi
agent đọc file đó trước khi xử lý dữ liệu game.

Luồng điển hình:

```text
nextui: `Entry_open`
  → nhận diện thư mục/tag emulator
  → `openRom`
  → ghi lệnh pak vào `/tmp/next`
  → supervisor chạy `Emus/<TAG>.pak/launch.sh`
  → pak chọn core/config
  → `minarch.elf <core.so> <rom>`
```

Tên thư mục ROM dạng `Tên (TAG)` chọn pak `Emus/TAG.pak`. `map.txt` trong thư mục ROM có thể ánh xạ tên file sang tên hiển thị.

### 4.3.1 Quy định bố trí game và chọn cấu hình

- Đặt game trong `Roms/<Tên hệ máy (TAG)>/`. Nếu thư mục không có `(TAG)`, tên thư mục chính là tag: `Roms/GBA` chọn `GBA.pak`; `Roms/NEOGEO` cần `NEOGEO.pak`, không tự động chọn FBN chỉ vì nội dung là game Neo Geo.
- `getEmuPath()` ưu tiên lần lượt: `Emus/<TAG>.pak/launch.sh` → `Emus/<PLATFORM>/<TAG>.pak/launch.sh` → `.system/paks/Emus/<TAG>.pak/launch.sh`. Pak người dùng có cùng tag sẽ che pak hệ thống; kiểm tra launcher thực sự được chọn trước khi sửa.
- Pak chọn core hoặc chương trình standalone. Với libretro, `minarch` lấy tag từ đường dẫn ROM, không từ tên core. BIOS mặc định ở `Bios/<TAG>/`, cấu hình riêng core ở `.userdata/<platform>/<TAG>-<core>/`; runtime như netplay có thể ghi đè đường dẫn BIOS/save. BIOS bắt buộc hay tùy chọn phụ thuộc core; không suy rằng thư mục BIOS trống luôn là nguyên nhân crash.
- Cấu hình minarch được đọc theo tầng: `.system/system.cfg` → `default.cfg` của pak đang được chọn → cấu hình người dùng. Hai tầng đầu ưu tiên `system-<DEVICE>.cfg` và `default-<DEVICE>.cfg` nếu có, thay cho bản không hậu tố.
- Ở tầng người dùng, chọn `<game.alt_name>-<DEVICE>.cfg` nếu tồn tại; nếu không, chọn `minarch-<DEVICE>.cfg`. Khi không có biến `DEVICE`, tên file không có hậu tố thiết bị. Tầng game và tầng console là hai lựa chọn, không được mô tả thành bốn lớp luôn cộng dồn. `game.alt_name` có thể là tên ROM đã giải nén theo `useExtractedFileName`, hoặc tên playlist `.m3u` của game nhiều đĩa.
- Không đổi tên ROM/tag hoặc chuyển pak tùy tiện: tag/tên game liên quan đến BIOS, cấu hình, save, state, recent và artwork. Nếu cần đổi, bảo toàn và cập nhật các tham chiếu tương ứng; xem thêm mục 6 và `.dev/ARCHITECTURE.md`.

### 4.3.2 Quy định đọc ROM nén và game đĩa

- “Không cần người dùng giải nén” không có nghĩa là dữ liệu không được giải nén. `Game_open()` đối chiếu `.zip`/`.7z` với `retro_system_info.valid_extensions` của core. Core hỗ trợ định dạng archive đó được nhận nguyên file để tự đọc.
- Nếu core không đọc archive đó trực tiếp, minarch giải nén vào `/tmp/nextarch/<TAG>/` rồi truyền ROM đã giải nén cho core. Đây là vùng tạm trong RAM trên thiết bị, không ghi đè ZIP gốc trên thẻ; có thể tái sử dụng bản tạm trong phiên trước khi tắt máy. Giải nén tăng thời gian mở game và dùng thêm RAM.
- Với ROM băng như FC/GB/GBA, ZIP hoặc 7z chứa một ROM đúng định dạng có thể dùng cơ chế tự giải nén trên. Không bắt người dùng giải nén hàng loạt các game đang hoạt động tốt; định dạng không nén cũng hợp lệ nếu core hỗ trợ.
- Với Neo Geo/Arcade dùng FBNeo, ZIP là romset gồm nhiều thành phần, được core đọc trực tiếp. Giữ nguyên tên ZIP, cấu trúc và các thành phần của bộ ROM; không tách thành các game riêng hoặc nén lại tùy tiện. Kiểm tra romset/CRC/BIOS phù hợp với core, không chỉ nhìn đuôi ZIP.
- Với PlayStation và các hệ game đĩa, không bọc BIN/CUE hoặc ảnh đĩa lớn trong ZIP/7z để minarch tự giải nén: cơ chế này chỉ chọn một file phù hợp, không giữ đầy đủ các track phụ thuộc, và ảnh lớn có thể làm đầy RAM. Dùng BIN/CUE đầy đủ, mở bằng CUE và giữ đúng đường dẫn/tên track; hoặc CHD nếu core hỗ trợ. CHD nén dữ liệu đĩa nhưng được đọc trực tiếp, không bung toàn bộ đĩa vào vùng tạm.
- Game nhiều đĩa có thể dùng playlist `.m3u` khi core/launcher hỗ trợ; mọi đường dẫn đĩa trong playlist và track trong CUE phải trỏ tới file có thật. Không tự tạo CUE, pregap hoặc danh sách track theo phỏng đoán.
- Emulator standalone như N64/NDS/Dreamcast và PortMaster không mặc nhiên đi qua `Game_open()`. Dùng định dạng được chính emulator/launcher hỗ trợ; không hứa mọi ZIP đều chạy chỉ vì các core libretro có cơ chế trên. PortMaster dùng script và dữ liệu game theo mục 10.
- Kiểm tra định dạng thật và nội dung archive, không chỉ phần mở rộng. Đổi `.rar` hay `.7z` thành `.zip` không chuyển đổi dữ liệu. Gói chứa ECM/APE hoặc nhiều archive lồng nhau phải được chuyển thành dữ liệu đĩa hợp lệ trước khi dùng; giữ bản gốc và kiểm tra checksum sau chuyển đổi.

Nguồn đối chiếu: `workspace/all/common/utils.c` (`getEmuName`, `getEmuPath`), `workspace/all/minarch/ma_core.c` (`Core_open`), `ma_config.c` (`Config_getPath`, `Config_load`) và `ma_game.c` (`Game_open`, `extract_zip`, `extract_7z`). Hướng dẫn định dạng cho người dùng: [ROM File Formats — NX Redux](https://nxredux.com/emulators/rom-formats/).

### 4.4 Mở tool hoặc pak độc lập

`openPak` và `openPakInPlace` trong `workspace/all/nextui/launcher.c` tạo lệnh chạy `launch.sh` của pak. `openPakInPlace` còn lưu vị trí danh sách để khi quay lại vẫn ở thư mục trước đó. Một pak là thư mục có hậu tố `.pak` và có `launch.sh` bên trong.

## 5. Các tầng mã nguồn

### 5.1 `workspace/all/common`: nền tảng dùng chung

Đây không phải một thư viện động duy nhất. Nhiều file common được compile tĩnh vào từng executable, vì vậy thay đổi common chỉ có hiệu lực với những binary đã được rebuild.

Nhóm API quan trọng:

| File/API | Trách nhiệm |
|---|---|
| `api.c`, `api.h` | vòng đời SDL/GFX, layer render, font/asset, input `PAD_*`, audio `SND_*`, nguồn `PWR_*`, rung `VIB_*`, LED `LEDS_*` |
| `config.c`, `config.h` | defaults, parse, getter/setter và ghi nguyên tử `minuisettings.txt` |
| `paths.c`, `paths.h`, `defines.h` | resolve đường dẫn SD/system/userdata; desktop hỗ trợ root runtime |
| `notification.c`, `.h` | hàng đợi notification ingame và indicator volume/brightness/color temperature |
| `play_time.c`, `.h` | bộ đếm thời gian chơi chủ động và mốc nhắc nghỉ |
| `audio_manager.c`, `.h` | theo dõi speaker/Bluetooth/USB DAC và chọn sample rate |
| `wifi.c`, `generic_wifi.c`, `generic_bt.c` | trạng thái và thao tác radio |
| `display_helper.c`, `drm_scanout.c` | phục hồi display/capture theo platform |
| `emu_overlay*` | menu overlay cho emulator standalone |
| `ra_*` | đăng nhập, badge, offline cache/sync của RetroAchievements |
| `http`, `wget_fetch`, `net_cache`, `stream_probe` | tải mạng, cache và probe stream |
| `scaler`, `text_shape`, `text_wrap`, `rgba` | primitive hiển thị/text |
| `common/ui/` | component UI tái sử dụng: list, menu bar, dialog, keyboard, toast, splash, progress, hint bar |

Mọi component UI mới phải được khai báo tập trung trong `workspace/all/common/ui/ui.mk`. Không thêm cùng source đó riêng lẻ vào nhiều Makefile.

Các hàm nền tảng nên biết:

- `PATHS_init(PLATFORM)`: phải chạy trước khi đọc macro đường dẫn.
- `InitSettings()` / `QuitSettings()`: vòng đời settings/msettings ở cấp ứng dụng.
- `CFG_init()`, `CFG_sync()`: nạp và ghi cấu hình.
- `GFX_init()`, `GFX_startFrame()`, `GFX_flip()`, `GFX_sync()`: vòng đời render.
- `PAD_init()`, `PAD_poll()`, `PAD_reset()`, `PAD_justPressed()`: input.
- `PWR_init()`, `PWR_update()`, `PWR_powerOff()`: pin, sleep và shutdown.
- `Notification_push()`, `Notification_update()`, `Notification_renderToLayer()`: notification trong minarch.

### 5.2 `workspace/all/nextui`: launcher/UI chính

| File | Chức năng |
|---|---|
| `nextui.c` | `main`, init hệ thống, state machine giữa Game List/Search/Game Switcher, dirty render loop, CPU policy |
| `content.c` | quét thư mục, tạo `Directory`/`Entry`, cache emulator và ROM index, collections/tools/search |
| `launcher.c` | mở thư mục, ROM, script, pak; resume; ghi `/tmp/next`; lưu/khôi phục vị trí |
| `gamelist.c` | input/render danh sách, context menu, rename/delete/collection/art fetch/netplay |
| `gameswitcher.c` | carousel các game có thể resume |
| `search.c` | tìm ROM toàn hệ thống |
| `recents.c` | đọc/ghi danh sách gần đây |
| `shortcuts.c` | shortcut ở menu gốc |
| `imgloader.c` | worker async, cache thumbnail/background, báo dirty khi ảnh xong |
| `artbg.c` | resolve và render background/art style |
| `cpu_policy.c` | boot boost, mức menu và mức idle |
| `types.c`, `.h` | `Entry`, `Directory` và container liên quan |

Vòng lặp `nextui.c` là dirty-flag loop: chỉ redraw khi input, animation, status hoặc worker làm thay đổi nội dung. Tác vụ nền muốn hiện ra mà không có input phải đặt dirty/request draw; không được giả định có render 60 FPS liên tục.

### 5.3 `workspace/all/minarch`: frontend libretro

`minarch.elf` tải core libretro và ROM rồi sở hữu game loop. Các module chính:

| File | Chức năng |
|---|---|
| `minarch.c` | orchestration: init → load core/game → loop frame → cleanup |
| `ma_core.c` | `dlopen` core, bind callback/API libretro, init/load/run/unload |
| `ma_game.c` | mở ROM, archive/temp data và metadata game |
| `ma_environment.c` | xử lý callback môi trường libretro |
| `ma_config.c`, `ma_options.c` | cấu hình frontend/core và persistence |
| `ma_input.c` | đọc pad, hotkey và mapping sang RetroPad |
| `ma_video.c`, `ma_hwrender.c`, `ma_shaders.c` | video, GPU/hardware render, shader |
| `ma_audio.c` | audio core → hệ thống âm thanh chung |
| `ma_menu.c` | quick menu trong game |
| `ma_saves.c` | SRAM, save state, resume/preview |
| `ma_rewind.c` | rewind buffer và worker |
| `ma_runframe.c`, `ma_emutime.c` | chạy frame và pacing |
| `ma_cheats.c`, `ma_cheat_match.c` | cheat |
| `ma_cpu_profile.c`, `ma_cpu_affinity.c` | CPU range và pin thread theo pak |
| `ra_integration.c` | RetroAchievements trong game |
| `chd_reader.c` | đọc CHD phục vụ hash/media |

Trình tự quan trọng trong `minarch.c`:

1. `PATHS_init`, CPU monitor và startup performance.
2. `GFX_init`, `PAD_init`, `PWR_init`, LED/rung.
3. `Core_open` rồi `Game_open`.
4. `Config_load`, CPU affinity, `Core_init`, RA init, `Core_load`.
5. `Input_init`, audio, menu, notification và resume state.
6. Game loop: netplay/link → `run_frame`/`core.run` → RA → play-time notification → indicator → render.
7. Quick menu, sleep và suspend không được tính vào thời gian chơi.
8. Cleanup theo thứ tự đối xứng; tránh `goto` bỏ qua các subsystem đã init.

### 5.4 `workspace/all/settings`: Settings hợp nhất

`settings.c` phát hiện thiết bị, tạo cây menu bằng `SettingItem`/`SettingsPage`, sau đó dùng `settings_menu.c` để xử lý input và render.

Các loại item chính nằm trong `settings_menu.h`:

- cycle: chọn một giá trị trong danh sách;
- color: cycle kèm color swatch;
- button: chạy callback;
- submenu: đẩy một `SettingsPage` lên stack;
- text input: mở keyboard;
- static: chỉ hiển thị.

Module con:

| File | Khu vực |
|---|---|
| `settings_wifi.*` | Wi‑Fi, connect/diagnostics |
| `settings_bt.*` | Bluetooth |
| `settings_audio.*` | output/rate negotiation |
| `settings_led.*` | profile LED |
| `settings_input.*` | input tester/calibration |
| `settings_clock.*` | ngày giờ/timezone |
| `settings_bootlogo.*` | duyệt BMP và ghi `/mnt/boot/bootlogo.bmp` |
| `settings_developer.*` | SSH, sleep, logging và tùy chọn dev |
| `settings_updater.*` | kiểm tra/tải/cài update |

Để thêm setting mới cần đồng bộ đủ các lớp:

1. Thêm field/default/getter/setter/parse/serialize trong `common/config.h/.c`.
2. Thêm labels/values và callback get/set/reset trong `settings.c`.
3. Thêm `SettingItem` vào đúng page trong `build_menu_tree()`.
4. Nếu là page mới, khai báo storage, init page, reset callback và thêm vào root menu.
5. Rebuild Settings; nếu setting được đọc trong game thì rebuild cả minarch hoặc binary tiêu thụ.
6. Kiểm thử file `minuisettings.txt` sau nhiều lần mở/thoát để chắc chắn key không bị mất.

### 5.5 Daemon và công cụ

| Module | Output/vai trò |
|---|---|
| `audiomon` | theo dõi route âm thanh và phát sự kiện đổi sink |
| `keymon` | phím Home/Menu cấp hệ thống, OSD và shortcut phần cứng |
| `sleepmon` | power/sleep cho pak standalone |
| `poweroff_next` | shutdown có sync, chờ process và unmount SD |
| `syncsettings` | áp lại volume/brightness/color temperature sau app init |
| `nextval` | đọc nhanh một giá trị cấu hình cho shell script |
| `show2` | splash/progress hiển thị ngoài app chính |
| `screenshot` | daemon chụp màn hình |
| `screenrecorder` | ghi màn hình |
| `osdctl` | truy vấn/điều khiển OSD |
| `osdmusic` | bridge widget OSD ↔ music service |
| `osdwait` | `LD_PRELOAD` shim giảm wakeup của stock OSD daemon |
| `usbgamepad` | biến máy thành USB HID gamepad trên tg5040 |
| `taskset` | helper affinity động phù hợp kernel/toolchain |

### 5.6 Ứng dụng cấp cao

| Module | Chức năng |
|---|---|
| `musicplayer` | UI nhạc, `musicplayerd`, client/ctl, playlist, podcast, radio, lyrics |
| `mediaplayer` | video/audio/IPTV trên ffplay engine |
| `imageviewer` | duyệt, probe và xem ảnh |
| `sync` | Device Sync |
| `scraper` | Artwork Manager và tải artwork |
| `extras` | Xtras catalog/install/update/uninstall |
| `emu-options` | chỉnh option emulator theo game/hệ |
| `cheatdb` | tải và quản lý cheat database |
| `ratools` | trình duyệt, prefetch, reset và sync RetroAchievements |
| `gametime`, `gametimectl`, `libgametimedb` | Game Tracker và database thời gian chơi |
| `netplay`, `netplay-wizard`, `n64-netplay-server` | link/netplay và wizard kết nối |

## 6. Cấu hình và dữ liệu runtime

Các đường dẫn chính trên thiết bị:

| Đường dẫn | Nội dung |
|---|---|
| `/mnt/SDCARD/.system` | hệ thống được đóng gói; có thể bị thay khi update |
| `/mnt/SDCARD/.userdata/<platform>` | cache/log/cấu hình riêng platform |
| `/mnt/SDCARD/.userdata/shared` | cấu hình và dữ liệu dùng chung giữa thiết bị |
| `/mnt/SDCARD/Roms` | ROM và PortMaster game data |
| `/mnt/SDCARD/Saves` | save/save state |
| `/mnt/SDCARD/Tools` | tool pak do người dùng/Xtras cài |
| `/mnt/SDCARD/Emus` | emulator pak và PortMaster runtime |
| `/mnt/SDCARD/Collections` | mỗi file `.txt` là một collection |

Settings chính nằm ở `/mnt/SDCARD/.userdata/shared/minuisettings.txt`. `CFG_init()` nạp cả file vào bộ nhớ; `CFG_sync()` merge key đã biết với key lạ, bỏ qua write nếu nội dung không đổi, và ghi nguyên tử.

Wi‑Fi/Bluetooth là trường hợp đặc biệt: giá trị trên đĩa được coi là authoritative trừ khi chính process hiện tại vừa đổi radio. Không thay logic này bằng cách chụp trạng thái radio tức thời; radio có thể tạm down trong boot/suspend và làm lưu nhầm `0`.

Các định dạng đáng chú ý:

- Recent: `.userdata/shared/.minui/recent.txt`.
- Shortcuts: `.userdata/shared/.minui/shortcuts.txt`.
- Simple Mode: file `.userdata/shared/enable-simple-mode`; nội dung có thể là PIN 4 chữ số.
- Collections: một đường dẫn tương đối SD trên mỗi dòng.
- Cache emulator/ROM: `.userdata/<platform>/emulist_cache.txt` và `romindex_cache.txt`.
- Log ứng dụng: `.userdata/<platform>/logs/<name>.txt`.

Desktop build gọi `PATHS_init()` để lấy `NXREDUX_SDCARD` và `NXREDUX_SYSTEM_ROOT`; device build dùng đường dẫn compile-time.

## 7. Rendering, input và OSD

### 7.1 Rendering

- UI chính và phần lớn tool dùng dirty flag, không redraw liên tục.
- Modal blocking phải tiếp tục gọi `PWR_update()`.
- Sau modal, thường cần `PAD_poll(); PAD_reset();` để sự kiện nút đóng modal không lọt sang màn cha.
- tg5040 render fbdev/EGL qua `/dev/fb0`; tg5050 dùng DRM/KMS và fb0 không phải scanout thật.
- Asset scale của tg5040 phụ thuộc thiết bị lúc chạy; glyph dùng chung phải có đủ `assets@1x` đến `assets@4x` khi cần.

### 7.2 Input Brick Pro

Các SDL button index chính: B=0, A=1, Y=2, X=3, L1=4, R1=5, Select=6, Start=7, Menu=8, L3=9, R3=10, L4/FN1=11, R4/FN2=12, Volume−=13, Volume+=14, Home=15.

Các lối thoát khác nhau theo runtime:

- minarch/emulator: Menu mở quick menu; `MENU+SELECT` thoát sang Game Switcher và tạo resume state.
- Tool native: phần lớn dùng B để back và `MENU+SELECT` cho quit request.
- PortMaster: thường dùng `SELECT+START` thông qua `gptokeyb`; nếu game treo trước khi helper bắt được process thì tổ hợp có thể không hoạt động.
- OSD: Home trên Brick Pro hoặc giữ Menu khoảng 500 ms. Trường `hotkey` trong `osdlayout.json` không tự kích hoạt daemon; `keymon` mới là bên tạo `/tmp/show_osdd`.

Khi port treo ở startup, ưu tiên thoát bằng hotkey/helper hoặc ADB có kiểm soát. Chỉ giữ nút nguồn để cưỡng bức tắt khi không còn cách khác; không rút thẻ/cắt nguồn trong lúc đang ghi.

### 7.3 OSD

Stock `trimui_osdd` vẽ trên plane riêng và được overlay-mount asset từ SD vào `/usr/trimui/osd`. `skeleton/SYSTEM/osd/` có ba lớp:

- `common/`: widget/asset dùng chung;
- `res/<WxH>/`: asset theo độ phân giải;
- `device/<device>/`: daemon và override theo model.

`scripts/assemble-osd.sh` ghép các lớp lúc package. Thay OSD file qua ADB phải reboot vì mount/staging có thể vẫn phục vụ bản cũ.

## 8. Nhắc thời gian chơi trên nhánh hiện tại

Luồng dữ liệu:

```text
Settings > Play-time Reminders
  → `CFG_setPlayTime*`
  → `.userdata/shared/minuisettings.txt`
  → minarch đọc `CFG_getPlayTime*`
  → `PlayTimeTracker_update`
  → `Notification_push(NOTIFICATION_PLAY_TIME, ...)`
  → `Notification_renderToLayer(5)`
```

Các key:

- `playTimeReminderEnabled`
- `playTimeFirstMinutes`
- `playTimeRepeatMinutes`
- `playTimeNotificationDuration`

Defaults: tắt, mốc đầu 60 phút, lặp 60 phút, notification 5 giây. Mốc đầu bị clamp 1–1440 phút; lặp 0–1440 phút, trong đó 0 nghĩa là chỉ nhắc lần đầu; duration 1–30 giây.

`PlayTimeTracker` giữ `elapsed_ms`, `next_reminder_ms` và `last_tick`. Khoảng tick lớn hơn 5 giây không được cộng, nhờ đó sleep/menu/block dài không bị tính là thời gian chơi. Sau quick menu và sau sleep, minarch gọi `PlayTimeTracker_resume()` để đặt lại mốc tick.

Các file liên quan:

- `workspace/all/common/play_time.c/.h`
- `workspace/all/common/notification.c/.h`
- `workspace/all/common/config.c/.h`
- `workspace/all/minarch/minarch.c`
- `workspace/all/settings/settings.c`
- `workspace/all/common/tests/test_play_time.c`

Lưu ý: chức năng này hiện tích hợp vào minarch/libretro. PortMaster và emulator standalone không đi qua game loop của minarch nên chưa tự nhận notification này nếu không có bridge/daemon riêng.

### 8.1 Khôi phục trên thẻ G ngày 2026-10-03

Sau khi đợt sửa filesystem/emulator/USB/game hoàn tất và bàn giao thẻ, đã khôi phục riêng hai binary tg5040 có tính năng nhắc giờ:

- `G:\.system\bin\minarch.elf`: 798336 byte; SHA-256 `9DEB2408C44AABD05B33D9BC52628B829F7A06D005B7723154275C5D9480BB9D`.
- `G:\.system\paks\Tools\Settings.pak\settings.elf`: 355160 byte; SHA-256 `82F600C0DB3874DF70BE32FB7DF146B6F3AE82C008D5F3C49FFF7BB9230BC6A5`.

Đây là binary nhắc giờ hợp lệ, chủ đích khác checksum của bản stock v1.12.0; không coi việc khác stock là bằng chứng tệp hỏng rồi tự chép đè lại. Hai binary đã kiểm tra ELF64/AArch64, dependency/GLIBC tương ứng với stock, và checksum sau locked flush/dismount/remount. Không thay `nextui.elf`, thư viện chung hay launcher/game/USB trong bước khôi phục này.

Giữ nguyên `minuisettings.txt`: bật nhắc, mốc đầu 180 phút, lặp 30 phút, thông báo 5 giây. Bộ đếm áp dụng cho từng phiên chạy minarch, không cộng dồn theo ngày và chưa có bridge cho PortMaster/standalone. Test host đã kiểm tra mốc 180/30, tắt/bật, tắt lặp, menu/sleep, tick wrap và phiên mới; vẫn cần thử thông báo thực tế trên Brick Pro.

Bản sao lưu trước sửa, bản sao riêng hai binary ngay trước cài và báo cáo đối chiếu nằm tại `D:\NXRedux-backup-20261003`. Mốc kiểm tra sau đợt sửa là `reminder-install-baseline.json`, khác bản sao lưu trước sửa vì chat phối hợp đã phục hồi một số tệp và bổ sung game/launcher. Báo cáo `reminder-install-verification.json` xác nhận 14223 tệp khác không thay đổi so với mốc sau sửa, hai binary khớp bản nhắc giờ, không có tệp mới/mất ngoài dự kiến hay lỗi đọc; kiểm tra filesystem cuối trả exit 0. Đây không phải xác nhận đã chơi thử trên thiết bị hoặc surface-test toàn bộ thẻ. Quy trình và kiểm tra phục hồi nằm ở `.local/playtime-restore-20261003/`; biên bản bàn giao nằm ở `.local/card-repairs/20261003-emulators/playtime-handoff.json`. Luôn bàn giao quyền ghi thẻ giữa các chat trước khi ghi hoặc dismount; không chạy các đợt sửa/cài đồng thời trên cùng volume.

## 9. Boot logo

`settings_bootlogo.c::loadImages()` chọn thư mục theo `DEVICE`:

- Brick, Brick Pro và Smart Pro S: `.system/paks/Tools/Settings.pak/bootlogo/brick/`
- Smart Pro: `.system/paks/Tools/Settings.pak/bootlogo/smartpro/`

Chỉ file có `.bmp` trong tên và được `IMG_Load()` đọc thành công mới xuất hiện. Khi người dùng chọn, code mount `/dev/mmcblk0p1` vào `/mnt/boot`, chép ảnh thành `/mnt/boot/bootlogo.bmp`, `sync`, unmount rồi reboot.

Khi thêm ảnh, phải kiểm tra đúng thư mục của `DEVICE`, BMP hợp lệ và kích thước/định dạng tương thích firmware. Không chỉ kiểm tra phần mở rộng; luôn thử `IMG_Load` hoặc mở bằng SDL/image tool tương đương trước khi cài.

## 10. PortMaster và port game

PortMaster không còn là binary bunded trực tiếp. Xtras cài:

- runtime/GUI vào `/mnt/SDCARD/Emus/shared/PortMaster/`;
- console runner vào `/mnt/SDCARD/Emus/PORTS.pak/launch.sh`;
- tool GUI vào `/mnt/SDCARD/Tools/PortMaster.pak/launch.sh`.

Nguồn launcher nằm tại `skeleton/SYSTEM/<platform>/paks/Tools/Xtras.pak/catalog/portmaster/`. Update system đồng bộ lại hai launcher nếu PortMaster đã được cài.

Luồng chạy port:

1. NextUI nhận ROM script trong `Roms/Ports (PORTS)`.
2. `Emus/PORTS.pak/launch.sh` bind-mount `.ports` vào đường dẫn tạm mà script PortMaster mong đợi.
3. Thiết lập controller layout, library path, performance mode và `sleepmon`.
4. Chạy script game; script gọi `control.txt`, `gptokeyb`, platform helper và binary game.
5. Cleanup unmount bind mount và trả về launcher.

Không dùng `rm -rf` vào đường bind mount của port. Cleanup hiện dùng `umount` rồi `rmdir` để một mount còn sống không thể xóa dữ liệu game thật.

Các tài sản như `main.pak`, `properties/`, dữ liệu thương mại và save là dữ liệu do người dùng cung cấp. Installer/update/repair phải bảo toàn chúng. Ví dụ PvZ Portable cần `main.pak` và `properties/` từ bản GOTY hợp pháp; không coi thư mục rỗng trong gói PortMaster là quyền xóa bản người dùng.

Khi debug port:

- xem `.userdata/<platform>/logs/PORTS.txt`;
- xem `Roms/Ports (PORTS)/.ports/<port>/log.txt`;
- kiểm tra binary theo đúng `DEVICE_ARCH`;
- kiểm tra `LD_LIBRARY_PATH`, file game bắt buộc và quyền execute;
- phân biệt patch/install screen với binary game;
- không lặp patch mỗi lần nếu marker/output hợp lệ đã tồn tại.

## 11. USB, MTP và an toàn thẻ nhớ

### 11.1 MTP

Trên tg5040, `nx_usb_mode.sh` ghép uMTP-Responder FunctionFS với ADB. Các action:

- `restore`/`start`: tạo `ffs.mtp`, chạy `umtprd`, bind gadget ADB+MTP;
- `stop`: dừng MTP và trả về ADB;
- `status`: in `mtp` hoặc `adb`.

Runtime nằm ở:

- `skeleton/SYSTEM/tg5040/bin/nx_usb_mode.sh`
- `skeleton/SYSTEM/tg5040/bin/umtprd`
- `skeleton/SYSTEM/tg5040/etc/umtprd.conf`

Game thường không được gọi `restore` sau khi thoát vì việc đó re-enumerate USB/MTP không cần thiết. Chỉ tool thật sự chiếm USB controller mới được stop/restore mode.

### 11.2 USB Storage/UMS

MTP truy cập file qua protocol và SD vẫn do thiết bị mount. USB Mass Storage thì đưa block device trực tiếp cho host; vì vậy SD bắt buộc phải unmount hoàn toàn trước khi export.

Wrapper `USB Storage.pak/launch.sh` tự chép vào `/tmp`, đổi cwd sang `/tmp`, dừng MTP, `sync`, rồi gọi stock USB Storage. Mục đích là không để script/cwd/log descriptor trên SD cản unmount.

Bất biến an toàn:

- Nếu `umount /mnt/SDCARD` báo busy/fail, không được tiếp tục export block device.
- Không để device và Windows cùng ghi filesystem qua hai đường khác nhau.
- Không ghi log trực tiếp lên SD trong lúc SD đang được export; giữ log ở `/tmp` rồi copy lại sau khi mount.
- Sau mọi copy quan trọng: flush/sync, đóng handle, tháo an toàn trước khi rút.
- Khi nghi hỏng thẻ: dừng ghi, tạo image/backup trước, sau đó mới repair.

Lỗi cluster “marked as free”, “already allocated for another file” hoặc thư mục còn nhưng rỗng là lỗi metadata filesystem, không phải bằng chứng một hàm ứng dụng đã gọi xóa thư mục.

## 12. Build và package

### 12.1 Build đầy đủ

Yêu cầu host: Docker và ADB. Các Makefile chính dùng shell Unix; trên Windows nên chạy trong WSL2/Docker Desktop thay vì PowerShell thuần.

```sh
# Firmware/UI, tái dùng core có sẵn
make all

# Build cả emulator cores
make all COMPILE_CORES=true

# Một core
make build-core PLATFORM=tg5040 CORE=<core-name>
```

Output quan trọng:

- binary component: `workspace/all/<component>/build/<platform>/`;
- staging package: `build/`;
- zip theo thiết bị: `build/BASE/MinUI-<device>.zip` và/hoặc bản phát hành trong `releases/`.

### 12.2 Build một component

```sh
docker run --rm -v "$(pwd)/workspace:/root/workspace" \
  ghcr.io/loveretro/tg5040-toolchain:latest \
  /bin/bash -c 'source ~/.bashrc && cd /root/workspace/all/<component> && make PLATFORM=tg5040'
```

Trên tree mới, build `workspace/<platform>/libmsettings` trước các component link `-lmsettings`. `workspace/hash.txt` cũng phải tồn tại cho build trong container.

Không chạy hai full build đồng thời trên cùng workspace. Riêng `musicplayer/opus_obj` không namespaced theo platform nên build tg5040 và tg5050 phải chạy tuần tự.

### 12.3 Deploy

```sh
make deploy DEVICE=brickpro
```

Lệnh này push `MinUI-brickpro.zip` tới `/mnt/SDCARD/MinUI.zip` và reboot. Dùng đúng `DEVICE`; `PLATFORM=tg5040` không đủ để phân biệt Brick, Brick Pro và Smart Pro.

Khi lặp nhanh một binary, dừng process đang chạy trước rồi mới ghi đè. exFAT/vfat không bảo vệ ETXTBSY; push đè executable đang mmap có thể làm hỏng binary đang chạy.

Đích deploy thường gặp:

- daemon/binary hệ thống trên thẻ đã cài → `/mnt/SDCARD/.system/bin/`;
- bundled tool pak trên thẻ đã cài → `/mnt/SDCARD/.system/paks/Tools/<Name>.pak/`; thư mục `<platform>` chỉ tồn tại ở source staging `skeleton/SYSTEM/<platform>/`, rồi được làm phẳng khi package;
- PortMaster user layer → cả `Tools/PortMaster.pak/launch.sh` và `Emus/PORTS.pak/launch.sh`;
- OSD asset/binary → cần reboot sau push.

Sau deploy nhanh, so sánh kích thước và SHA-256 giữa source và thẻ; đừng chỉ dựa vào exit code copy.

## 13. Kiểm thử

### 13.1 Test host

Các test shell/E2E nằm trong `scripts/tests/`. Common có runner:

```sh
sh workspace/all/common/tests/run_tests.sh
```

Play-time tracker hiện có thể test độc lập:

```sh
cc -std=gnu99 -Wall -Werror \
  -o /tmp/nx_test_play_time \
  workspace/all/common/play_time.c \
  workspace/all/common/tests/test_play_time.c
/tmp/nx_test_play_time
```

Test quan trọng theo vùng:

- `scripts/tests/test-portmaster-launch.sh`
- `scripts/tests/test-update-portmaster-refresh.sh`
- `scripts/tests/test-button-layout-sh.sh`
- `scripts/tests/test-minarch-cpu-profile.sh`
- `scripts/tests/test-menu-animations.sh`
- `scripts/tests/test-osd-overlay-tg5050.sh`
- `scripts/tests/test-sync-rsync-fat32.sh`
- `workspace/all/musicplayer/tests/run_tests.sh`

Shell script target thiết bị phải hợp POSIX `sh` trừ khi PortMaster bảo đảm `/bin/bash`. tg5040 BusyBox thiếu nhiều applet; không giả định có `nohup`, `setsid`, `timeout` hoặc `unzip`.

### 13.2 Kiểm thử phần cứng

Desktop build giúp bắt lỗi UI/logic nhưng không xác nhận được:

- DRM/fbdev/overlay plane;
- input daemon và event code thật;
- audio route Bluetooth/USB DAC;
- suspend/poweroff/PMIC;
- USB gadget và unmount SD;
- hiệu năng/CPU affinity;
- stock firmware library compatibility.

Mỗi thay đổi platform hoặc launcher phải được thử trên đúng thiết bị. Với Brick Pro, luôn kiểm tra ít nhất: boot → menu → game minarch → tool pak → PortMaster → sleep/wake → thoát → shutdown an toàn.

### 13.3 Log và chẩn đoán

| Log/marker | Dùng để xem |
|---|---|
| `.userdata/<platform>/logs/nextui.txt` | crash/flow launcher UI |
| `.userdata/<platform>/logs/PORTS.txt` | runner PortMaster |
| `.ports/<game>/log.txt` | binary/script của từng port |
| `/tmp/nextui_boottime` | mốc boot shell/GFX/menu/first frame |
| `/tmp/nx-mtp.log` | MTP gadget |
| `.userdata/<platform>/logs/usb-storage.txt` | stock USB Storage và filesystem check |
| `/tmp/nx_osd_mount_failed` | OSD overlay mount thất bại |

Trước khi “sửa” theo triệu chứng, thu log sau đúng một lần tái hiện; log nhiều phiên có thể bị append/ghi đè tùy app.

## 14. Quy ước sửa mã

- Giữ C theo style hiện tại; chạy `make format` hoặc clang-format lên file đã sửa.
- Shared UI component mới: một `.c/.h` riêng, đăng ký trong `common/ui/ui.mk`.
- Không chặn main loop lâu; tải mạng/ảnh dùng worker hoặc modal có power update.
- Không `SIGCHLD=SIG_IGN`; nó làm `popen()`/`system()` trả ECHILD. Async command dùng double-fork/exec như `runCommandAsync`.
- Không ghi thẳng setting file bằng nhiều writer nếu đã có CFG API. Nếu OSD shell buộc phải sửa trực tiếp, bảo toàn hợp đồng merge và atomic write.
- Không viết trực tiếp LED sysfs ngoài choke point phù hợp; `LEDS_setProfile()` có thể áp lại profile.
- Không sửa stock rootfs khi có thể overlay/stage từ SD.
- Không dùng binary/input daemon của model này cho model khác.
- Mọi network command trong installer phải có timeout và checksum khi upstream cung cấp digest.
- Installer phải idempotent và fail-closed; không để runtime nửa cài vẫn launch được.
- Không xóa dữ liệu người dùng trong `Roms`, `Saves`, `.userdata` hoặc pak thương mại khi update code hệ thống.

## 15. Công thức thay đổi thường gặp

### Thêm một trang Settings

1. Xác định setting thuộc shared config hay module riêng.
2. Thêm schema/default/serialization.
3. Tạo callbacks và item/page.
4. Nối vào root menu có điều kiện theo capability thiết bị.
5. Rebuild mọi binary đọc setting.
6. Test default, cycle, reset, thoát/mở lại và downgrade/unknown-key preservation.

### Thêm notification ingame

1. Thêm `NotificationType` nếu semantics mới.
2. Quyết định duration và queue behavior trong `notification.c`.
3. Gọi `Notification_push` từ game loop hoặc event callback.
4. Không tự vẽ ngoài layer notification; giữ `Notification_renderToLayer(5)` là đường render chung.
5. Test notification chồng với achievement, save/load và system indicator.

### Thêm tool pak

1. Tạo binary/module trong `workspace/all/<tool>` nếu cần.
2. Tạo `skeleton/SYSTEM/<platform>/paks/Tools/<Name>.pak/launch.sh`.
3. Nối build/copy/package trong Makefile.
4. Thiết lập CPU/audio/input và cleanup đối xứng.
5. Đảm bảo B/quit request hoạt động và không để daemon/process mồ côi.

### Sửa PortMaster

1. Sửa nguồn trong catalog dưới `Xtras.pak/catalog/portmaster`.
2. Chạy test launcher/install.
3. Deploy cả GUI launcher và Ports runner nếu test nhanh.
4. Bảo toàn game data và asset người dùng.
5. Tái hiện với log của đúng port, không suy từ màn hình patch.

### Sửa USB/MTP

1. Ghi rõ ai đang sở hữu USB controller.
2. Lưu mode trước khi tool chiếm controller.
3. Stop MTP trước UMS/gamepad; chỉ restore sau cleanup.
4. Với UMS, xác nhận SD đã unmount thật trước khi export.
5. Test Windows connect/disconnect, ADB còn hoạt động, thoát tool, vào/ra game và shutdown.

## 16. Checklist trước khi bàn giao

- Đúng platform và đúng `DEVICE`.
- Không sửa/xóa thay đổi unrelated của người dùng.
- Build đúng component và mọi consumer của common code.
- Chạy test host liên quan; shell syntax check khi có thể.
- Test trên phần cứng cho input/display/audio/power/USB.
- Không push đè executable đang chạy.
- Xác minh SHA-256/kích thước sau copy lên thẻ.
- Xem log của một lần tái hiện sạch.
- `sync` và tháo thẻ an toàn.
- Ghi lại file đã đổi, hành vi đã xác minh và phần chưa thể xác minh.

## 17. Tài liệu chuyên sâu còn lại

File này là bản tổng hợp. Khi cần chi tiết byte patch, phép đo phần cứng hoặc recipe dài, xem:

- `.dev/ARCHITECTURE.md`
- `.dev/BUILD.md`
- `.dev/DEVICES.md`
- `.dev/TESTING.md`
- `.dev/INPUT_MAPPING.md`
- `.dev/PAKS.md`
- `.dev/OSD.md`
- `.dev/AUDIO.md`
- `.dev/CAPTURE.md`
- `.dev/MTP.md`
- `.dev/USB_GAMEPAD.md`

Không sao chép mù các lệnh cũ: luôn đối chiếu script/Makefile hiện tại và trạng thái thật của thiết bị/thẻ trước khi chạy.
