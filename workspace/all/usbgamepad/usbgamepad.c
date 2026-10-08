#include "gamepad_mapper.h"

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/hid.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define INPUT_NAME "TRIMUI Player1"
#define GADGET_ROOT "/sys/kernel/config/usb_gadget/g1"
#define GADGET_CONFIG GADGET_ROOT "/configs/c.1"
#define GADGET_FUNCTIONS GADGET_ROOT "/functions"
#define FFS_NAME "nxgamepad"
#define FFS_FUNCTION GADGET_FUNCTIONS "/ffs." FFS_NAME
#define FFS_LINK GADGET_CONFIG "/ffs." FFS_NAME
#define FFS_PARENT "/dev/usb-ffs"
#define FFS_MOUNT FFS_PARENT "/" FFS_NAME
#define FFS_EP0 FFS_MOUNT "/ep0"
#define FFS_EP_IN FFS_MOUNT "/ep1"
#define FFS_EP_OUT FFS_MOUNT "/ep2"
#define UDC_CLASS "/sys/class/udc"
#define USB_ROLE_ROOT "/sys/devices/platform/soc/usbc0"
#define USB_ROLE_DEVICE USB_ROLE_ROOT "/usb_device"
#define USB_ROLE_STATE USB_ROLE_ROOT "/otg_role"
#define ACTIVE_MARKER "/tmp/nx_usb_gamepad_active"
#define SCREEN_MARKER "/tmp/nx_usb_gamepad_screen_off"
#define STATUS_FIFO "/tmp/show2.fifo"
#define DISPLAY_DEVICE "/dev/disp"
#define DISP_LCD_SET_BRIGHTNESS 0x102
#define RUMBLE_GPIO "/sys/class/gpio/gpio227/value"
#define RUMBLE_VOLTAGE "/sys/class/motor/voltage"
#define LAYOUT_STATE_ENV "NXGP_LAYOUT_STATE_FILE"
#define EXIT_HOLD_MS 1500
#define REPORT_INTERVAL_MS 4
#define RATE_WINDOW_MS 2000
#define SCREEN_OFF_DELAY_MS 700
#define HAPTIC_PULSE_MS 55
#define ENUMERATION_STUCK_MS 4000
#define MAX_USB_RECOVERY_ATTEMPTS 3

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(value) (((value) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, array) (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1ul)

// Generic Desktop / Game Pad: four signed 16-bit stick axes and seventeen
// buttons.  Z/Rx are intentionally used for right X/Y: Chromium on Windows
// assigns raw axes by HID usage number, making this pair contiguous as browser
// axes 2/3.  The last four directional buttons precede the Home button.
static const uint8_t BROWSER_REPORT_DESCRIPTOR[] = {
	0x05, 0x01,       // Usage Page (Generic Desktop)
	0x09, 0x05,       // Usage (Game Pad)
	0xa1, 0x01,       // Collection (Application)
	0x16, 0x00, 0x80, //   Logical Minimum (-32768)
	0x26, 0xff, 0x7f, //   Logical Maximum (32767)
	0x75, 0x10,       //   Report Size (16)
	0x95, 0x04,       //   Report Count (4)
	0x09, 0x30,       //   Usage (X)
	0x09, 0x31,       //   Usage (Y)
	0x09, 0x32,       //   Usage (Z; right stick X / browser axis 2)
	0x09, 0x33,       //   Usage (Rx; right stick Y / browser axis 3)
	0x81, 0x02,       //   Input (Data, Variable, Absolute)
	0x05, 0x09,       //   Usage Page (Button)
	0x19, 0x01,       //   Usage Minimum (Button 1)
	0x29, 0x11,       //   Usage Maximum (Button 17)
	0x15, 0x00,       //   Logical Minimum (0)
	0x25, 0x01,       //   Logical Maximum (1)
	0x75, 0x01,       //   Report Size (1)
	0x95, 0x11,       //   Report Count (17)
	0x81, 0x02,       //   Input (Data, Variable, Absolute)
	0x75, 0x01,       //   Report Size (1)
	0x95, 0x07,       //   Report Count (7)
	0x81, 0x03,       //   Input (Constant, Variable, Absolute)
	0xc0,             // End Collection
};

_Static_assert(sizeof(BROWSER_REPORT_DESCRIPTOR) == 49,
	"USB gamepad HID descriptor length changed; update report_length deliberately");

// Wired HORIPAD-compatible input-only layout (0f0d:00c1). This is a known
// SDL/Steam controller and Chromium's Windows mapping promotes raw button 12
// to standard HOME/B16 after placing the POV directions at B12-B15.
static const uint8_t PC_REPORT_DESCRIPTOR[] = {
	0x05, 0x01,       // Usage Page (Generic Desktop)
	0x09, 0x05,       // Usage (Game Pad)
	0xa1, 0x01,       // Collection (Application)
	0x15, 0x00,       //   Logical Minimum (0)
	0x25, 0x01,       //   Logical Maximum (1)
	0x35, 0x00,       //   Physical Minimum (0)
	0x45, 0x01,       //   Physical Maximum (1)
	0x75, 0x01,       //   Report Size (1)
	0x95, 0x0e,       //   Report Count (14)
	0x05, 0x09,       //   Usage Page (Button)
	0x19, 0x01,       //   Usage Minimum (Button 1)
	0x29, 0x0e,       //   Usage Maximum (Button 14)
	0x81, 0x02,       //   Input (Data, Variable, Absolute)
	0x95, 0x02,       //   Report Count (2 padding bits)
	0x81, 0x01,       //   Input (Constant, Array, Absolute)
	0x05, 0x01,       //   Usage Page (Generic Desktop)
	0x25, 0x07,       //   Logical Maximum (7)
	0x46, 0x3b, 0x01, //   Physical Maximum (315)
	0x75, 0x04,       //   Report Size (4)
	0x95, 0x01,       //   Report Count (1)
	0x65, 0x14,       //   Unit (degrees)
	0x09, 0x39,       //   Usage (Hat Switch)
	0x81, 0x42,       //   Input (Data, Variable, Absolute, Null State)
	0x65, 0x00,       //   Unit (none)
	0x95, 0x01,       //   Report Count (1 padding nibble)
	0x81, 0x01,       //   Input (Constant, Array, Absolute)
	0x26, 0xff, 0x00, //   Logical Maximum (255)
	0x46, 0xff, 0x00, //   Physical Maximum (255)
	0x09, 0x30,       //   Usage (X)
	0x09, 0x31,       //   Usage (Y)
	0x09, 0x32,       //   Usage (Z)
	0x09, 0x35,       //   Usage (Rz)
	0x75, 0x08,       //   Report Size (8)
	0x95, 0x04,       //   Report Count (4)
	0x81, 0x02,       //   Input (Data, Variable, Absolute)
	0x75, 0x08,       //   Report Size (8)
	0x95, 0x01,       //   Report Count (1 trailing pad byte)
	0x81, 0x01,       //   Input (Constant, Array, Absolute)
	0xc0,             // End Collection
};

_Static_assert(sizeof(PC_REPORT_DESCRIPTOR) == 80,
	"PC compatibility HID descriptor length changed; update report_length deliberately");

// Lab-only descriptor used by --probe-functionfs-out. It exists solely to
// prove that this kernel accepts one interrupt-IN plus one interrupt-OUT
// endpoint through FunctionFS. It is not a release controller protocol.
static const uint8_t OUTPUT_PROBE_REPORT_DESCRIPTOR[] = {
	0x05, 0x01,       // Usage Page (Generic Desktop)
	0x09, 0x05,       // Usage (Game Pad)
	0xa1, 0x01,       // Collection (Application)
	0x15, 0x00,       //   Logical Minimum (0)
	0x26, 0xff, 0x00, //   Logical Maximum (255)
	0x75, 0x08,       //   Report Size (8)
	0x95, 0x08,       //   Report Count (8 input bytes)
	0x06, 0x00, 0xff, //   Usage Page (Vendor Defined)
	0x09, 0x01,       //   Usage (1)
	0x81, 0x02,       //   Input (Data, Variable, Absolute)
	0x95, 0x20,       //   Report Count (32 output bytes)
	0x09, 0x02,       //   Usage (2)
	0x91, 0x02,       //   Output (Data, Variable, Absolute)
	0xc0,             // End Collection
};

// Brick Pro's stock 4.9.191 kernel has ConfigFS FunctionFS support but was
// compiled without CONFIG_USB_CONFIGFS_F_HID.  FunctionFS still accepts HID
// interface descriptors, so the small userspace driver below supplies the HID
// control requests and interrupt-IN reports itself.
typedef struct __attribute__((packed)) {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t bcdHID;
	uint8_t bCountryCode;
	uint8_t bNumDescriptors;
	uint8_t bReportDescriptorType;
	uint16_t wDescriptorLength;
} NxHidDescriptor;

_Static_assert(sizeof(NxHidDescriptor) == 9, "USB HID descriptor must be nine bytes");

#define NX_INTERFACE_DESCRIPTOR_INIT(endpointCount)                                                         \
	{                                                                                                     \
		.bLength = USB_DT_INTERFACE_SIZE, .bDescriptorType = USB_DT_INTERFACE, .bInterfaceNumber = 0,   \
		.bAlternateSetting = 0, .bNumEndpoints = endpointCount,                                         \
		.bInterfaceClass = USB_INTERFACE_CLASS_HID,                                                      \
		.bInterfaceSubClass = 0, .bInterfaceProtocol = 0, .iInterface = 1                                 \
	}

#define NX_HID_DESCRIPTOR_INIT(reportLength)                                                                \
	{                                                                                                     \
		.bLength = sizeof(NxHidDescriptor), .bDescriptorType = HID_DT_HID, .bcdHID = 0x0111,             \
		.bCountryCode = 0, .bNumDescriptors = 1, .bReportDescriptorType = HID_DT_REPORT,                  \
		.wDescriptorLength = reportLength                                                                 \
	}

#define NX_ENDPOINT_DESCRIPTOR_INIT(address, packetSize, interval)                                          \
	{                                                                                                     \
		.bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,                              \
		.bEndpointAddress = address, .bmAttributes = USB_ENDPOINT_XFER_INT,                               \
		.wMaxPacketSize = packetSize,                                                                      \
		.bInterval = interval                                                                             \
	}

typedef struct __attribute__((packed)) {
	struct usb_functionfs_descs_head_v2 header;
	uint32_t fsCount;
	uint32_t hsCount;
	struct usb_interface_descriptor fsInterface;
	NxHidDescriptor fsHid;
	struct usb_endpoint_descriptor_no_audio fsEndpoint;
	struct usb_interface_descriptor hsInterface;
	NxHidDescriptor hsHid;
	struct usb_endpoint_descriptor_no_audio hsEndpoint;
} NxFunctionFsDescriptors;

static const NxFunctionFsDescriptors BROWSER_FUNCTIONFS_DESCRIPTORS = {
	.header =
		{
			.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
			.length = sizeof(NxFunctionFsDescriptors),
			.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC,
		},
	.fsCount = 3,
	.hsCount = 3,
	.fsInterface = NX_INTERFACE_DESCRIPTOR_INIT(1),
	.fsHid = NX_HID_DESCRIPTOR_INIT(sizeof(BROWSER_REPORT_DESCRIPTOR)),
	.fsEndpoint = NX_ENDPOINT_DESCRIPTOR_INIT(0x81, NXGP_MAX_REPORT_SIZE, 4),
	.hsInterface = NX_INTERFACE_DESCRIPTOR_INIT(1),
	.hsHid = NX_HID_DESCRIPTOR_INIT(sizeof(BROWSER_REPORT_DESCRIPTOR)),
	.hsEndpoint = NX_ENDPOINT_DESCRIPTOR_INIT(0x81, NXGP_MAX_REPORT_SIZE, 4),
};

static const NxFunctionFsDescriptors PC_FUNCTIONFS_DESCRIPTORS = {
	.header =
		{
			.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
			.length = sizeof(NxFunctionFsDescriptors),
			.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC,
		},
	.fsCount = 3,
	.hsCount = 3,
	.fsInterface = NX_INTERFACE_DESCRIPTOR_INIT(1),
	.fsHid = NX_HID_DESCRIPTOR_INIT(sizeof(PC_REPORT_DESCRIPTOR)),
	.fsEndpoint = NX_ENDPOINT_DESCRIPTOR_INIT(0x81, NXGP_MAX_REPORT_SIZE, 4),
	.hsInterface = NX_INTERFACE_DESCRIPTOR_INIT(1),
	.hsHid = NX_HID_DESCRIPTOR_INIT(sizeof(PC_REPORT_DESCRIPTOR)),
	.hsEndpoint = NX_ENDPOINT_DESCRIPTOR_INIT(0x81, NXGP_MAX_REPORT_SIZE, 4),
};

typedef struct __attribute__((packed)) {
	struct usb_functionfs_descs_head_v2 header;
	uint32_t fsCount;
	uint32_t hsCount;
	struct usb_interface_descriptor fsInterface;
	NxHidDescriptor fsHid;
	struct usb_endpoint_descriptor_no_audio fsEndpointIn;
	struct usb_endpoint_descriptor_no_audio fsEndpointOut;
	struct usb_interface_descriptor hsInterface;
	NxHidDescriptor hsHid;
	struct usb_endpoint_descriptor_no_audio hsEndpointIn;
	struct usb_endpoint_descriptor_no_audio hsEndpointOut;
} NxFunctionFsOutProbeDescriptors;

static const NxFunctionFsOutProbeDescriptors OUTPUT_PROBE_FUNCTIONFS_DESCRIPTORS = {
	.header = {
		.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
		.length = sizeof(NxFunctionFsOutProbeDescriptors),
		.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC,
	},
	.fsCount = 4,
	.hsCount = 4,
	.fsInterface = NX_INTERFACE_DESCRIPTOR_INIT(2),
	.fsHid = NX_HID_DESCRIPTOR_INIT(sizeof(OUTPUT_PROBE_REPORT_DESCRIPTOR)),
	.fsEndpointIn = NX_ENDPOINT_DESCRIPTOR_INIT(0x81, 8, 4),
	.fsEndpointOut = NX_ENDPOINT_DESCRIPTOR_INIT(0x01, 32, 4),
	.hsInterface = NX_INTERFACE_DESCRIPTOR_INIT(2),
	.hsHid = NX_HID_DESCRIPTOR_INIT(sizeof(OUTPUT_PROBE_REPORT_DESCRIPTOR)),
	// High-speed intervals are powers of two in 125 us microframes: 6 = 4 ms.
	.hsEndpointIn = NX_ENDPOINT_DESCRIPTOR_INIT(0x81, 8, 6),
	.hsEndpointOut = NX_ENDPOINT_DESCRIPTOR_INIT(0x01, 32, 6),
};

typedef struct __attribute__((packed)) {
	struct usb_functionfs_strings_head header;
	uint16_t language;
	char interfaceName[sizeof("NX Redux USB Gamepad")];
} NxFunctionFsStrings;

static const NxFunctionFsStrings FUNCTIONFS_STRINGS = {
	.header =
		{
			.magic = FUNCTIONFS_STRINGS_MAGIC,
			.length = sizeof(NxFunctionFsStrings),
			.str_count = 1,
			.lang_count = 1,
		},
	.language = 0x0409,
	.interfaceName = "NX Redux USB Gamepad",
};

typedef enum {
	GAMEPAD_PROFILE_BROWSER,
	GAMEPAD_PROFILE_PC,
	GAMEPAD_PROFILE_OUTPUT_PROBE,
} GamepadProfile;

typedef struct {
	GamepadProfile profile;
	int ep0;
	int epIn;
	int epOut;
	bool mounted;
	bool linked;
	bool bound;
	bool enabled;
	bool sawSetup;
	uint8_t idleRate;
	uint8_t protocol;
	size_t reportSize;
	uint8_t currentReport[NXGP_MAX_REPORT_SIZE];
	char udc[128];
} FunctionFsGamepad;

typedef struct {
	bool valid;
	struct input_absinfo info;
} AxisInfo;

typedef struct {
	int restoreBrightnessRaw;
	bool screenOff;
	uint64_t screenOffAt;
	bool hapticEnabled;
	int hapticVoltage;
	uint64_t hapticOffAt;
} LocalControls;

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t controlTimedOut = 0;

static void handleSignal(int signalNumber) {
	(void)signalNumber;
	running = 0;
}

static void handleControlTimeout(int signalNumber) {
	(void)signalNumber;
	controlTimedOut = 1;
}

static void setControlTimeout(bool armed) {
	struct itimerval timer;
	memset(&timer, 0, sizeof(timer));
	if (armed) {
		timer.it_value.tv_sec = 2;
		controlTimedOut = 0;
	}
	(void)setitimer(ITIMER_REAL, &timer, NULL);
}

static uint64_t monotonicMs(void) {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static bool writeAll(int fd, const void* data, size_t size) {
	const uint8_t* cursor = data;
	while (size > 0) {
		ssize_t written = write(fd, cursor, size);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (written == 0) {
			errno = EIO;
			return false;
		}
		cursor += written;
		size -= (size_t)written;
	}
	return true;
}

static bool writeFile(const char* path, const void* data, size_t size) {
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "usbgamepad: open %s: %s\n", path, strerror(errno));
		return false;
	}
	bool ok = writeAll(fd, data, size);
	if (!ok)
		fprintf(stderr, "usbgamepad: write %s: %s\n", path, strerror(errno));
	close(fd);
	return ok;
}

