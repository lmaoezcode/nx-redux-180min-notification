# Phối hợp giữa các session Claude Code

Mọi session (mỗi cuộc trò chuyện trong tab Code) làm việc trong repo này đều nạp
file này qua `CLAUDE.md`. Session không đọc được hội thoại của nhau, nên phối hợp
bằng ba cách: file trạng thái chung, nhánh/worktree riêng, và nhắn giữa các session.

## 1. Thư mục trạng thái chung và bàn giao (KHÔNG commit)

Thư mục chung nằm ở repo chính (đường dẫn tuyệt đối):

`C:\Users\LamHoang\Documents\trimui-brickpro-os\nx redux\.agents\handoff\`

- `khoa-the-g.md`: khóa ghi thẻ G và USB (một dòng). Chỉ có bản ở thư mục chung;
  chỉ session **Thẻ & ROM** đọc-ghi nó (xem mục 2).
- `<tên-session>.md`: nhật ký bàn giao của từng session.

Nơi ghi bàn giao phụ thuộc session chạy ở đâu:

- Session chạy trong repo chính: ghi thẳng vào thư mục chung ở trên.
- Session dùng worktree (`.claude/worktrees/<tên>/`): ghi bàn giao trong
  `.agents/handoff/` **của worktree mình**, không ghi vào thư mục chung của repo
  chính. Worktree có bản sao riêng của thư mục này; nó không commit và không đi
  theo nhánh.
- Thẻ & ROM gom các bàn giao của worktree về thư mục chung: chép nguyên văn
  `<worktree>\.agents\handoff\<tên-session>.md` sang
  `<repo chính>\.agents\handoff\<tên-session>.md`; nếu file đích đã có thì nối
  thêm vào cuối, không ghi đè.

Không `git add` thư mục `.agents/handoff/` ở bất kỳ nơi nào (đã có trong
`.gitignore`).

## 2. Khóa thẻ G và USB

Cụ thể hóa quy tắc trong `AGENT.md`: "Luôn bàn giao quyền ghi thẻ giữa các
chat trước khi ghi hoặc dismount; không chạy các đợt sửa/cài đồng thời trên
cùng volume."

**Thẻ & ROM là session DUY NHẤT lấy khóa thẻ và chép file lên thẻ G.** Các session
khác (Lõi hệ thống, USB & Tay cầm, Port & Việt hóa, Kết nối) không lấy khóa, không
ghi lên thẻ G, không dismount, không chạy USB Storage/USB Gamepad hay deploy qua
adb vào thẻ. Chúng chuẩn bị gói triển khai (tệp, đường dẫn đích trên thẻ, SHA-256,
cách kiểm) và nhờ Thẻ & ROM làm, bằng nhắn giữa các session (mục 4) hoặc ghi vào
mục "Cần Thẻ & ROM triển khai" trong bàn giao (mục 5).

Quy trình của Thẻ & ROM:

1. Trước khi ghi lên thẻ G, dismount, chạy USB Storage/USB Gamepad hoặc deploy
   qua adb: đọc `khoa-the-g.md`. Chỉ tiếp tục khi nó là `Thẻ G: trống`.
2. Ghi đè thành `Thẻ G: <tên session> giữ từ <YYYY-MM-DD HH:MM> — <việc>`.
3. Xong việc (kể cả khi thất bại): `sync`/`Write-VolumeCache`, tháo an toàn, ghi
   lại `Thẻ G: trống`.
4. Khóa đang bị giữ hoặc không rõ ai giữ: KHÔNG tự gỡ; hỏi người dùng hoặc nhắn
   session đang giữ.
5. Chỉ đọc thẻ (xem log, liệt kê file) không cần khóa và mọi session đều được làm.

## 3. Các session và phạm vi

| Session | Phạm vi | Nhánh gợi ý |
|---|---|---|
| Trợ lý (dự án Claude bên ngoài, không phải session tab Code) | Kế hoạch, prompt, kiểm chứng bên thứ ba; không sửa code, không ghi thẻ | — |
| Thẻ & ROM | ROM, BIOS, artwork, save, cài port lên thẻ, sao lưu thẻ; triển khai tệp lên thẻ G cho các session khác; giữ khóa thẻ duy nhất; không sửa `workspace/` | — |
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
binary trên thẻ, vừa merge upstream), và để nhờ Thẻ & ROM triển khai một gói lên
thẻ G. Vẫn ghi vào file bàn giao để Trợ lý đọc được.

## 5. Bàn giao cuối buổi

Ghi THÊM (không ghi đè) vào `.agents\handoff\<tên-session>.md`: thư mục chung ở
mục 1 nếu chạy trong repo chính, hoặc `.agents\handoff\` của worktree mình nếu dùng
worktree.

```
## YYYY-MM-DD
- Đã làm:
- Đã xác minh trên Brick Pro (log/SHA-256):
- Chưa xác minh:
- Bước tiếp theo:
- Cần Thẻ & ROM triển khai lên thẻ G: <tệp -> đích, SHA-256 | không>
- Khóa thẻ đã trả: có/không/không áp dụng (chỉ Thẻ & ROM giữ khóa)
```
