/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __GOOGLE_MODEM_NOTIFIER_H__
#define __GOOGLE_MODEM_NOTIFIER_H__

enum modem_event {
	MODEM_EVENT_RESET	= 1,
	MODEM_EVENT_EXIT,
	MODEM_EVENT_ONLINE	= 4,
	MODEM_EVENT_OFFLINE	= 5,
	MODEM_EVENT_WATCHDOG	= 9,
};

enum modem_voice_call_event {
	MODEM_VOICE_CALL_OFF	= 0,
	MODEM_VOICE_CALL_ON	= 1,
};

#endif /* __GOOGLE_MODEM_NOTIFIER_H__ */