static bool writeText(const char* path, const char* value) {
	return writeFile(path, value, strlen(value));
}

static bool textFileEquals(const char* path, const char* expected) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	char value[64];
	ssize_t length = read(fd, value, sizeof(value) - 1);
	close(fd);
	if (length <= 0)
		return false;
	value[length] = '\0';
	while (length > 0 &&
		(value[length - 1] == '\n' || value[length - 1] == '\r' || value[length - 1] == ' ' ||
			value[length - 1] == '\t'))
		value[--length] = '\0';
	return strcmp(value, expected) == 0;
}

static int environmentInteger(const char* name, int fallback, int minimum, int maximum) {
	const char* value = getenv(name);
	if (!value || !*value)
		return fallback;
	char* end = NULL;
	long parsed = strtol(value, &end, 10);
	if (!end || *end != '\0' || parsed < minimum || parsed > maximum)
		return fallback;
	return (int)parsed;
}

static bool setRawBrightness(int value) {
	int fd = open(DISPLAY_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "usbgamepad: open %s: %s\n", DISPLAY_DEVICE, strerror(errno));
		return false;
	}
	unsigned long parameters[4] = {0, (unsigned long)value, 0, 0};
	bool ok = ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &parameters) == 0;
	if (!ok)
		fprintf(stderr, "usbgamepad: set raw brightness %d: %s\n", value, strerror(errno));
	close(fd);
	return ok;
}

static bool writeScreenMarker(int restoreBrightnessRaw) {
	int fd = open(SCREEN_MARKER, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		fprintf(stderr, "usbgamepad: create %s: %s\n", SCREEN_MARKER, strerror(errno));
		return false;
	}
	char value[16];
	int length = snprintf(value, sizeof(value), "%d\n", restoreBrightnessRaw);
	bool ok = length > 0 && (size_t)length < sizeof(value) && writeAll(fd, value, (size_t)length);
	if (!ok)
		fprintf(stderr, "usbgamepad: write %s: %s\n", SCREEN_MARKER, strerror(errno));
	close(fd);
	return ok;
}

