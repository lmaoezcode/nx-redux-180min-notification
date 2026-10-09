"""Linux-only integration tests. The test binary CANNOT use production /tmp."""
import datetime
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class DaemonTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="nx-playtime-test-")
        self.root = Path(self.directory.name)
        self.shared = self.root / "shared"
        self.runtime = self.root / "runtime"
        self.shared.mkdir()
        (self.runtime / "trimui_osd").mkdir(parents=True)
        self.ledger = self.shared / "daily-playtime.txt"
        self.pid = self.runtime / "nx-playtime.pid"
        self.toast = self.runtime / "trimui_osd/osd_toast_msg"
        (self.shared / "minuisettings.txt").write_text(
            "playTimeReminderEnabled=1\nplayTimeFirstMinutes=1\n"
            "playTimeRepeatMinutes=30\nplayTimeNotificationDuration=5\n"
            "unrelatedUserSetting=keep-me\n")
        self.env = dict(os.environ, SHARED_USERDATA_PATH=str(self.shared),
                        NX_PLAYTIME_TEST_TMP=str(self.runtime))
        self.children = []

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=5)
        self.directory.cleanup()

    def seed(self, milliseconds=0, notified=0, at=0, date=None):
        day = date or datetime.date.today().strftime("%Y%m%d")
        self.ledger.write_text(f"NX_DAILY_PLAYTIME 1 {day} {milliseconds} {notified} {at}\n")

    def start(self):
        child = subprocess.Popen([BINARY, "--run", str(os.getpid())], env=self.env,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(child)
        return child

    def ready(self, child):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.pid.exists() and self.pid.read_text().strip() == str(child.pid):
                return
            self.assertIsNone(child.poll(), "helper exited before readiness")
            time.sleep(0.02)
        self.fail("helper did not become ready")

    def stop(self, child):
        child.terminate()
        self.assertEqual(child.wait(timeout=5), 0)
        self.assertFalse(self.pid.exists())

    def test_sparse_checkpoints_but_clean_exit_saves(self):
        # While nothing notable happens the SD ledger must not be rewritten
        # every few seconds (each save is write+fsync+rename on exFAT) ...
        self.seed(0)
        child = self.start()
        self.ready(child)
        first = self.ledger.stat().st_mtime_ns
        time.sleep(3.3)
        self.assertEqual(self.ledger.stat().st_mtime_ns, first,
                         "ledger rewritten without a reminder, new day or exit")
        # ... but a clean stop must flush the counted time immediately.
        self.stop(child)
        counted = int(self.ledger.read_text().split()[3])
        self.assertGreaterEqual(counted, 3000)
        self.assertLess(counted, 6000)

    def test_multiple_games_restart_and_no_duplicate(self):
        self.seed(59000)
        config_before = (self.shared / "minuisettings.txt").read_bytes()
        child = self.start()
        self.ready(child)
        time.sleep(1.2)
        self.assertTrue(self.toast.exists())
        toast = json.loads(self.toast.read_text())
        self.assertEqual(toast["duration"], 5000)
        self.assertEqual(toast["size"], 2)
        self.assertIn("Today: 1m", toast["message"])
        self.stop(child)
        after_one = self.ledger.read_text().split()
        self.assertEqual(after_one[4], "1")
        self.toast.unlink()  # Fake OSD consumes the notification.
        child = self.start()
        self.ready(child)
        time.sleep(1.2)
        self.stop(child)
        self.assertFalse(self.toast.exists())
        self.assertGreater(int(self.ledger.read_text().split()[3]), int(after_one[3]))
        self.assertEqual((self.shared / "minuisettings.txt").read_bytes(), config_before)

    def test_quick_menu_sleep_and_osd_menu_pause(self):
        self.seed(1000)
        child = self.start()
        self.ready(child)
        os.kill(child.pid, signal.SIGUSR1)
        time.sleep(1.1)
        os.kill(child.pid, signal.SIGUSR2)
        (self.runtime / "system_suspend").touch()
        time.sleep(1.1)
        (self.runtime / "system_suspend").unlink()
        (self.runtime / "trimui_osd/osdd_show_up").touch()
        time.sleep(1.1)
        self.stop(child)
        self.assertLess(int(self.ledger.read_text().split()[3]), 1500)
        self.assertFalse(self.toast.exists())

    def test_pending_system_toast_is_not_overwritten(self):
        self.seed(60000)
        self.toast.write_text('{"type":"volume","message":"keep-me"}')
        before = self.toast.read_bytes()
        child = self.start()
        self.ready(child)
        time.sleep(1.1)
        self.assertEqual(self.toast.read_bytes(), before)
        self.assertEqual(self.ledger.read_text().split()[4], "0")
        self.toast.unlink()
        time.sleep(1.1)
        self.assertIn("Take a break", json.loads(self.toast.read_text())["message"])
        self.stop(child)

    def test_reminder_goes_to_a_live_inline_client_not_the_system_toast(self):
        # minarch registers its PID; the helper must hand the reminder over
        # instead of raising the system toast. This test process plays minarch.
        self.seed(59000)
        inline = self.runtime / "nx-playtime-inline"
        due = self.runtime / "nx-playtime-due"
        inline.write_text(f"{os.getpid()}\n")
        child = self.start()
        self.ready(child)
        time.sleep(1.2)
        self.assertFalse(self.toast.exists(), "system toast raised although a client is alive")
        self.assertTrue(due.exists())
        self.assertIn("Today: 1m", due.read_text())
        self.stop(child)
        self.assertEqual(self.ledger.read_text().split()[4], "1")  # counted as notified

    def test_dead_inline_client_falls_back_to_the_system_toast(self):
        self.seed(59000)
        gone = subprocess.Popen(["true"])
        gone.wait()
        (self.runtime / "nx-playtime-inline").write_text(f"{gone.pid}\n")
        child = self.start()
        self.ready(child)
        time.sleep(1.2)
        self.assertTrue(self.toast.exists())
        self.assertFalse((self.runtime / "nx-playtime-due").exists())
        self.stop(child)

    def test_stale_due_file_is_dropped_at_start(self):
        self.seed(0)
        due = self.runtime / "nx-playtime-due"
        due.write_text("Today: 99m. Take a break.\n")
        child = self.start()
        self.ready(child)
        self.assertFalse(due.exists())
        self.stop(child)

    def test_invalid_ledger_is_preserved_and_helper_fails_safely(self):
        self.ledger.write_text("unknown future ledger format\n")
        before = self.ledger.read_bytes()
        child = self.start()
        self.assertEqual(child.wait(timeout=5), 1)
        self.assertFalse(self.pid.exists())
        self.assertEqual(self.ledger.read_bytes(), before)

    def test_stale_day_reset_and_single_owner_lock(self):
        self.seed(10800000, 1, 10800000, date="20200101")
        child = self.start()
        self.ready(child)
        self.assertEqual(int(self.ledger.read_text().split()[3]), 0)
        second = self.start()
        self.assertEqual(second.wait(timeout=5), 1)
        self.assertEqual(int(self.pid.read_text()), child.pid)
        self.stop(child)
        self.assertEqual(len(list(self.shared.iterdir())), 2)


unittest.main()
