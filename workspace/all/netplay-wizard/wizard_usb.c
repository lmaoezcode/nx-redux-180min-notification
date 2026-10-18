/*
 * USB Cable connection mode: one device's second (top) port to the other's
 * main (bottom) port. usblink.elf (workspace/all/usblink) does the link — this
 * file only starts it, waits for the handshake and stops it. The wizard never
 * learns which end is USB host: the daemon reports the peer's address, and
 * netplay host/client stays whatever the user picked on the role menu.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include "api.h"
#include "defines.h"
#include "ui_buttonhintbar.h"
#include "ui_message.h"
#include "usblink_state.h"
#include "utils.h"
#include "wizard.h"

#define USBLINK_STATE_PATH "/tmp/usblink.state"
#define USBLINK_PID_PATH "/tmp/usblink.pid"
// Written by `nx_usb_mode.sh link-start`; it also holds the USB mode to give
// back (MTP or ADB) and stays until `link-stop` has done so.
#define USBLINK_MARKER_PATH "/tmp/nx_usb_link_active"

// The link is started and stopped through nx_usb_mode.sh (link-start /
// link-stop), never by running usblink.elf directly: usblink rebinds the UDC of
// the same gadget as MTP, USB Storage and USB Gamepad, and only nx_usb_mode.sh
// takes the USB guard, stops MTP first and gives it back afterwards.

// Any pidfile, live pid or not, or the marker: a SIGKILLed daemon leaves its
// pidfile behind together with a gadget that may still be linked, and that
// dead-pid case is exactly the one link-stop has to repair; the marker covers a
// link-stop that was cut off before MTP came back. link-stop removes both, so
// this goes false again once anything has cleaned up.
bool wiz_usb_link_may_be_up(void) {
	return access(USBLINK_PID_PATH, F_OK) == 0 || access(USBLINK_MARKER_PATH, F_OK) == 0;
}

void wiz_usb_link_stop(void) {
	// Called on every wizard teardown, USB session or not. Without a pidfile or
	// marker there is nothing to repair, and link-stop (guarded, pauses adb for a
	// moment) must not run for Hotspot/WiFi sessions.
	if (!wiz_usb_link_may_be_up())
		return;
	system("nx_usb_mode.sh link-stop >/dev/null 2>&1");
}

// GFX_blitText centres inside dst_rect, so a full-width rect at y is a centred
// line at y. Same as wizard_net.c's wiz_net_blit (static there).
static void wiz_usb_blit(TTF_Font* f, const char* text, int y) {
	GFX_blitText(f, text, 0, COLOR_WHITE, wiz_screen,
				 &(SDL_Rect){0, y, wiz_screen->w, SCALE1(FONT_LARGE)});
}

// The three-tier layout of wizard_net.c's wiz_net_render_waiting(), with the
// instruction split over two medium lines; the status line moves down by the
// same one medium line so nothing overlaps.
static void wiz_usb_render(const char* status, bool cancelable) {
	int center_y = wiz_screen->h / 2;

	GFX_clear(wiz_screen);
	wiz_usb_blit(font.large, "USB Cable", center_y - SCALE1(FONT_LARGE + PADDING));
	wiz_usb_blit(font.medium, "Connect one device's top port", center_y + SCALE1(5));
	wiz_usb_blit(font.medium, "to the other device's bottom port", center_y + SCALE1(5 + FONT_MEDIUM));
	wiz_usb_blit(font.small, status, center_y + SCALE1(28 + FONT_MEDIUM));
	if (cancelable)
		UI_renderButtonHintBar(wiz_screen, (char*[]){"B", "CANCEL", NULL});
	GFX_flip(wiz_screen);
}

// Terminal message, drawn the way wizard.c's show_message() draws its own.
static void wiz_usb_fail(const char* message, int hold_ms) {
	GFX_clear(wiz_screen);
	UI_renderCenteredMessage(wiz_screen, message);
	GFX_flip(wiz_screen);
	SDL_Delay(hold_ms);
	wiz_usb_link_stop();
}

int wiz_usb_link_up(WizSession* s) {
	// `start` blocks until the daemon publishes its first state (up to ~3 s);
	// put the cabling hint up first rather than leaving the mode menu on show
	// with B dead underneath it.
	wiz_usb_render("Starting USB link...", false);
	if (system("nx_usb_mode.sh link-start >/dev/null 2>&1") != 0) {
		// link-start repairs and gives MTP back itself when the daemon fails to
		// start; the stop inside wiz_usb_fail() still covers a daemon that was
		// slow to report and is alive now (it does nothing when nothing is left).
		wiz_usb_fail("USB link could not start.", 1500);
		return -1;
	}

	bool dirty = true;
	// No wall-clock ceiling: like the host's waiting screen, this waits for a
	// person to plug a cable in, and B is live every frame.
	while (1) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_B) || app_quit) {
			wiz_usb_link_stop();
			return -2;
		}

		UsbLinkState st;
		if (usblink_state_read(USBLINK_STATE_PATH, &st) == 0) {
			// The address ends up in a ping command line (wiz_client_known_host),
			// so anything that is not a dotted quad reads as not linked yet.
			struct in_addr addr;
			if (!strcmp(st.link, "up") && inet_pton(AF_INET, st.peer_ip, &addr) == 1) {
				snprintf(s->peer_ip, sizeof(s->peer_ip), "%s", st.peer_ip);
				return 0;
			}
			if (!strcmp(st.link, "error")) {
				const char* why = !strcmp(st.error, "version")
									  ? "Both devices need the same\nNXRedux version."
									  : "USB link unavailable\non this device.";
				wiz_usb_fail(why, 2000);
				return -1;
			}
		}

		PWR_update(&dirty, NULL, NULL, NULL);
		if (dirty) {
			wiz_usb_render("Waiting for the other device...", true);
			dirty = false;
		} else {
			GFX_sync();
		}
	}
}