static bool readScreenMarker(int* restoreBrightnessRaw) {
	int fd = open(SCREEN_MARKER, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	char value[16];
	ssize_t length = read(fd, value, sizeof(value) - 1);
	close(fd);
	if (length <= 0)
		return false;
	value[length] = '\0';
	char* end = NULL;
	long parsed = strtol(value, &end, 10);
	if (end == value || parsed < 1 || parsed > 255)
		return false;
	*restoreBrightnessRaw = (int)parsed;
	return true;
}

static void restoreScreenFromMarker(void) {
	int restoreBrightnessRaw;
	if (readScreenMarker(&restoreBrightnessRaw)) {
		fprintf(stderr, "usbgamepad: restoring screen brightness to raw %d\n", restoreBrightnessRaw);
		if (setRawBrightness(restoreBrightnessRaw))
			(void)unlink(SCREEN_MARKER);
		return;
	}
	// A malformed marker cannot be used for recovery and should not trap
	// every later launch in a retry loop.
	(void)unlink(SCREEN_MARKER);
}

static void initLocalControls(LocalControls* controls) {
	memset(controls, 0, sizeof(*controls));
	controls->restoreBrightnessRaw =
		environmentInteger("NXGP_SCREEN_BRIGHTNESS_RAW", 16, 1, 255);
	controls->hapticEnabled = environmentInteger("NXGP_RUMBLE_ENABLED", 0, 0, 1) != 0;
	int strength = environmentInteger("NXGP_RUMBLE_STRENGTH", 0, 0, 2);
	controls->hapticVoltage = strength == 1 ? 900000 : strength == 2 ? 1700000 : 1200000;
}

static void stopHaptic(LocalControls* controls) {
	if (controls->hapticOffAt != 0)
		(void)writeText(RUMBLE_GPIO, "0\n");
	controls->hapticOffAt = 0;
}

static void pulseHaptic(LocalControls* controls, uint64_t now) {
	if (!controls->hapticEnabled)
		return;
	char voltage[24];
	int length = snprintf(voltage, sizeof(voltage), "%d\n", controls->hapticVoltage);
	if (length <= 0 || (size_t)length >= sizeof(voltage) || !writeText(RUMBLE_VOLTAGE, voltage) ||
		!writeText(RUMBLE_GPIO, "1\n"))
		return;
	controls->hapticOffAt = now + HAPTIC_PULSE_MS;
}

static void wakeScreen(LocalControls* controls) {
	controls->screenOffAt = 0;
	if (controls->screenOff && !setRawBrightness(controls->restoreBrightnessRaw))
		return;
	controls->screenOff = false;
	(void)unlink(SCREEN_MARKER);
}

static void finishScreenOff(LocalControls* controls) {
	controls->screenOffAt = 0;
	if (!writeScreenMarker(controls->restoreBrightnessRaw))
		return;
	if (!setRawBrightness(0)) {
		(void)unlink(SCREEN_MARKER);
		return;
	}
	controls->screenOff = true;
}

static const char* layoutName(NxGamepadLayout layout) {
	return layout == NXGP_LAYOUT_NINTENDO ? "Nintendo" : "Xbox";
}

static bool persistLayout(NxGamepadLayout layout) {
	const char* path = getenv(LAYOUT_STATE_ENV);
	if (!path || !*path)
		return true;
	char temporary[512];
	int temporaryLength = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (temporaryLength <= 0 || (size_t)temporaryLength >= sizeof(temporary)) {
		fprintf(stderr, "usbgamepad: layout state path is too long\n");
		return false;
	}
	int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		fprintf(stderr, "usbgamepad: create %s: %s\n", temporary, strerror(errno));
		return false;
	}
	const char* value = layout == NXGP_LAYOUT_NINTENDO ? "nintendo\n" : "xbox\n";
	bool ok = writeAll(fd, value, strlen(value));
	if (ok)
		ok = fsync(fd) == 0;
	int writeError = errno;
	close(fd);
	if (ok && rename(temporary, path) != 0) {
		writeError = errno;
		ok = false;
	}
	if (!ok) {
		fprintf(stderr, "usbgamepad: persist layout %s: %s\n", path, strerror(writeError));
		(void)unlink(temporary);
	}
	return ok;
}

static bool makeDirectory(const char* path) {
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return true;
	fprintf(stderr, "usbgamepad: mkdir %s: %s\n", path, strerror(errno));
	return false;
}

static void notifyStatus(const char* message) {
	int fd = open(STATUS_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	char line[256];
	int length = snprintf(line, sizeof(line), "TEXT:%s\n", message);
	if (length > 0) {
		if ((size_t)length >= sizeof(line))
			length = (int)sizeof(line) - 1;
		(void)write(fd, line, (size_t)length);
	}
	close(fd);
}

static bool findUdc(char* output, size_t outputSize) {
	DIR* directory = opendir(UDC_CLASS);
	if (!directory) {
		fprintf(stderr, "usbgamepad: opendir %s: %s\n", UDC_CLASS, strerror(errno));
		return false;
	}
	bool found = false;
	struct dirent* entry;
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		snprintf(output, outputSize, "%s", entry->d_name);
		found = true;
		break;
	}
	closedir(directory);
	if (!found)
		fprintf(stderr, "usbgamepad: no USB device controller found\n");
	return found;
}

static bool readBoundUdc(char* output, size_t outputSize) {
	char path[256];
	snprintf(path, sizeof(path), "%s/UDC", GADGET_ROOT);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	char value[128];
	ssize_t length = read(fd, value, sizeof(value) - 1);
	int readError = errno;
	close(fd);
	if (length < 0) {
		if (readError != ENOENT && readError != ENODEV)
			fprintf(stderr, "usbgamepad: read %s: %s\n", path, strerror(readError));
		return false;
	}
	value[length] = '\0';
	while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r' || value[length - 1] == '\t' ||
		value[length - 1] == ' '))
		value[--length] = '\0';
	if (length == 0 || strcmp(value, "none") == 0)
		return false;
	snprintf(output, outputSize, "%s", value);
	return true;
}

static bool unbindGadget(void) {
	char path[256];
	snprintf(path, sizeof(path), "%s/UDC", GADGET_ROOT);
	if (access(path, F_OK) != 0)
		return true;
	// ConfigFS expects the equivalent of `echo "" > UDC`, i.e. one newline.
	// The Brick Pro's 4.9 UDC can report ENODEV after it has already completed
	// the unbind.  The write status is therefore advisory; the UDC file is the
	// postcondition and must be empty (or "none") before links are changed.
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "usbgamepad: open %s: %s\n", path, strerror(errno));
		return false;
	}
	bool ok = writeAll(fd, "\n", 1);
	int writeError = errno;
	close(fd);
	if (!ok && writeError != ENODEV)
		fprintf(stderr, "usbgamepad: write %s: %s\n", path, strerror(writeError));
	for (int attempt = 0; attempt < 100; attempt++) {
		int stateFd = open(path, O_RDONLY | O_CLOEXEC);
		if (stateFd < 0) {
			usleep(20000);
			continue;
		}
		char value[128];
		ssize_t length = read(stateFd, value, sizeof(value) - 1);
		close(stateFd);
		if (length >= 0) {
			value[length] = '\0';
			while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r' ||
				value[length - 1] == '\t' || value[length - 1] == ' '))
				value[--length] = '\0';
			if (length == 0 || strcmp(value, "none") == 0)
				return true;
		}
		usleep(20000);
	}
	fprintf(stderr, "usbgamepad: gadget remained bound after UDC unbind request\n");
	return false;
}

static bool triggerUsbRole(const char* path) {
	if (access(path, F_OK) != 0) {
		fprintf(stderr, "usbgamepad: vendor USB role trigger is missing: %s\n", path);
		return false;
	}
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "usbgamepad: open %s: %s\n", path, strerror(errno));
		return false;
	}
	char ignored;
	ssize_t result;
	do {
		result = read(fd, &ignored, 1);
	} while (result < 0 && errno == EINTR);
	int error = errno;
	close(fd);
	if (result < 0) {
		fprintf(stderr, "usbgamepad: trigger %s: %s\n", path, strerror(error));
		return false;
	}
	return true;
}

static bool udcIsPresent(void) {
	DIR* directory = opendir(UDC_CLASS);
	if (!directory)
		return false;
	bool found = false;
	struct dirent* entry;
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] != '.') {
			found = true;
			break;
		}
	}
	closedir(directory);
	return found;
}

static bool ensureUsbDeviceRole(void) {
	// usb_device is an action-on-read vendor node.  Avoid touching it while the
	// OTG manager already reports device mode: doing so unregisters the UDC and
	// can interrupt the ConfigFS transaction or its recovery path.
	if (textFileEquals(USB_ROLE_STATE, "usb_device") && udcIsPresent())
		return true;
	if (!triggerUsbRole(USB_ROLE_DEVICE))
		return false;
	for (int attempt = 0; attempt < 100; attempt++) {
		if (textFileEquals(USB_ROLE_STATE, "usb_device") && udcIsPresent()) {
			usleep(250000);
			return true;
		}
		usleep(20000);
	}
	fprintf(stderr, "usbgamepad: USB device controller did not return after PHY reset\n");
	return false;
}

