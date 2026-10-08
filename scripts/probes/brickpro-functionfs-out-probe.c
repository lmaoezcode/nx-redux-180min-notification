/* Disabled H2 endpoint experiment. The installed kernel has only one INT
 * allocation and no bulk->INT fallback, verified from boot + live RAM code.
 * Do not enable on this firmware: IN+OUT cannot bind. A control-OUT spike must
 * be separate, with reviewed MTP takeover and independent SSH recovery.
 * Reuses the exact existing lab descriptor
 * and ep0 implementation, NOT the old application's main/lifecycle/input.
 * Existing ADB/MTP links and all identity attributes remain unchanged.
 * Guardian inherits the lab endpoint descriptors before either USB change.
 */
#define main nx_owner_probe_main_unused
#include "brickpro-usb-owner-probe.c"
#undef main
#define main nx_old_gamepad_main_unused
#include "../../workspace/all/usbgamepad/usbgamepad.c"
#undef main

struct OutContext { FunctionFsGamepad ffs; bool valid_out; unsigned in_reports; };

static void release_worker_endpoints(const struct Snapshot *s) {
	struct OutContext *context = s->context;
	int *fds[] = {&context->ffs.epIn, &context->ffs.epOut, &context->ffs.ep0};
	for (unsigned i = 0; i < 3; ++i) if (*fds[i] >= 0) { close(*fds[i]); *fds[i] = -1; }
	/* Guardian still owns copies, so this cannot withdraw the live function. */
}
static int cleanup_out_function(const struct Snapshot *s) {
	struct OutContext *context = s->context;
	struct stat info;
	if (!lstat(FFS_LINK, &info)) {
		if (!link_matches(FFS_LINK, FFS_FUNCTION)) {
			record(s, "cleanup_link_target_refused", 0); return 1;
		}
		if (unlink(FFS_LINK)) { record(s, "cleanup_unlink_failed", errno); return 1; }
	} else if (errno != ENOENT) { record(s, "cleanup_lstat_failed", errno); return 1; }
	release_worker_endpoints(s);
	if (context->ffs.mounted && umount(FFS_MOUNT) && errno != EINVAL && errno != ENOENT) {
		record(s, "cleanup_umount_failed", errno); return 1;
	}
	context->ffs.mounted = false;
	if (rmdir(FFS_MOUNT) && errno != ENOENT) { record(s, "cleanup_mount_dir_failed", errno); return 1; }
	if (rmdir(FFS_FUNCTION) && errno != ENOENT) { record(s, "cleanup_function_failed", errno); return 1; }
	record(s, "owned_function_cleanup_verified", 0);
	return 0;
}
static bool activate_out_function(const struct Snapshot *s) {
	if (symlink(FFS_FUNCTION, FFS_LINK)) return false;
	if (write_value(s->udc_path, s->udc)) { record(s, "lab_bind_write_failed", errno); return false; }
	char value[128];
	if (read_value(s->udc_path, value, sizeof(value)) || strcmp(value, s->udc)) return false;
	record(s, "lab_bound_existing_identity", 0);
	return true;
}
static int service_out_function(const struct Snapshot *s) {
	struct OutContext *context = s->context;
	FunctionFsGamepad *gamepad = &context->ffs;
	struct pollfd descriptors[2] = {
		{gamepad->ep0, POLLIN, 0}, {gamepad->epOut, POLLIN, 0}
	};
	int rc = poll(descriptors, 2, 0);
	if (rc < 0 && errno != EINTR) return 1;
	bool dirty = false;
	if ((descriptors[0].revents & POLLIN) && !handleFunctionFsEvents(gamepad, &dirty)) return 1;
	if (!running) return 1;
	if ((descriptors[1].revents & POLLIN) && gamepad->enabled) {
		uint8_t data[64];
		ssize_t n = read(gamepad->epOut, data, sizeof(data));
		if (n > 0) {
			fprintf(stderr, "host_interrupt_out length=%zd data=", n);
			for (ssize_t i = 0; i < n; ++i) fprintf(stderr, "%02x", data[i]);
			fprintf(stderr, "\n");
			bool match = n == 32;
			for (ssize_t i = 0; match && i < n; ++i) match = data[i] == (uint8_t)(0xa0 + (i & 15));
			if (match) {
				context->valid_out = true;
				record(s, "host_interrupt_out_verified", 32);
				return 2;
			}
		} else if (n < 0 && errno != EAGAIN && errno != EINTR) return 1;
	}
	if (gamepad->enabled) {
		uint8_t neutral[8] = {0};
		ssize_t n = write(gamepad->epIn, neutral, sizeof(neutral));
		if (n == sizeof(neutral)) ++context->in_reports;
		else if (n < 0 && errno != EAGAIN && errno != EINTR) return 1;
	}
	return 0;
}

