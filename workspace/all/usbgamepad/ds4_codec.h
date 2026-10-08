#ifndef NX_USB_GAMEPAD_DS4_CODEC_H
#define NX_USB_GAMEPAD_DS4_CODEC_H

#include "gamepad_mapper.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	NX_DS4_USB_INPUT_SIZE = 64,
	NX_DS4_USB_OUTPUT_PACKET_SIZE = 32,
	NX_DS4_USB_OUTPUT_CONTROL_SIZE = 31,
	NX_DS4_USB_INPUT_ID = 0x01,
	NX_DS4_USB_OUTPUT_ID = 0x05,
};

typedef struct {
	uint8_t battery_level; /* 0..10; values above 10 are clamped. */
	bool usb_cable;
	uint8_t sequence; /* Low six bits are encoded in the DS4 button counter. */
} NxDs4UsbInputMeta;

typedef struct {
	bool motor_valid;
	uint8_t weak_motor;
	uint8_t strong_motor;
	bool led_valid;
	uint8_t led_red;
	uint8_t led_green;
	uint8_t led_blue;
	bool blink_valid;
	uint8_t blink_on;
	uint8_t blink_off;
} NxDs4Feedback;

void NxDs4_encodeUsbInput(const NxGamepadState* state, const NxDs4UsbInputMeta* meta,
	uint8_t report[NX_DS4_USB_INPUT_SIZE]);

/* Full packet form includes report ID 0x05 at byte zero. */
bool NxDs4_decodeUsbOutputPacket(const uint8_t* packet, size_t size, NxDs4Feedback* feedback);

/* FunctionFS SET_REPORT places ID 0x05 in wValue and exposes only the 31-byte
 * data stage. This entry point makes that distinction explicit. */
bool NxDs4_decodeUsbOutputControl(uint8_t report_id, const uint8_t* payload, size_t size,
	NxDs4Feedback* feedback);

#endif