static bool readUdcState(const FunctionFsGamepad* gamepad, char* output, size_t outputSize) {
	if (!gamepad->udc[0] || outputSize == 0)
		return false;
	char path[256];
	snprintf(path, sizeof(path), "%s/%s/state", UDC_CLASS, gamepad->udc);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	ssize_t length = read(fd, output, outputSize - 1);
	close(fd);
	if (length <= 0)
		return false;
	output[length] = '\0';
	while (length > 0 && (output[length - 1] == '\n' || output[length - 1] == '\r' ||
		output[length - 1] == '\t' || output[length - 1] == ' '))
		output[--length] = '\0';
	return length > 0;
}

static bool udcStateCanBeStuck(const char* state) {
	return strcmp(state, "default") == 0 || strcmp(state, "addressed") == 0 ||
		strcmp(state, "configured") == 0;
}

static bool ensureStockGadget(void) {
	if (access(GADGET_CONFIG, F_OK) == 0 && access(GADGET_FUNCTIONS, F_OK) == 0)
		return true;

	// A normal boot has already created g1.  This fallback only repairs a
	// partial gadget left by an interrupted earlier run.
	if (system("/bin/setusbconfig adb") != 0 || access(GADGET_CONFIG, F_OK) != 0 ||
		access(GADGET_FUNCTIONS, F_OK) != 0) {
		fprintf(stderr, "usbgamepad: stock USB gadget is incomplete (need %s and %s)\n", GADGET_CONFIG,
			GADGET_FUNCTIONS);
		return false;
	}
	return true;
}

static bool removeLink(const char* path) {
	if (unlink(path) == 0 || errno == ENOENT)
		return true;
	fprintf(stderr, "usbgamepad: unlink %s: %s\n", path, strerror(errno));
	return false;
}

static bool removeAllFunctionLinks(void) {
	DIR* directory = opendir(GADGET_CONFIG);
	if (!directory) {
		fprintf(stderr, "usbgamepad: opendir %s: %s\n", GADGET_CONFIG, strerror(errno));
		return false;
	}
	bool ok = true;
	struct dirent* entry;
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", GADGET_CONFIG, entry->d_name);
		struct stat info;
		if (lstat(path, &info) == 0 && S_ISLNK(info.st_mode) && unlink(path) != 0) {
			fprintf(stderr, "usbgamepad: unlink %s: %s\n", path, strerror(errno));
			ok = false;
		}
	}
	closedir(directory);
	return ok;
}

static bool removeDirectory(const char* path) {
	if (rmdir(path) == 0 || errno == ENOENT)
		return true;
	fprintf(stderr, "usbgamepad: rmdir %s: %s\n", path, strerror(errno));
	return false;
}

static bool unmountFunctionFs(void) {
	if (umount2(FFS_MOUNT, MNT_DETACH) == 0 || errno == EINVAL || errno == ENOENT)
		return true;
	fprintf(stderr, "usbgamepad: unmount %s: %s\n", FFS_MOUNT, strerror(errno));
	return false;
}

static const char* profileName(GamepadProfile profile) {
	switch (profile) {
	case GAMEPAD_PROFILE_PC: return "pc";
	case GAMEPAD_PROFILE_OUTPUT_PROBE: return "out-probe";
	case GAMEPAD_PROFILE_BROWSER: return "browser";
	}
	return "unknown";
}

static void initFunctionFsGamepad(FunctionFsGamepad* gamepad, GamepadProfile profile) {
	memset(gamepad, 0, sizeof(*gamepad));
	gamepad->profile = profile;
	gamepad->ep0 = -1;
	gamepad->epIn = -1;
	gamepad->epOut = -1;
	gamepad->protocol = 1;
	gamepad->reportSize = profile == GAMEPAD_PROFILE_BROWSER ?
		NXGP_BROWSER_REPORT_SIZE :
		NXGP_PC_REPORT_SIZE;
	// ep0 can receive GET_REPORT immediately after the UDC is bound, before
	// bridgeInput() has opened the physical controls.  Give the host a valid
	// neutral HORIPAD packet from the first control request instead of the
	// all-zero struct image (which means POV-up and four fully-left axes).
	if (profile != GAMEPAD_PROFILE_BROWSER) {
		static const uint8_t neutral[NXGP_PC_REPORT_SIZE] = {0, 0, 8, 128, 128, 128, 128, 0};
		memcpy(gamepad->currentReport, neutral, sizeof(neutral));
	}
}

static bool removeStaleGamepadFunction(void) {
	bool ok = removeLink(FFS_LINK);
	ok = unmountFunctionFs() && ok;
	ok = removeDirectory(FFS_MOUNT) && ok;
	ok = removeDirectory(FFS_FUNCTION) && ok;
	return ok;
}

static bool configureFunctionFs(FunctionFsGamepad* gamepad, bool outputProbe) {
	if (!makeDirectory(FFS_FUNCTION) || !makeDirectory(FFS_PARENT) || !makeDirectory(FFS_MOUNT))
		return false;

	if (mount(FFS_NAME, FFS_MOUNT, "functionfs", MS_NOSUID | MS_NODEV, NULL) != 0) {
		fprintf(stderr, "usbgamepad: mount FunctionFS at %s: %s\n", FFS_MOUNT, strerror(errno));
		return false;
	}
	gamepad->mounted = true;

	gamepad->ep0 = open(FFS_EP0, O_RDWR | O_CLOEXEC);
	if (gamepad->ep0 < 0) {
		fprintf(stderr, "usbgamepad: open %s: %s\n", FFS_EP0, strerror(errno));
		return false;
	}
	const void* descriptors;
	size_t descriptorSize;
	if (outputProbe) {
		descriptors = &OUTPUT_PROBE_FUNCTIONFS_DESCRIPTORS;
		descriptorSize = sizeof(OUTPUT_PROBE_FUNCTIONFS_DESCRIPTORS);
	} else if (gamepad->profile == GAMEPAD_PROFILE_PC) {
		descriptors = &PC_FUNCTIONFS_DESCRIPTORS;
		descriptorSize = sizeof(PC_FUNCTIONFS_DESCRIPTORS);
	} else {
		descriptors = &BROWSER_FUNCTIONFS_DESCRIPTORS;
		descriptorSize = sizeof(BROWSER_FUNCTIONFS_DESCRIPTORS);
	}
	if (!writeAll(gamepad->ep0, descriptors, descriptorSize)) {
		fprintf(stderr, "usbgamepad: write FunctionFS descriptors: %s\n", strerror(errno));
		return false;
	}
	if (!writeAll(gamepad->ep0, &FUNCTIONFS_STRINGS, sizeof(FUNCTIONFS_STRINGS))) {
		fprintf(stderr, "usbgamepad: write FunctionFS strings: %s\n", strerror(errno));
		return false;
	}

	gamepad->epIn = open(FFS_EP_IN, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (gamepad->epIn < 0) {
		fprintf(stderr, "usbgamepad: open %s: %s\n", FFS_EP_IN, strerror(errno));
		return false;
	}
	if (outputProbe) {
		gamepad->epOut = open(FFS_EP_OUT, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (gamepad->epOut < 0) {
			fprintf(stderr, "usbgamepad: open %s: %s\n", FFS_EP_OUT, strerror(errno));
			return false;
		}
	}
	return true;
}

static bool configureGamepad(FunctionFsGamepad* gamepad) {
	char udc[128];
	if (!ensureStockGadget())
		return false;
	if (!readBoundUdc(udc, sizeof(udc)) && !findUdc(udc, sizeof(udc)))
		return false;
	if (!unbindGadget())
		return false;
	// A real ConfigFS unbind is sufficient to make the host drop the old
	// device.  Never switch this VBUS-detected OTG port to usb_null here.
	usleep(250000);

	// Controller mode deliberately exposes only the gamepad function.  In
	// particular, do not leave ADB attached to an arbitrary gaming host.
	if (!removeAllFunctionLinks() || !removeStaleGamepadFunction())
		return false;
	bool outputProbe = gamepad->profile == GAMEPAD_PROFILE_OUTPUT_PROBE;
	if (!configureFunctionFs(gamepad, outputProbe))
		return false;

	const char* vendorId = outputProbe ?
		"0x18d1\n" : gamepad->profile == GAMEPAD_PROFILE_PC ? "0x0f0d\n" : "0x0525\n";
	const char* productId = outputProbe ?
		"0xd002\n" : gamepad->profile == GAMEPAD_PROFILE_PC ? "0x00c1\n" : "0xa4ad\n";
	const char* deviceVersion = outputProbe ?
		"0x9901\n" : gamepad->profile == GAMEPAD_PROFILE_PC ? "0x0101\n" : "0x0100\n";
	const char* manufacturer = outputProbe ?
		"TRIMUI (NX lab)\n" : gamepad->profile == GAMEPAD_PROFILE_PC ? "NX Redux\n" : "TRIMUI\n";
	// A release-specific serial and device revision prevent Windows from reusing
	// a failed cached device instance after an interrupted FunctionFS/PHY
	// transition. VID/PID and the eight-byte report stay identical to the
	// verified v8 HORIPAD path.
	const char* serial = outputProbe ?
		"NX-FFS-OUT-LAB\n" : gamepad->profile == GAMEPAD_PROFILE_PC ?
		"NXREDUX-GAMEPAD-V11\n" :
		"NXREDUX-GAMEPAD-RAW\n";
	const char* product = outputProbe ?
		"NX FunctionFS OUT Lab\n" : gamepad->profile == GAMEPAD_PROFILE_PC ?
		"NX Redux USB Gamepad\n" :
		"NX Redux USB Gamepad\n";
	if (!writeText(GADGET_ROOT "/idVendor", vendorId) ||
		!writeText(GADGET_ROOT "/idProduct", productId) ||
		!writeText(GADGET_ROOT "/bcdDevice", deviceVersion) ||
		!writeText(GADGET_ROOT "/bDeviceClass", "0\n") ||
		!writeText(GADGET_ROOT "/bDeviceSubClass", "0\n") ||
		!writeText(GADGET_ROOT "/bDeviceProtocol", "0\n") ||
		!writeText(GADGET_ROOT "/strings/0x409/manufacturer", manufacturer) ||
		!writeText(GADGET_ROOT "/strings/0x409/serialnumber", serial) ||
		!writeText(GADGET_ROOT "/strings/0x409/product", product))
		return false;

	if (symlink(FFS_FUNCTION, FFS_LINK) != 0) {
		fprintf(stderr, "usbgamepad: symlink FunctionFS gamepad: %s\n", strerror(errno));
		return false;
	}
	gamepad->linked = true;
	if (!ensureUsbDeviceRole())
		return false;
	char udcPath[256];
	snprintf(udcPath, sizeof(udcPath), "%s/UDC", GADGET_ROOT);
	if (!writeText(udcPath, udc))
		return false;
	snprintf(gamepad->udc, sizeof(gamepad->udc), "%s", udc);

	fprintf(stderr, "usbgamepad: configured FunctionFS HID gamepad (%s profile) on %s\n",
		profileName(gamepad->profile), udc);
	return true;
}

static void closeFunctionFs(FunctionFsGamepad* gamepad) {
	if (gamepad->epOut >= 0) {
		close(gamepad->epOut);
		gamepad->epOut = -1;
	}
	if (gamepad->epIn >= 0) {
		close(gamepad->epIn);
		gamepad->epIn = -1;
	}
	if (gamepad->ep0 >= 0) {
		close(gamepad->ep0);
		gamepad->ep0 = -1;
	}
	gamepad->enabled = false;
	(void)removeLink(FFS_LINK);
	gamepad->linked = false;
	(void)unmountFunctionFs();
	gamepad->mounted = false;
	(void)removeDirectory(FFS_MOUNT);
	(void)removeDirectory(FFS_FUNCTION);
}

static bool restoreAdb(FunctionFsGamepad* gamepad) {
	bool detached = unbindGadget();
	closeFunctionFs(gamepad);
	if (!detached || !ensureUsbDeviceRole()) {
		fprintf(stderr, "usbgamepad: failed to restore the USB device role\n");
		return false;
	}
	int result = system("/bin/setusbconfig adb");
	if (result != 0) {
		fprintf(stderr, "usbgamepad: failed to restore ADB USB mode\n");
		return false;
	}
	return true;
}

static int openTrimuiInput(char* selectedPath, size_t selectedPathSize) {
	for (int index = 0; index < 32; index++) {
		char path[64];
		snprintf(path, sizeof(path), INPUT_DIR "/event%d", index);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		char name[128] = {0};
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && strcmp(name, INPUT_NAME) == 0) {
			snprintf(selectedPath, selectedPathSize, "%s", path);
			return fd;
		}
		close(fd);
	}
	fprintf(stderr, "usbgamepad: could not find input device '%s'\n", INPUT_NAME);
	return -1;
}

static int32_t scaleSignedAxis(int32_t value, const AxisInfo* axis) {
	if (!axis->valid || axis->info.maximum <= axis->info.minimum) {
		if (value < -32767)
			return -32767;
		if (value > 32767)
			return 32767;
		return value;
	}
	if (value < axis->info.minimum)
		value = axis->info.minimum;
	if (value > axis->info.maximum)
		value = axis->info.maximum;
	int64_t span = (int64_t)axis->info.maximum - axis->info.minimum;
	return (int32_t)(((int64_t)(value - axis->info.minimum) * 65534) / span - 32767);
}

static int32_t scaleTrigger(int32_t value, const AxisInfo* axis) {
	if (!axis->valid || axis->info.maximum <= axis->info.minimum) {
		if (value < 0)
			return 0;
		if (value > 255)
			return 255;
		return value;
	}
	if (value < axis->info.minimum)
		value = axis->info.minimum;
	if (value > axis->info.maximum)
		value = axis->info.maximum;
	int64_t span = (int64_t)axis->info.maximum - axis->info.minimum;
	return (int32_t)(((int64_t)(value - axis->info.minimum) * 255) / span);
}

static int32_t normalizeAbs(uint16_t code, int32_t value, const AxisInfo axes[ABS_CNT]) {
	switch (code) {
	case ABS_X:
	case ABS_Y:
	case ABS_RX:
	case ABS_RY: return scaleSignedAxis(value, &axes[code]);
	case ABS_Z:
	case ABS_RZ: return scaleTrigger(value, &axes[code]);
	case ABS_HAT0X:
	case ABS_HAT0Y: return value < 0 ? -1 : value > 0 ? 1 : 0;
	default: return value;
	}
}

static void loadAxisInfo(int inputFd, AxisInfo axes[ABS_CNT]) {
	memset(axes, 0, sizeof(AxisInfo) * ABS_CNT);
	const uint16_t codes[] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		uint16_t code = codes[i];
		axes[code].valid = ioctl(inputFd, EVIOCGABS(code), &axes[code].info) == 0;
	}
}

