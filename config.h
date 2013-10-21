#ifndef CJET_CONFIG_H
#define CJET_CONFIG_H

#define SERVER_PORT \
	11122
#define LISTEN_BACKLOG \
	40
/*
 * It is somehow beneficial if this size is 32 bit aligned.
 */
#define MAX_MESSAGE_SIZE \
	512

/* Linux specific configs */
#define MAX_EPOLL_EVENTS \
	100

#endif
