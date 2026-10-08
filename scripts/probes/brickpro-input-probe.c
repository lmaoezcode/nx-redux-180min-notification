/*
 * Read-only evdev capability and timing probe for NX Controller research.
 *
 * It never calls EVIOCGRAB and never writes to the input device. Build for the
 * Brick Pro's AArch64 userspace, copy to /tmp, and run while moving both sticks
 * and pressing every physical control:
 *
 *   brickpro-input-probe --seconds 20
 *   brickpro-input-probe --device /dev/input/event3 --seconds 20
 *   brickpro-input-probe --seconds 20 --trace-controls
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define NBITS(x) ((((x)-1U) / BITS_PER_LONG) + 1U)
#define TEST_BIT(bit, array) (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signo) {
	(void)signo;
	stop_requested = 1;
}

static uint64_t monotonic_ns(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static bool device_name(int fd, char *name, size_t size) {
	memset(name, 0, size);
	return ioctl(fd, EVIOCGNAME(size), name) >= 0;
}

static int find_trimui_input(char *path, size_t size) {
	char candidate[64];
	char name[256];
	for (int i = 0; i < 32; ++i) {
		snprintf(candidate, sizeof(candidate), "/dev/input/event%d", i);
		int fd = open(candidate, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		bool match = device_name(fd, name, sizeof(name)) && strcmp(name, "TRIMUI Player1") == 0;
		printf("candidate path=%s name=%s selected=%s\n", candidate,
			name[0] ? name : "(unknown)", match ? "yes" : "no");
		close(fd);
		if (match) {
			snprintf(path, size, "%s", candidate);
			return 0;
		}
	}
	return -1;
}

static const char *abs_name(unsigned code) {
	switch (code) {
	case ABS_X: return "ABS_X";
	case ABS_Y: return "ABS_Y";
	case ABS_Z: return "ABS_Z";
	case ABS_RX: return "ABS_RX";
	case ABS_RY: return "ABS_RY";
	case ABS_RZ: return "ABS_RZ";
	case ABS_HAT0X: return "ABS_HAT0X";
	case ABS_HAT0Y: return "ABS_HAT0Y";
	default: return "ABS_OTHER";
	}
}

static const char *key_name(unsigned code) {
	switch (code) {
	case KEY_F1: return "KEY_F1";
	case KEY_F2: return "KEY_F2";
	case KEY_VOLUMEDOWN: return "KEY_VOLUMEDOWN";
	case KEY_VOLUMEUP: return "KEY_VOLUMEUP";
	case KEY_HOMEPAGE: return "KEY_HOMEPAGE";
	case BTN_SOUTH: return "BTN_SOUTH";
	case BTN_EAST: return "BTN_EAST";
	case BTN_NORTH: return "BTN_NORTH";
	case BTN_WEST: return "BTN_WEST";
	case BTN_TL: return "BTN_TL";
	case BTN_TR: return "BTN_TR";
	case BTN_TL2: return "BTN_TL2";
	case BTN_TR2: return "BTN_TR2";
	case BTN_SELECT: return "BTN_SELECT";
	case BTN_START: return "BTN_START";
	case BTN_MODE: return "BTN_MODE";
	case BTN_THUMBL: return "BTN_THUMBL";
	case BTN_THUMBR: return "BTN_THUMBR";
	default: return "KEY_OTHER";
	}
}

static void print_capabilities(int fd) {
	int version = 0;
	struct input_id id = {0};
	unsigned long ev_bits[NBITS(EV_MAX + 1)] = {0};
	unsigned long key_bits[NBITS(KEY_MAX + 1)] = {0};
	unsigned long abs_bits[NBITS(ABS_MAX + 1)] = {0};
	unsigned long sw_bits[NBITS(SW_MAX + 1)] = {0};
	unsigned long keys_down[NBITS(KEY_MAX + 1)] = {0};

	if (ioctl(fd, EVIOCGVERSION, &version) >= 0)
		printf("evdev_version=%d.%d.%d\n", version >> 16, (version >> 8) & 0xff, version & 0xff);
	if (ioctl(fd, EVIOCGID, &id) >= 0)
		printf("input_id bustype=0x%04x vendor=0x%04x product=0x%04x version=0x%04x\n",
			id.bustype, id.vendor, id.product, id.version);

	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
		perror("EVIOCGBIT(EV)");
	if (TEST_BIT(EV_KEY, ev_bits) && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
		printf("supported_keys=");
		for (unsigned code = 0; code <= KEY_MAX; ++code)
			if (TEST_BIT(code, key_bits))
				printf("%u:%s,", code, key_name(code));
		printf("\n");
		if (ioctl(fd, EVIOCGKEY(sizeof(keys_down)), keys_down) >= 0) {
			printf("keys_down_at_start=");
			for (unsigned code = 0; code <= KEY_MAX; ++code)
				if (TEST_BIT(code, keys_down))
					printf("%u:%s,", code, key_name(code));
			printf("\n");
		}
	}
	if (TEST_BIT(EV_SW, ev_bits) && ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw_bits)), sw_bits) >= 0) {
		printf("supported_switches=");
		for (unsigned code = 0; code <= SW_MAX; ++code)
			if (TEST_BIT(code, sw_bits))
				printf("%u,", code);
		printf("\n");
	}
	if (TEST_BIT(EV_ABS, ev_bits) && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
		for (unsigned code = 0; code <= ABS_MAX; ++code) {
			if (!TEST_BIT(code, abs_bits))
				continue;
			struct input_absinfo info;
			if (ioctl(fd, EVIOCGABS(code), &info) >= 0) {
				printf("abs code=%u name=%s value=%d min=%d max=%d fuzz=%d flat=%d resolution=%d\n",
					code, abs_name(code), info.value, info.minimum, info.maximum,
					info.fuzz, info.flat, info.resolution);
			}
		}
	}
}

int main(int argc, char **argv) {
	char path[64] = {0};
	int seconds = 20;
	bool trace_controls = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			snprintf(path, sizeof(path), "%s", argv[++i]);
		} else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			seconds = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--trace-controls") == 0) {
			trace_controls = true;
		} else {
			fprintf(stderr, "usage: %s [--device /dev/input/eventN] [--seconds 1..300] [--trace-controls]\n", argv[0]);
			return 2;
		}
	}
	if (seconds < 1 || seconds > 300) {
		fprintf(stderr, "seconds must be in range 1..300\n");
		return 2;
	}
	printf("brickpro_input_probe_version=2 read_only=yes evdev_grab=no trace_controls=%s\n",
		trace_controls ? "yes" : "no");
	if (!path[0] && find_trimui_input(path, sizeof(path)) != 0) {
		fprintf(stderr, "TRIMUI Player1 input device not found\n");
		return 1;
	}

	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}
	char name[256] = {0};
	device_name(fd, name, sizeof(name));
	printf("selected_path=%s selected_name=%s sample_seconds=%d\n", path, name, seconds);
	print_capabilities(fd);
	printf("sample_instruction=move both sticks continuously and press every control\n");
	fflush(stdout);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	uint64_t start = monotonic_ns();
	uint64_t deadline = start + (uint64_t)seconds * 1000000000ULL;
	uint64_t last_frame = 0, interval_sum = 0, interval_min = UINT64_MAX, interval_max = 0;
	uint64_t frames = 0, intervals = 0, syn_dropped = 0, key_events = 0, abs_events = 0;
	uint64_t abs_counts[ABS_MAX + 1] = {0};
	uint64_t key_counts[KEY_MAX + 1] = {0};

	while (!stop_requested && monotonic_ns() < deadline) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int ready = poll(&pfd, 1, 250);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (ready == 0)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "input disconnected revents=0x%x\n", pfd.revents);
			break;
		}
		struct input_event events[64];
		ssize_t bytes;
		while ((bytes = read(fd, events, sizeof(events))) > 0) {
			size_t count = (size_t)bytes / sizeof(events[0]);
			for (size_t i = 0; i < count; ++i) {
				const struct input_event *event = &events[i];
				if (event->type == EV_SYN && event->code == SYN_DROPPED) {
					syn_dropped++;
				} else if (event->type == EV_SYN && event->code == SYN_REPORT) {
					uint64_t now = monotonic_ns();
					frames++;
					if (last_frame) {
						uint64_t delta = now - last_frame;
						intervals++;
						interval_sum += delta;
						if (delta < interval_min) interval_min = delta;
						if (delta > interval_max) interval_max = delta;
					}
					last_frame = now;
				} else if (event->type == EV_ABS && event->code <= ABS_MAX) {
					abs_events++;
					abs_counts[event->code]++;
					if (trace_controls && (event->code == ABS_Z || event->code == ABS_RZ ||
							event->code == ABS_HAT0X || event->code == ABS_HAT0Y)) {
						printf("control_event type=abs code=%u name=%s value=%d\n",
							event->code, abs_name(event->code), event->value);
						fflush(stdout);
					}
				} else if (event->type == EV_KEY && event->code <= KEY_MAX) {
					key_events++;
					key_counts[event->code]++;
					if (trace_controls) {
						printf("control_event type=key code=%u name=%s value=%d\n",
							event->code, key_name(event->code), event->value);
						fflush(stdout);
					}
				} else if (trace_controls && event->type == EV_SW) {
					printf("control_event type=switch code=%u value=%d\n",
						event->code, event->value);
					fflush(stdout);
				}
			}
		}
		if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
			perror("read");
			break;
		}
	}

	uint64_t end = monotonic_ns();
	double elapsed = (double)(end - start) / 1000000000.0;
	printf("summary elapsed_s=%.6f frames=%llu frame_rate_hz=%.3f abs_events=%llu key_events=%llu syn_dropped=%llu\n",
		elapsed, (unsigned long long)frames, elapsed > 0 ? (double)frames / elapsed : 0.0,
		(unsigned long long)abs_events, (unsigned long long)key_events,
		(unsigned long long)syn_dropped);
	if (intervals) {
		printf("frame_intervals count=%llu min_ms=%.3f avg_ms=%.3f max_ms=%.3f\n",
			(unsigned long long)intervals, (double)interval_min / 1000000.0,
			(double)interval_sum / (double)intervals / 1000000.0,
			(double)interval_max / 1000000.0);
	}
	for (unsigned code = 0; code <= ABS_MAX; ++code)
		if (abs_counts[code])
			printf("abs_events code=%u name=%s count=%llu rate_hz=%.3f\n", code, abs_name(code),
				(unsigned long long)abs_counts[code], elapsed > 0 ? (double)abs_counts[code] / elapsed : 0.0);
	for (unsigned code = 0; code <= KEY_MAX; ++code)
		if (key_counts[code])
			printf("key_events code=%u name=%s count=%llu\n", code, key_name(code),
				(unsigned long long)key_counts[code]);

	close(fd);
	return 0;
}