static void resyncInput(int inputFd, const AxisInfo axes[ABS_CNT], NxGamepadState* state) {
	unsigned long keys[NBITS(KEY_MAX + 1)];
	memset(keys, 0, sizeof(keys));
	const uint16_t keyCodes[] = {
		NXGP_BTN_A,       NXGP_BTN_B,      NXGP_BTN_X,       NXGP_BTN_Y,     NXGP_BTN_TL,
		NXGP_BTN_TR,      NXGP_BTN_TL2,    NXGP_BTN_TR2,     NXGP_BTN_SELECT, NXGP_BTN_START,
		NXGP_BTN_MODE,    NXGP_BTN_THUMBL, NXGP_BTN_THUMBR,  NXGP_KEY_F1,     NXGP_KEY_F2,
		NXGP_KEY_HOMEPAGE,
	};
	if (ioctl(inputFd, EVIOCGKEY(sizeof(keys)), keys) == 0) {
		for (size_t i = 0; i < sizeof(keyCodes) / sizeof(keyCodes[0]); i++)
			NxGamepad_setKey(state, keyCodes[i], TEST_BIT(keyCodes[i], keys) != 0);
	}
	const uint16_t absCodes[] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
	for (size_t i = 0; i < sizeof(absCodes) / sizeof(absCodes[0]); i++) {
		uint16_t code = absCodes[i];
		struct input_absinfo current;
		if (ioctl(inputFd, EVIOCGABS(code), &current) == 0)
			NxGamepad_setAbs(state, code, normalizeAbs(code, current.value, axes));
	}
}

static bool ep0WriteResponse(FunctionFsGamepad* gamepad, const void* data, size_t available,
	uint16_t requested) {
	size_t size = available < requested ? available : requested;
	ssize_t result;
	int error;
	do {
		setControlTimeout(true);
		result = write(gamepad->ep0, data, size);
		error = result < 0 ? errno : 0;
		setControlTimeout(false);
	} while (result < 0 && error == EINTR && !controlTimedOut && running);
	if (result == (ssize_t)size)
		return true;
	if (result < 0 &&
		(error == EIDRM || error == EL2HLT || error == ECONNRESET || error == ESHUTDOWN)) {
		fprintf(stderr, "usbgamepad: host ended control response (errno=%d)\n", error);
		return true;
	}
	fprintf(stderr, "usbgamepad: FunctionFS control response: result=%zd errno=%d (%s)\n", result,
		error, result < 0 ? strerror(error) : "short write");
	return false;
}

static bool ep0ConsumeOut(FunctionFsGamepad* gamepad, uint16_t requested) {
	uint8_t buffer[256];
	if (requested > sizeof(buffer))
		return false;
	ssize_t result;
	int error;
	do {
		setControlTimeout(true);
		result = read(gamepad->ep0, buffer, requested);
		error = result < 0 ? errno : 0;
		setControlTimeout(false);
	} while (result < 0 && error == EINTR && !controlTimedOut && running);
	if (result >= 0)
		return true;
	if (error == EIDRM || error == EL2HLT || error == ECONNRESET || error == ESHUTDOWN) {
		fprintf(stderr, "usbgamepad: host ended control data stage (errno=%d)\n", error);
		return true;
	}
	fprintf(stderr, "usbgamepad: FunctionFS control data: result=%zd errno=%d (%s)\n", result, error,
		strerror(error));
	return false;
}

static void stallSetup(FunctionFsGamepad* gamepad, const struct usb_ctrlrequest* setup) {
	uint8_t unused = 0;
	ssize_t result;
	setControlTimeout(true);
	if (setup->bRequestType & USB_DIR_IN)
		result = read(gamepad->ep0, &unused, 0);
	else
		result = write(gamepad->ep0, &unused, 0);
	setControlTimeout(false);
	if (result < 0 && controlTimedOut)
		running = 0;
	(void)result;
}

