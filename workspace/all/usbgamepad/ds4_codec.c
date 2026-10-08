#include "ds4_codec.h"

#include <string.h>

static bool pressed(const NxGamepadState* state, NxGamepadButton button) {
	return (state->buttons & (1u << (unsigned)button)) != 0;
}

static uint8_t axis_u8(int16_t value) {
	return (uint8_t)(((int32_t)value + 32768) >> 8);
}

static uint8_t hat(const NxGamepadState* state) {
	bool up=pressed(state,NXGP_BUTTON_DPAD_UP), down=pressed(state,NXGP_BUTTON_DPAD_DOWN);
	bool left=pressed(state,NXGP_BUTTON_DPAD_LEFT), right=pressed(state,NXGP_BUTTON_DPAD_RIGHT);
	int x=right!=left ? (right?1:-1):0, y=down!=up ? (down?1:-1):0;
	if (!x && y<0) return 0;
	if (x>0 && y<0) return 1;
	if (x>0 && !y) return 2;
	if (x>0 && y>0) return 3;
	if (!x && y>0) return 4;
	if (x<0 && y>0) return 5;
	if (x<0 && !y) return 6;
	if (x<0 && y<0) return 7;
	return 8;
}

static void face_buttons(const NxGamepadState* state, uint8_t* value) {
	NxGamepadButton cross=NXGP_BUTTON_SOUTH, circle=NXGP_BUTTON_EAST;
	NxGamepadButton square=NXGP_BUTTON_WEST, triangle=NXGP_BUTTON_NORTH;
	if (state->layout==NXGP_LAYOUT_NINTENDO) {
		cross=NXGP_BUTTON_EAST; circle=NXGP_BUTTON_SOUTH;
		square=NXGP_BUTTON_NORTH; triangle=NXGP_BUTTON_WEST;
	}
	if (pressed(state,square)) *value|=0x10;
	if (pressed(state,cross)) *value|=0x20;
	if (pressed(state,circle)) *value|=0x40;
	if (pressed(state,triangle)) *value|=0x80;
}

void NxDs4_encodeUsbInput(const NxGamepadState* state, const NxDs4UsbInputMeta* meta,
	uint8_t report[NX_DS4_USB_INPUT_SIZE]) {
	memset(report,0,NX_DS4_USB_INPUT_SIZE);
	report[0]=NX_DS4_USB_INPUT_ID;
	report[1]=axis_u8(state->left_x); report[2]=axis_u8(state->left_y);
	report[3]=axis_u8(state->right_x); report[4]=axis_u8(state->right_y);
	report[5]=hat(state); face_buttons(state,&report[5]);
	if (pressed(state,NXGP_BUTTON_L1)) report[6]|=0x01;
	if (pressed(state,NXGP_BUTTON_R1)) report[6]|=0x02;
	if (pressed(state,NXGP_BUTTON_L2)) report[6]|=0x04;
	if (pressed(state,NXGP_BUTTON_R2)) report[6]|=0x08;
	if (pressed(state,NXGP_BUTTON_SELECT)) report[6]|=0x10;
	if (pressed(state,NXGP_BUTTON_START)) report[6]|=0x20;
	if (pressed(state,NXGP_BUTTON_L3)) report[6]|=0x40;
	if (pressed(state,NXGP_BUTTON_R3)) report[6]|=0x80;
	if (pressed(state,NXGP_BUTTON_HOME)) report[7]|=0x01;
	report[7]|=(uint8_t)((meta->sequence&0x3f)<<2);
	report[8]=pressed(state,NXGP_BUTTON_L2)?255:0;
	report[9]=pressed(state,NXGP_BUTTON_R2)?255:0;
	/* No gyro/accelerometer/touch hardware is advertised. Fields stay neutral,
	 * and num_touch_reports at byte 33 remains zero. */
	uint8_t battery=meta->battery_level>10?10:meta->battery_level;
	report[30]=(uint8_t)(battery|(meta->usb_cable?0x10:0));
}

static bool decode_common(const uint8_t* common,size_t size,NxDs4Feedback* feedback) {
	if (!common || size!=31 || !feedback) return false;
	memset(feedback,0,sizeof(*feedback));
	uint8_t flags=common[0];
	feedback->motor_valid=(flags&0x01)!=0;
	feedback->led_valid=(flags&0x02)!=0;
	feedback->blink_valid=(flags&0x04)!=0;
	feedback->weak_motor=common[3];
	feedback->strong_motor=common[4];
	feedback->led_red=common[5]; feedback->led_green=common[6]; feedback->led_blue=common[7];
	feedback->blink_on=common[8]; feedback->blink_off=common[9];
	return true;
}

bool NxDs4_decodeUsbOutputPacket(const uint8_t* packet,size_t size,NxDs4Feedback* feedback) {
	return packet && size==NX_DS4_USB_OUTPUT_PACKET_SIZE && packet[0]==NX_DS4_USB_OUTPUT_ID &&
		decode_common(packet+1,size-1,feedback);
}

bool NxDs4_decodeUsbOutputControl(uint8_t report_id,const uint8_t* payload,size_t size,
	NxDs4Feedback* feedback) {
	return report_id==NX_DS4_USB_OUTPUT_ID && decode_common(payload,size,feedback);
}
