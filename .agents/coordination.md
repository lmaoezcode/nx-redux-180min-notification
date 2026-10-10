# Phối hợp giữa các session Claude Code

Mọi session (mỗi cuộc trò chuyện trong tab Code) làm việc trong repo này đều nạp
file này qua `CLAUDE.md`. Session không đọc được hội thoại của nhau, nên phối hợp
bằng ba cách: file trạng thái chung, nhánh/worktree riêng, và nhắn giữa các session.

## 1. Thư mục trạng thái chung (KHÔNG commit)

Luôn dùng ĐƯỜNG DẪN TUYỆT ĐỐI tới bản trong thư mục repo chính, kể cả khi
session đang chạy trong worktree (worktree có bản sao riêng, sẽ lệch nhau):

`C:\Users\LamHoang\Documents\trimui-brickpro-os\nx redux\.agents\handoff\`

- `khoa-the-g.md`: khóa ghi thẻ G và USB (một dòng).
- `<tên-session>.md`: nhật ký bàn giao của từng session.

Không `git add` thư mục này (nên thêm `.agents/handoff/` vào `.gitignore`).

## 2. Khóa thẻ G và USB

Cụ thể hóa quy tắc trong `AGENT.md`: "Luôn bàn giao quyền ghi thẻ giữa các
chat trước khi ghi hoặc dismount; không chạy các đợt sửa/cài đồng thời trên
cùng volume."

1. Trước khi ghi lên thẻ G, dismount, chạy USB Storage/USB Gamepad hoặc deploy
   qua adb: đọc `khoa-the-g.md`. Chỉ tiếp tục khi nó là `Thẻ G: trống`.
2. Ghi đè thành `Thẻ G: <tên session> giữ từ <YYYY-MM-DD HH:MM> — <việc>`.
3. Xong việc (kể cả khi thất bại): `sync`, tháo an toàn, ghi lại `Thẻ G: trống`.
4. Khóa đang bị giữ hoặc không rõ ai giữ: KHÔNG tự gỡ; hỏi người dùng hoặc nhắn
   session đang giữ.
5. Chỉ đọc thẻ (xem log, liệt kê file) không cần khóa.

## 3. Các session và phạm vi

| Session | Phạm vi | Nhánh gợi ý |
|---|---|---|
| Trợ lý (dự án Claude bên ngoài, không phải session tab Code) | Kế hoạch, prompt, kiểm chứng bên thứ ba; không sửa code, không ghi thẻ | — |
| Thẻ & ROM | ROM, BIOS, artwork, save, cài port lên thẻ, sao lưu thẻ; không sửa `workspace/` | — |
| Lõi hệ thống | minarch, nextui, settings, playtimed, merge upstream | `brickpro/core` |
| USB & Tay cầm | USB Gamepad/NX Controller, USB Storage, MTP, sau này Bluetooth | `brickpro/usb-controller` |
| Port & Việt hóa | PortMaster, Ruffle-Handheld, Việt hóa, thử nghiệm Symbian | `brickpro/ports` |
| Kết nối | Syncthing, SSH, quản lý từ điện thoại | `brickpro/connect` |

Ngoài phạm vi thì dừng và hỏi người dùng. Session sửa code nên được mở với tùy
chọn **worktree** của tab Code (worktree nằm ở `.claude/worktrees/`). Không
reset/checkout đè thay đổi chưa commit của session khác.

## 4. Nhắn giữa các session

Tab Code cho phép một session liệt kê các session khác, đọc chúng đang làm gì và
nhắn cho chúng. Dùng khi việc của mình ảnh hưởng session khác (ví dụ vừa thay
binary trên thẻ, vừa merge upstream). Vẫn ghi vào file bàn giao để Trợ lý đọc được.

## 5. Bàn giao cuối buổi

Ghi THÊM (không ghi đè) vào `.agents\handoff\<tên-session>.md` (đường dẫn tuyệt đối ở mục 1):

```
## YYYY-MM-DD
- Đã làm:
- Đã xác minh trên Brick Pro (log/SHA-256):
- Chưa xác minh:
- Bước tiếp theo:
- Khóa thẻ đã trả: có/không
```