static bool handleSetup(FunctionFsGamepad* gamepad, const struct usb_ctrlrequest* setup) {
	uint8_t requestType = setup->bRequestType;
	uint8_t type = requestType & USB_TYPE_MASK;
	uint8_t recipient = requestType & USB_RECIP_MASK;
	uint16_t value = le16toh(setup->wValue);
	uint16_t length = le16toh(setup->wLength);
	bool directionIn = (requestType & USB_DIR_IN) != 0;
	fprintf(stderr, "usbgamepad: setup type=%02x request=%02x value=%04x index=%04x length=%u\n",
		requestType, setup->bRequest, value, le16toh(setup->wIndex), length);

	if (recipient == USB_RECIP_INTERFACE && directionIn && type == USB_TYPE_STANDARD &&
		setup->bRequest == USB_REQ_GET_DESCRIPTOR) {
		switch ((uint8_t)(value >> 8)) {
		case HID_DT_HID: {
			const NxHidDescriptor* descriptor;
			if (gamepad->profile == GAMEPAD_PROFILE_OUTPUT_PROBE)
				descriptor = &OUTPUT_PROBE_FUNCTIONFS_DESCRIPTORS.fsHid;
			else if (gamepad->profile == GAMEPAD_PROFILE_PC)
				descriptor = &PC_FUNCTIONFS_DESCRIPTORS.fsHid;
			else
				descriptor = &BROWSER_FUNCTIONFS_DESCRIPTORS.fsHid;
			return ep0WriteResponse(gamepad, descriptor, sizeof(*descriptor), length);
		}
		case HID_DT_REPORT:
			if (gamepad->profile == GAMEPAD_PROFILE_OUTPUT_PROBE)
				return ep0WriteResponse(gamepad, OUTPUT_PROBE_REPORT_DESCRIPTOR,
					sizeof(OUTPUT_PROBE_REPORT_DESCRIPTOR), length);
			if (gamepad->profile == GAMEPAD_PROFILE_PC)
				return ep0WriteResponse(
					gamepad, PC_REPORT_DESCRIPTOR, sizeof(PC_REPORT_DESCRIPTOR), length);
			return ep0WriteResponse(
				gamepad, BROWSER_REPORT_DESCRIPTOR, sizeof(BROWSER_REPORT_DESCRIPTOR), length);
		default: break;
		}
	}

	if (recipient == USB_RECIP_INTERFACE && type == USB_TYPE_CLASS) {
		if (directionIn) {
			switch (setup->bRequest) {
			case HID_REQ_GET_REPORT:
				if ((value >> 8) == 1)
					return ep0WriteResponse(
						gamepad, gamepad->currentReport, gamepad->reportSize, length);
				break;
			case HID_REQ_GET_IDLE:
				return ep0WriteResponse(gamepad, &gamepad->idleRate, sizeof(gamepad->idleRate), length);
			case HID_REQ_GET_PROTOCOL:
				return ep0WriteResponse(gamepad, &gamepad->protocol, sizeof(gamepad->protocol), length);
			default: break;
			}
		} else {
			switch (setup->bRequest) {
			case HID_REQ_SET_REPORT:
				if (ep0ConsumeOut(gamepad, length))
					return true;
				break;
			case HID_REQ_SET_IDLE:
				gamepad->idleRate = (uint8_t)(value >> 8);
				return ep0ConsumeOut(gamepad, length);
			case HID_REQ_SET_PROTOCOL:
				gamepad->protocol = (uint8_t)value;
				return ep0ConsumeOut(gamepad, length);
			default: break;
			}
		}
	}

	fprintf(stderr, "usbgamepad: stall unsupported setup type=%02x request=%02x value=%04x length=%u\n",
		requestType, setup->bRequest, value, length);
	stallSetup(gamepad, setup);
	return true;
}

static bool handleFunctionFsEvents(FunctionFsGamepad* gamepad, bool* reportDirty) {
	struct usb_functionfs_event events[4];
	ssize_t bytes;
	do {
		bytes = read(gamepad->ep0, events, sizeof(events));
	} while (bytes < 0 && errno == EINTR && running);
	if (bytes < 0 && errno == EAGAIN)
		return true;
	if (bytes < 0) {
		fprintf(stderr, "usbgamepad: read FunctionFS events: %s\n", strerror(errno));
		return false;
	}
	if ((size_t)bytes % sizeof(events[0]) != 0) {
		fprintf(stderr, "usbgamepad: malformed FunctionFS event block\n");
		return false;
	}

	size_t count = (size_t)bytes / sizeof(events[0]);
	for (size_t i = 0; i < count; i++) {
		switch (events[i].type) {
		case FUNCTIONFS_BIND:
			gamepad->bound = true;
			fprintf(stderr, "usbgamepad: FunctionFS BIND\n");
			break;
		case FUNCTIONFS_UNBIND:
			gamepad->bound = false;
			gamepad->enabled = false;
			fprintf(stderr, "usbgamepad: FunctionFS UNBIND\n");
			break;
		case FUNCTIONFS_DISABLE:
			gamepad->enabled = false;
			fprintf(stderr, "usbgamepad: FunctionFS DISABLE\n");
			break;
		case FUNCTIONFS_ENABLE:
			gamepad->enabled = true;
			*reportDirty = true;
			fprintf(stderr, "usbgamepad: FunctionFS ENABLE\n");
			break;
		case FUNCTIONFS_SETUP:
			gamepad->sawSetup = true;
			if (!handleSetup(gamepad, &events[i].u.setup))
				return false;
			break;
		case FUNCTIONFS_SUSPEND:
			fprintf(stderr, "usbgamepad: FunctionFS SUSPEND\n");
			break;
		case FUNCTIONFS_RESUME:
			*reportDirty = true;
			fprintf(stderr, "usbgamepad: FunctionFS RESUME\n");
			break;
		default: break;
		}
	}
	return true;
}

static void encodeReport(
	const FunctionFsGamepad* gamepad, const NxGamepadState* state, uint8_t report[NXGP_MAX_REPORT_SIZE]) {
	if (gamepad->profile != GAMEPAD_PROFILE_BROWSER)
		NxGamepad_encodePcReport(state, report);
	else
		NxGamepad_encodeBrowserReport(state, report);
}

static bool sendReport(FunctionFsGamepad* gamepad, const uint8_t report[NXGP_MAX_REPORT_SIZE]) {
	if (!gamepad->enabled)
		return false;
	ssize_t result = write(gamepad->epIn, report, gamepad->reportSize);
	if (result == (ssize_t)gamepad->reportSize)
		return true;
	if (result < 0 && (errno == EINTR || errno == EAGAIN))
		return false;
	if (result < 0 &&
		(errno == ENODEV || errno == ESHUTDOWN || errno == EPIPE || errno == ECONNRESET)) {
		gamepad->enabled = false;
		fprintf(stderr, "usbgamepad: FunctionFS IN endpoint unavailable: %s\n", strerror(errno));
		return false;
	}
	fprintf(stderr, "usbgamepad: FunctionFS report: %s\n",
		result < 0 ? strerror(errno) : "short write");
	return false;
}

