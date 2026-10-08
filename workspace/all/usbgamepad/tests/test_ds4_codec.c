#include "../ds4_codec.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void neutral_and_axes(void) {
	NxGamepadState state; NxGamepad_init(&state);
	NxDs4UsbInputMeta meta={.battery_level=7,.usb_cable=true,.sequence=0x2a};
	uint8_t report[NX_DS4_USB_INPUT_SIZE]; NxDs4_encodeUsbInput(&state,&meta,report);
	assert(report[0]==0x01 && report[1]==128 && report[2]==128 && report[3]==128 && report[4]==128);
	assert(report[5]==8 && report[6]==0 && report[7]==(uint8_t)(0x2a<<2));
	assert(report[8]==0 && report[9]==0 && report[30]==0x17 && report[33]==0);
	for (size_t i=10;i<30;++i) assert(report[i]==0);
	for (size_t i=31;i<64;++i) assert(report[i]==0);
	NxGamepad_setAbs(&state,NXGP_ABS_X,-32768); NxGamepad_setAbs(&state,NXGP_ABS_Y,32767);
	NxGamepad_setAbs(&state,NXGP_ABS_RX,-1); NxGamepad_setAbs(&state,NXGP_ABS_RY,0);
	NxDs4_encodeUsbInput(&state,&meta,report);
	assert(report[1]==0 && report[2]==255 && report[3]==127 && report[4]==128);
}

static void buttons_layout_hat_and_triggers(void) {
	NxGamepadState state; NxGamepad_init(&state); NxDs4UsbInputMeta meta={0};
	uint8_t report[NX_DS4_USB_INPUT_SIZE];
	NxGamepad_setKey(&state,NXGP_BTN_A,true); /* physical bottom -> Cross */
	NxGamepad_setKey(&state,NXGP_BTN_B,true); /* physical right -> Circle */
	NxGamepad_setKey(&state,NXGP_BTN_TL,true); NxGamepad_setKey(&state,NXGP_BTN_TR2,true);
	NxGamepad_setKey(&state,NXGP_BTN_SELECT,true); NxGamepad_setKey(&state,NXGP_BTN_THUMBR,true);
	NxGamepad_setKey(&state,NXGP_KEY_HOMEPAGE,true);
	NxGamepad_setAbs(&state,NXGP_ABS_HAT0X,1); NxGamepad_setAbs(&state,NXGP_ABS_HAT0Y,-1);
	NxDs4_encodeUsbInput(&state,&meta,report);
	assert(report[5]==(uint8_t)(0x20|0x40|1));
	assert(report[6]==(uint8_t)(0x01|0x08|0x10|0x80));
	assert(report[7]==0x01 && report[8]==0 && report[9]==255);
	NxGamepad_setLayout(&state,NXGP_LAYOUT_NINTENDO);
	NxDs4_encodeUsbInput(&state,&meta,report);
	assert((report[5]&0xf0)==(0x20|0x40)); /* A/B labels still become host A/B. */
	NxGamepad_setKey(&state,NXGP_BTN_A,false);
	NxDs4_encodeUsbInput(&state,&meta,report);
	assert((report[5]&0xf0)==0x20); /* physical right A -> Cross in Nintendo mode */
	NxGamepad_setKey(&state,NXGP_BTN_B,false);
	NxGamepad_setKey(&state,NXGP_BTN_A,true);
	NxDs4_encodeUsbInput(&state,&meta,report); assert((report[5]&0xf0)==0x40);
	NxGamepad_setKey(&state,NXGP_BTN_A,false); NxGamepad_setKey(&state,NXGP_BTN_X,true);
	NxDs4_encodeUsbInput(&state,&meta,report); assert((report[5]&0xf0)==0x80);
	NxGamepad_setKey(&state,NXGP_BTN_X,false); NxGamepad_setKey(&state,NXGP_BTN_Y,true);
	NxDs4_encodeUsbInput(&state,&meta,report); assert((report[5]&0xf0)==0x10);
}

static void all_hat_directions(void) {
	static const int8_t xy[9][2]={{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,0}};
	NxGamepadState state; NxDs4UsbInputMeta meta={0}; uint8_t report[NX_DS4_USB_INPUT_SIZE];
	for (uint8_t expected=0;expected<9;++expected) {
		NxGamepad_init(&state);
		NxGamepad_setAbs(&state,NXGP_ABS_HAT0X,xy[expected][0]);
		NxGamepad_setAbs(&state,NXGP_ABS_HAT0Y,xy[expected][1]);
		NxDs4_encodeUsbInput(&state,&meta,report);
		assert((report[5]&0x0f)==expected);
	}
}

static void output_decode(void) {
	uint8_t packet[NX_DS4_USB_OUTPUT_PACKET_SIZE]={0}; packet[0]=0x05; packet[1]=0x07;
	packet[4]=0x22; packet[5]=0xcc; packet[6]=1; packet[7]=2; packet[8]=3; packet[9]=4; packet[10]=5;
	NxDs4Feedback f; assert(NxDs4_decodeUsbOutputPacket(packet,sizeof(packet),&f));
	assert(f.motor_valid && f.weak_motor==0x22 && f.strong_motor==0xcc);
	assert(f.led_valid && f.led_red==1 && f.led_green==2 && f.led_blue==3);
	assert(f.blink_valid && f.blink_on==4 && f.blink_off==5);
	assert(NxDs4_decodeUsbOutputControl(0x05,packet+1,31,&f));
	assert(!NxDs4_decodeUsbOutputControl(0x04,packet+1,31,&f));
	assert(!NxDs4_decodeUsbOutputControl(0x05,packet+1,30,&f));
	packet[0]=0x04; assert(!NxDs4_decodeUsbOutputPacket(packet,sizeof(packet),&f));
	assert(!NxDs4_decodeUsbOutputPacket(packet,sizeof(packet)-1,&f));
}

int main(void) {
	neutral_and_axes(); buttons_layout_hat_and_triggers(); all_hat_directions(); output_decode();
	puts("ds4 codec golden tests passed (pure codec; no USB or motor)"); return 0;
}