int main(int argc, char **argv) {
#ifndef NX_ENABLE_UNVERIFIED_USB_OUT_LIVE
	/* Recovery bug is fixed in source only. The endpoint allocation limit
	 * independently makes this descriptor unsuitable for the current kernel. */
	fputs("Live IN+OUT probe disabled: current kernel has only one interrupt endpoint.\n", stderr);
	return 2;
#endif
	if (argc != 2 || strncmp(argv[1], "/tmp/nx-usb-owner-", 18) ||
		strchr(argv[1] + 18, '/') || strstr(argv[1], "..") || !argv[1][18] || geteuid()) return 2;
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGTERM, request_stop);
	(void)signal(SIGINT, request_stop);
	(void)signal(SIGALRM, handleControlTimeout);
	int lock = open("/tmp/nx-usb-owner-probe.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 2;
	if (!access("/tmp/nx_usb_mode.lock", F_OK) || !access("/tmp/nx-h2-usb-out.active", F_OK) ||
		!access(FFS_FUNCTION, F_OK) || !access(FFS_MOUNT, F_OK) || !access(FFS_LINK, F_OK)) return 2;
	struct OutContext context = {0};
	struct Snapshot s = {.log_fd = -1, .unbind = true, .hold_ms = 8000, .guardian_ms = 12000,
		.context = &context, .activate = activate_out_function, .step = service_out_function,
		.worker_release = release_worker_endpoints, .cleanup = cleanup_out_function};
	char role[64];
	if (read_value(USB_ROLE_STATE, role, sizeof(role)) || strcmp(role, "usb_device") ||
		discover_adbd(&s.adb)) return 2;
	DIR *config = opendir(GADGET_CONFIG);
	if (!config) return 2;
	unsigned functions = 0;
	struct dirent *entry;
	while ((entry = readdir(config))) {
		char path[512], target[512];
		snprintf(path, sizeof(path), GADGET_CONFIG "/%s", entry->d_name);
		ssize_t n = readlink(path, target, sizeof(target) - 1);
		if (n < 0) continue;
		target[n] = 0;
		const char *name = strrchr(target, '/');
		if (!name || (strcmp(name + 1, "ffs.adb") && strcmp(name + 1, "ffs.mtp"))) {
			closedir(config); return 2;
		}
		++functions;
	}
	closedir(config);
	if (functions != 2) return 2; /* Only the verified, current ADB+MTP baseline. */
	snprintf(s.udc_path, sizeof(s.udc_path), GADGET_ROOT "/UDC");
	if (read_value(s.udc_path, s.udc, sizeof(s.udc)) || udc_unbound(s.udc)) return 2;
	s.log_fd = open(argv[1], O_CREAT | O_EXCL | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
	if (s.log_fd < 0) return 2;
	if (dup2(s.log_fd, STDERR_FILENO) < 0) return 2;
	dprintf(s.log_fd, "snapshot_adbd_pid=%ld start=%llu exe=%s original_udc=%s fs_interval_ms=4 hs_interval_ms=4\n",
		(long)s.adb.pid, s.adb.start, s.adb.exe, s.udc);
	initFunctionFsGamepad(&context.ffs, GAMEPAD_PROFILE_OUTPUT_PROBE);
	if (!configureFunctionFs(&context.ffs, true)) { closeFunctionFs(&context.ffs); return 1; }
	record(&s, "lab_parser_ready_guard_inherits_endpoints", 0);
	int rc = run_probe(&s);
	record(&s, "lab_in_report_count", (int)context.in_reports);
	if (!context.valid_out) { record(&s, "no_verified_host_out", 0); rc = 1; }
	close(s.log_fd); close(lock);
	return rc;
}