static int bridgeInput(
	FunctionFsGamepad* gamepad, uint64_t automaticExitMs, NxGamepadLayout* activeLayout) {
	char inputPath[64];
	int inputFd = openTrimuiInput(inputPath, sizeof(inputPath));
	if (inputFd < 0)
		return 1;
	fprintf(stderr, "usbgamepad: reading %s\n", inputPath);

	AxisInfo axes[ABS_CNT];
	loadAxisInfo(inputFd, axes);
	NxGamepadState state;
	NxGamepad_init(&state);
	(void)NxGamepad_setLayout(&state, *activeLayout);
	resyncInput(inputFd, axes, &state);
	LocalControls controls;
	initLocalControls(&controls);
	fprintf(stderr,
		"usbgamepad: local controls layout=%s FN1=layout FN2=screen brightness_raw=%d haptic=%s voltage=%d\n",
		layoutName(state.layout), controls.restoreBrightnessRaw,
		controls.hapticEnabled ? "on" : "off", controls.hapticVoltage);

	uint8_t report[NXGP_MAX_REPORT_SIZE];
	bool reportDirty = true;
	bool dropping = false;
	bool wasConnected = false;
	bool retryUsb = false;
	bool layoutSwitchPending = false;
	bool layoutNeutralSent = false;
	NxGamepadLayout requestedLayout = state.layout;
	uint64_t chordStarted = 0;
	uint64_t automaticExitAt = automaticExitMs ? monotonicMs() + automaticExitMs : 0;
	uint64_t nextReportAt = monotonicMs();
	uint64_t nextUdcStateCheckAt = nextReportAt;
	uint64_t stuckUdcStateSince = 0;
	char lastUdcState[32] = {0};
	uint64_t rateWindowStarted = nextReportAt;
	uint64_t sourceFrames = 0;
	uint64_t inputChanges = 0;
	uint64_t encodedStateChanges = 0;
	uint64_t usbStateChanges = 0;
	uint64_t axisChanges[4] = {0, 0, 0, 0}; // LX, LY, RX, RY
	uint64_t stickFrames[2] = {0, 0};       // frames carrying a changed L/R stick
	bool stickChanged[2] = {false, false};
	uint64_t usbReports = 0;
	uint64_t usbOutputReports = 0;
	encodeReport(gamepad, &state, gamepad->currentReport);
	uint8_t lastSourceReport[NXGP_MAX_REPORT_SIZE];
	uint8_t lastSentReport[NXGP_MAX_REPORT_SIZE];
	memcpy(lastSourceReport, gamepad->currentReport, gamepad->reportSize);
	memcpy(lastSentReport, gamepad->currentReport, gamepad->reportSize);

	char readyStatus[160];
	snprintf(readyStatus, sizeof(readyStatus), "%s layout - FN1: layout | FN2: screen",
		layoutName(state.layout));
	notifyStatus(readyStatus);
	while (running) {
		struct pollfd descriptors[3] = {
			{.fd = inputFd, .events = POLLIN},
			{.fd = gamepad->ep0, .events = POLLIN},
			{.fd = gamepad->epOut, .events = POLLIN},
		};
		int pollResult = poll(descriptors, 3, REPORT_INTERVAL_MS);
		if (pollResult < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "usbgamepad: poll: %s\n", strerror(errno));
			break;
		}
		// Enumeration and HID class requests have strict host-side deadlines.
		// Service ep0 before draining the continuously active 100 Hz input
		// device so local controls and diagnostics can never delay Windows.
		if (pollResult > 0 && (descriptors[1].revents & POLLIN) &&
			!handleFunctionFsEvents(gamepad, &reportDirty)) {
			retryUsb = true;
			break;
		}
		if (pollResult > 0 && (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
			fprintf(stderr, "usbgamepad: FunctionFS control endpoint disconnected (revents=0x%x)\n",
				descriptors[1].revents);
			retryUsb = true;
			break;
		}
		// The lab profile deliberately does not interpret output bytes as rumble.
		// It records bounded hexadecimal diagnostics only, proving whether a real
		// host can open and write the interrupt-OUT endpoint without ever driving
		// Brick Pro hardware from an unvalidated packet.
		if (pollResult > 0 && (descriptors[2].revents & POLLIN)) {
			uint8_t output[64];
			ssize_t bytes;
			while ((bytes = read(gamepad->epOut, output, sizeof(output))) > 0) {
				usbOutputReports++;
				fprintf(stderr, "usbgamepad: interrupt OUT length=%zd data=", bytes);
				size_t shown = (size_t)bytes < 32 ? (size_t)bytes : 32;
				for (size_t i = 0; i < shown; i++)
					fprintf(stderr, "%s%02x", i ? " " : "", output[i]);
				if ((size_t)bytes > shown)
					fprintf(stderr, " ...");
				fputc('\n', stderr);
			}
			if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
				fprintf(stderr, "usbgamepad: FunctionFS OUT read: %s\n", strerror(errno));
				retryUsb = true;
				break;
			}
		}
		if (pollResult > 0 && gamepad->enabled &&
			(descriptors[2].revents & (POLLERR | POLLHUP | POLLNVAL))) {
			fprintf(stderr, "usbgamepad: FunctionFS OUT endpoint disconnected (revents=0x%x)\n",
				descriptors[2].revents);
			retryUsb = true;
			break;
		}
		if (pollResult > 0 && (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
			fprintf(stderr, "usbgamepad: input device disconnected\n");
			break;
		}
		if (pollResult > 0 && (descriptors[0].revents & POLLIN)) {
			struct input_event events[32];
			ssize_t bytes;
			while ((bytes = read(inputFd, events, sizeof(events))) > 0) {
				size_t count = (size_t)bytes / sizeof(events[0]);
				for (size_t i = 0; i < count; i++) {
					const struct input_event* event = &events[i];
					if (event->type == EV_SYN && event->code == SYN_DROPPED) {
						dropping = true;
						continue;
					}
					if (event->type == EV_SYN && event->code == SYN_REPORT) {
						sourceFrames++;
						if (stickChanged[0])
							stickFrames[0]++;
						if (stickChanged[1])
							stickFrames[1]++;
						stickChanged[0] = false;
						stickChanged[1] = false;
						if (dropping) {
							resyncInput(inputFd, axes, &state);
							dropping = false;
						}
						uint8_t sourceReport[NXGP_MAX_REPORT_SIZE];
						encodeReport(gamepad, &state, sourceReport);
						if (memcmp(sourceReport, lastSourceReport, gamepad->reportSize) != 0) {
							encodedStateChanges++;
							memcpy(lastSourceReport, sourceReport, gamepad->reportSize);
						}
						reportDirty = true;
						continue;
					}
					if (dropping)
						continue;
					bool changed = false;
					if (event->type == EV_KEY) {
						changed = NxGamepad_setKey(&state, event->code, event->value != 0);
						if (changed && event->value != 0 && event->code == NXGP_KEY_F1) {
							if (!layoutSwitchPending) {
								requestedLayout = NxGamepad_otherLayout(state.layout);
								layoutSwitchPending = true;
								layoutNeutralSent = false;
								fprintf(stderr, "usbgamepad: layout requested %s face_buttons=%s\n",
									layoutName(requestedLayout),
									NxGamepad_faceButtonsHeld(&state) ? "held" : "released");
								if (NxGamepad_faceButtonsHeld(&state))
									notifyStatus("Release A/B/X/Y to change layout");
								else
									notifyStatus("Changing layout...");
							} else {
								fprintf(stderr, "usbgamepad: layout request ignored; transition already pending\n");
							}
						} else if (changed && event->value != 0 && event->code == NXGP_KEY_F2) {
							uint64_t actionNow = monotonicMs();
							if (controls.screenOff || controls.screenOffAt != 0) {
								wakeScreen(&controls);
								notifyStatus("Screen on - FN2: turn off");
								fprintf(stderr, "usbgamepad: screen turned on\n");
							} else {
								notifyStatus("Screen off - press FN2 to wake");
								controls.screenOffAt = actionNow + SCREEN_OFF_DELAY_MS;
								fprintf(stderr, "usbgamepad: screen-off requested\n");
							}
							pulseHaptic(&controls, actionNow);
						}
					}
					else if (event->type == EV_ABS && event->code < ABS_CNT)
						changed = NxGamepad_setAbs(
							&state, event->code, normalizeAbs(event->code, event->value, axes));
					if (changed) {
						inputChanges++;
						switch (event->code) {
						case ABS_X: axisChanges[0]++; stickChanged[0] = true; break;
						case ABS_Y: axisChanges[1]++; stickChanged[0] = true; break;
						case ABS_RX: axisChanges[2]++; stickChanged[1] = true; break;
						case ABS_RY: axisChanges[3]++; stickChanged[1] = true; break;
						default: break;
						}
						reportDirty = true;
					}
				}
			}
			if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
				fprintf(stderr, "usbgamepad: input read: %s\n", strerror(errno));
				break;
			}
		}
		uint64_t now = monotonicMs();
		if (now >= nextUdcStateCheckAt) {
			char udcState[32];
			if (readUdcState(gamepad, udcState, sizeof(udcState))) {
				if (strcmp(lastUdcState, udcState) != 0) {
					fprintf(stderr, "usbgamepad: UDC state %s\n", udcState);
					snprintf(lastUdcState, sizeof(lastUdcState), "%s", udcState);
					stuckUdcStateSince = 0;
				}
				if (!gamepad->enabled && udcStateCanBeStuck(udcState)) {
					if (stuckUdcStateSince == 0)
						stuckUdcStateSince = now;
					else if (now - stuckUdcStateSince >= ENUMERATION_STUCK_MS) {
						fprintf(stderr,
							"usbgamepad: USB enumeration stalled in UDC state %s for %u ms\n",
							udcState, ENUMERATION_STUCK_MS);
						retryUsb = true;
						break;
					}
				} else {
					stuckUdcStateSince = 0;
				}
			}
			nextUdcStateCheckAt = now + 500;
		}

		if (layoutSwitchPending) {
			if (layoutNeutralSent || !gamepad->enabled) {
				(void)NxGamepad_setLayout(&state, requestedLayout);
				*activeLayout = state.layout;
				(void)persistLayout(state.layout);
				layoutSwitchPending = false;
				layoutNeutralSent = false;
				reportDirty = true;
				char status[96];
				snprintf(status, sizeof(status), "Layout: %s", layoutName(state.layout));
				notifyStatus(status);
				fprintf(stderr, "usbgamepad: layout committed %s\n", layoutName(state.layout));
			} else if (!NxGamepad_faceButtonsHeld(&state)) {
				NxGamepadState neutralState = state;
				NxGamepad_clearFaceButtons(&neutralState);
				uint8_t neutralReport[NXGP_MAX_REPORT_SIZE];
				encodeReport(gamepad, &neutralState, neutralReport);
				memcpy(gamepad->currentReport, neutralReport, gamepad->reportSize);
				if (sendReport(gamepad, neutralReport)) {
					if (memcmp(neutralReport, lastSentReport, gamepad->reportSize) != 0) {
						usbStateChanges++;
						memcpy(lastSentReport, neutralReport, gamepad->reportSize);
					}
					usbReports++;
					nextReportAt = now + REPORT_INTERVAL_MS;
					layoutNeutralSent = true;
					fprintf(stderr, "usbgamepad: layout neutral face-button report sent\n");
				}
			}
		}
		if (controls.hapticOffAt != 0 && now >= controls.hapticOffAt)
			stopHaptic(&controls);
		if (controls.screenOffAt != 0 && now >= controls.screenOffAt) {
			finishScreenOff(&controls);
			if (controls.screenOff)
				fprintf(stderr, "usbgamepad: screen turned off (backlight raw 0)\n");
		}
		if (automaticExitAt && now >= automaticExitAt)
			running = 0;
		if (NxGamepad_exitChordActive(&state)) {
			if (chordStarted == 0)
				chordStarted = now;
			else if (now - chordStarted >= EXIT_HOLD_MS)
				running = 0;
		} else {
			chordStarted = 0;
		}

		if (gamepad->enabled != wasConnected) {
			if (gamepad->enabled) {
				char status[128];
				snprintf(status, sizeof(status), "Connected - %s layout | FN1 layout | FN2 screen",
					layoutName(state.layout));
				notifyStatus(status);
			} else {
				snprintf(readyStatus, sizeof(readyStatus), "%s layout - connect the bottom USB-C port",
					layoutName(state.layout));
				notifyStatus(readyStatus);
			}
			wasConnected = gamepad->enabled;
			reportDirty = true;
		}

		bool periodicReportDue = gamepad->enabled && now >= nextReportAt;
		if (reportDirty || periodicReportDue) {
			encodeReport(gamepad, &state, report);
			memcpy(gamepad->currentReport, report, gamepad->reportSize);
			if (sendReport(gamepad, report)) {
				if (memcmp(report, lastSentReport, gamepad->reportSize) != 0) {
					usbStateChanges++;
					memcpy(lastSentReport, report, gamepad->reportSize);
				}
				reportDirty = false;
				usbReports++;
				nextReportAt = now + REPORT_INTERVAL_MS;
			} else if (periodicReportDue) {
				nextReportAt = now + REPORT_INTERVAL_MS;
			}
		}

		if (now - rateWindowStarted >= RATE_WINDOW_MS) {
			double seconds = (double)(now - rateWindowStarted) / 1000.0;
			double sourceRate = sourceFrames / seconds;
			double changeRate = inputChanges / seconds;
			double encodedChangeRate = encodedStateChanges / seconds;
			double usbStateChangeRate = usbStateChanges / seconds;
			double leftStickRate = stickFrames[0] / seconds;
			double rightStickRate = stickFrames[1] / seconds;
			double usbRate = usbReports / seconds;
			double usbOutputRate = usbOutputReports / seconds;
			fprintf(stderr,
				"usbgamepad: rates profile=%s source_frames=%.1fHz input_changes=%.1f/s "
				"stick_frames=L%.1f/R%.1fHz axes=LX%.1f/LY%.1f/RX%.1f/RY%.1fHz "
				"encoded_changes=%.1fHz usb_state_changes=%.1fHz usb_reports=%.1fHz "
				"usb_output_reports=%.1fHz\n",
				profileName(gamepad->profile), sourceRate, changeRate, leftStickRate, rightStickRate,
				axisChanges[0] / seconds, axisChanges[1] / seconds, axisChanges[2] / seconds,
				axisChanges[3] / seconds, encodedChangeRate, usbStateChangeRate, usbRate,
				usbOutputRate);
			if (gamepad->enabled) {
				char status[160];
				snprintf(status, sizeof(status), "%s | src %.0f | new %.0f | USB %.0f",
					layoutName(state.layout), sourceRate, usbStateChangeRate, usbRate);
				notifyStatus(status);
			}
			rateWindowStarted = now;
			sourceFrames = 0;
			inputChanges = 0;
			encodedStateChanges = 0;
			usbStateChanges = 0;
			memset(axisChanges, 0, sizeof(axisChanges));
			memset(stickFrames, 0, sizeof(stickFrames));
			usbReports = 0;
			usbOutputReports = 0;
		}
	}

	NxGamepad_init(&state);
	encodeReport(gamepad, &state, report);
	memcpy(gamepad->currentReport, report, gamepad->reportSize);
	(void)sendReport(gamepad, report); // release every host-side button on exit
	wakeScreen(&controls);
	stopHaptic(&controls);
	close(inputFd);
	*activeLayout = state.layout;
	return retryUsb ? 2 : running ? 1 : 0;
}

static void createMarker(void) {
	int fd = open(ACTIVE_MARKER, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0)
		close(fd);
}

static bool parseProfile(const char* value, GamepadProfile* profile) {
	if (strcmp(value, "browser") == 0 || strcmp(value, "hid") == 0) {
		*profile = GAMEPAD_PROFILE_BROWSER;
		return true;
	}
	if (strcmp(value, "pc") == 0 || strcmp(value, "sdl") == 0) {
		*profile = GAMEPAD_PROFILE_PC;
		return true;
	}
	if (strcmp(value, "out-probe") == 0) {
		*profile = GAMEPAD_PROFILE_OUTPUT_PROBE;
		return true;
	}
	return false;
}

static bool parseLayout(const char* value, NxGamepadLayout* layout) {
	if (strcmp(value, "xbox") == 0) {
		*layout = NXGP_LAYOUT_XBOX;
		return true;
	}
	if (strcmp(value, "nintendo") == 0 || strcmp(value, "switch") == 0) {
		*layout = NXGP_LAYOUT_NINTENDO;
		return true;
	}
	return false;
}

static void printUsage(const char* program) {
	fprintf(stderr,
		"usage: %s [--profile browser|pc|out-probe] [--layout xbox|nintendo] "
		"[--test-seconds SECONDS] [--probe-functionfs|--probe-functionfs-out|--restore]\n",
		program);
}

int main(int argc, char* argv[]) {
	// The public pak is the Steam/SDL-compatible profile. The old browser
	// layout remains available only when explicitly requested for diagnostics.
	GamepadProfile profile = GAMEPAD_PROFILE_PC;
	NxGamepadLayout initialLayout = NXGP_LAYOUT_XBOX;
	uint64_t automaticExitMs = 0;
	bool probeFunctionFs = false;
	bool probeFunctionFsOut = false;
	bool restoreOnly = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
			if (!parseProfile(argv[++i], &profile)) {
				fprintf(stderr, "usbgamepad: profile must be 'browser', 'pc', or 'out-probe'\n");
				return 2;
			}
		} else if (strncmp(argv[i], "--profile=", 10) == 0) {
			if (!parseProfile(argv[i] + 10, &profile)) {
				fprintf(stderr, "usbgamepad: profile must be 'browser', 'pc', or 'out-probe'\n");
				return 2;
			}
		} else if (strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
			if (!parseLayout(argv[++i], &initialLayout)) {
				fprintf(stderr, "usbgamepad: layout must be 'xbox' or 'nintendo'\n");
				return 2;
			}
		} else if (strncmp(argv[i], "--layout=", 9) == 0) {
			if (!parseLayout(argv[i] + 9, &initialLayout)) {
				fprintf(stderr, "usbgamepad: layout must be 'xbox' or 'nintendo'\n");
				return 2;
			}
		} else if (strcmp(argv[i], "--test-seconds") == 0 && i + 1 < argc) {
			char* end = NULL;
			unsigned long seconds = strtoul(argv[++i], &end, 10);
			if (!end || *end != '\0' || seconds == 0 || seconds > 120) {
				fprintf(stderr, "usbgamepad: test duration must be between 1 and 120 seconds\n");
				return 2;
			}
			automaticExitMs = (uint64_t)seconds * 1000u;
		} else if (strcmp(argv[i], "--probe-functionfs") == 0) {
			probeFunctionFs = true;
		} else if (strcmp(argv[i], "--probe-functionfs-out") == 0) {
			probeFunctionFsOut = true;
		} else if (strcmp(argv[i], "--restore") == 0) {
			restoreOnly = true;
		} else {
			printUsage(argv[0]);
			return 2;
		}
	}
	if ((probeFunctionFs && probeFunctionFsOut) ||
		((probeFunctionFs || probeFunctionFsOut) && restoreOnly) ||
		(restoreOnly && automaticExitMs != 0)) {
		printUsage(argv[0]);
		return 2;
	}

	FunctionFsGamepad gamepad;
	initFunctionFsGamepad(&gamepad, profile);
	if (probeFunctionFs || probeFunctionFsOut) {
		if (!removeStaleGamepadFunction())
			return 1;
		bool ready = configureFunctionFs(&gamepad, probeFunctionFsOut);
		closeFunctionFs(&gamepad);
		if (ready)
			fprintf(stderr,
				"usbgamepad: FunctionFS HID descriptors accepted by kernel (%s profile, out=%s)\n",
				profileName(profile), probeFunctionFsOut ? "yes" : "no");
		return ready ? 0 : 1;
	}
	if (restoreOnly) {
		restoreScreenFromMarker();
		(void)writeText(RUMBLE_GPIO, "0\n");
		bool restored = restoreAdb(&gamepad);
		if (restored)
			unlink(ACTIVE_MARKER);
		return restored ? 0 : 1;
	}

	struct sigaction stopAction;
	memset(&stopAction, 0, sizeof(stopAction));
	sigemptyset(&stopAction.sa_mask);
	stopAction.sa_handler = handleSignal;
	(void)sigaction(SIGINT, &stopAction, NULL);
	(void)sigaction(SIGTERM, &stopAction, NULL);
	struct sigaction timeoutAction;
	memset(&timeoutAction, 0, sizeof(timeoutAction));
	sigemptyset(&timeoutAction.sa_mask);
	timeoutAction.sa_handler = handleControlTimeout;
	(void)sigaction(SIGALRM, &timeoutAction, NULL);
	// An ADB-launched timed hardware test loses its parent shell when USB is
	// rebound.  Ignore that test-only hangup and rely on the built-in deadline;
	// normal pak runs still treat SIGHUP as a request to restore ADB at once.
	stopAction.sa_handler = automaticExitMs ? SIG_IGN : handleSignal;
	(void)sigaction(SIGHUP, &stopAction, NULL);
	restoreScreenFromMarker();
	(void)writeText(RUMBLE_GPIO, "0\n");
	createMarker();
	if (!configureGamepad(&gamepad)) {
		notifyStatus("USB gamepad setup failed");
		restoreScreenFromMarker();
		(void)writeText(RUMBLE_GPIO, "0\n");
		if (restoreAdb(&gamepad))
			unlink(ACTIVE_MARKER);
		return 1;
	}

	int result;
	int usbRecoveryAttempts = 0;
	for (;;) {
		result = bridgeInput(&gamepad, automaticExitMs, &initialLayout);
		if (result != 2 || !running)
			break;
		if (usbRecoveryAttempts >= MAX_USB_RECOVERY_ATTEMPTS) {
			fprintf(stderr, "usbgamepad: USB recovery limit reached\n");
			result = 1;
			break;
		}
		usbRecoveryAttempts++;
		fprintf(stderr, "usbgamepad: recovering USB gadget attempt %d/%d\n",
			usbRecoveryAttempts, MAX_USB_RECOVERY_ATTEMPTS);
		notifyStatus("USB reconnecting...");
		(void)unbindGadget();
		closeFunctionFs(&gamepad);
		initFunctionFsGamepad(&gamepad, profile);
		if (!configureGamepad(&gamepad)) {
			fprintf(stderr, "usbgamepad: USB gadget recovery failed\n");
			result = 1;
			break;
		}
	}
	restoreScreenFromMarker();
	(void)writeText(RUMBLE_GPIO, "0\n");
	notifyStatus("Restoring USB data mode...");
	bool restored = restoreAdb(&gamepad);
	if (restored)
		unlink(ACTIVE_MARKER);
	return result != 0 || !restored;
}
