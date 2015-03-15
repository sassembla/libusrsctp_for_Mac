/*
 * Copyright (C) 2011-2012 Michael Tuexen
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Usage: discard_server [local_encaps_port] [remote_encaps_port]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#if !defined(__Userspace_os_Windows)
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <usrsctp.h>

#define BUFFER_SIZE 10240

const int use_cb = 0;

static int
receive_cb(struct socket *sock, union sctp_sockstore addr, void *data,
           size_t datalen, struct sctp_rcvinfo rcv, int flags, void *ulp_info)
{
	char name[INET6_ADDRSTRLEN];

	if (data) {
		if (flags & MSG_NOTIFICATION) {
			printf("Notification of length %d received.\n", (int)datalen);
		} else {
			printf("Msg of length %d received from %s:%u on stream %d with SSN %u and TSN %u, PPID %d, context %u.\n",
			       (int)datalen,
			       addr.sa.sa_family == AF_INET ?
			           inet_ntop(AF_INET, &addr.sin.sin_addr, name, INET6_ADDRSTRLEN):
			           inet_ntop(AF_INET6, &addr.sin6.sin6_addr, name, INET6_ADDRSTRLEN),
			       ntohs(addr.sin.sin_port),
			       rcv.rcv_sid,
			       rcv.rcv_ssn,
			       rcv.rcv_tsn,
			       ntohl(rcv.rcv_ppid),
			       rcv.rcv_context);
		}
		free(data);
	}
	return 1;
}

int
main(int argc, char *argv[])
{
	struct socket *sock;
	struct sockaddr_in6 addr;
	struct sctp_udpencaps encaps;
	struct sctp_event event;
	uint16_t event_types[] = {SCTP_ASSOC_CHANGE,
	                          SCTP_PEER_ADDR_CHANGE,
	                          SCTP_REMOTE_ERROR,
	                          SCTP_SHUTDOWN_EVENT,
	                          SCTP_ADAPTATION_INDICATION,
	                          SCTP_PARTIAL_DELIVERY_EVENT};
	unsigned int i;
	struct sctp_assoc_value av;
	const int on = 1;
	int n, flags;
	socklen_t from_len;
	char buffer[BUFFER_SIZE];
	char name[INET6_ADDRSTRLEN];
	socklen_t infolen;
	struct sctp_rcvinfo rcv_info;
	unsigned int infotype;

	if (argc > 1) {
		usrsctp_init(atoi(argv[1]));
	} else {
		usrsctp_init(9899);
	}
	usrsctp_sysctl_set_sctp_debug_on(0);
	usrsctp_sysctl_set_sctp_blackhole(2);

	if ((sock = usrsctp_socket(AF_INET6, SOCK_SEQPACKET, IPPROTO_SCTP, use_cb?receive_cb:NULL, NULL, 0, NULL)) == NULL) {
		perror("userspace_socket");
	}
	if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_I_WANT_MAPPED_V4_ADDR, (const void*)&on, (socklen_t)sizeof(int)) < 0) {
		perror("setsockopt");
	}
	memset(&av, 0, sizeof(struct sctp_assoc_value));
	av.assoc_id = SCTP_ALL_ASSOC;
	av.assoc_value = 47;

	if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_CONTEXT, (const void*)&av, (socklen_t)sizeof(struct sctp_assoc_value)) < 0) {
		perror("setsockopt");
	}
	if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(int)) < 0) {
		perror("setsockopt SCTP_RECVRCVINFO");
	}
	if (argc > 2) {
		memset(&encaps, 0, sizeof(struct sctp_udpencaps));
		encaps.sue_address.ss_family = AF_INET6;
		encaps.sue_port = htons(atoi(argv[2]));
		if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_REMOTE_UDP_ENCAPS_PORT, (const void*)&encaps, (socklen_t)sizeof(struct sctp_udpencaps)) < 0) {
			perror("setsockopt");
		}
	}
	memset(&event, 0, sizeof(event));
	event.se_assoc_id = SCTP_FUTURE_ASSOC;
	event.se_on = 1;
	for (i = 0; i < (unsigned int)(sizeof(event_types)/sizeof(uint16_t)); i++) {
		event.se_type = event_types[i];
		if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(struct sctp_event)) < 0) {
			perror("userspace_setsockopt");
		}
	}
	memset((void *)&addr, 0, sizeof(struct sockaddr_in6));
#ifdef HAVE_SIN_LEN
	addr.sin6_len = sizeof(struct sockaddr_in6);
#endif
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(9);
	addr.sin6_addr = in6addr_any;
	if (usrsctp_bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in6)) < 0) {
		perror("userspace_bind");
	}
	if (usrsctp_listen(sock, 1) < 0) {
		perror("userspace_listen");
	}
	while (1) {
		if (use_cb) {
#if defined (__Userspace_os_Windows)
			Sleep(1*1000);
#else
			sleep(1);
#endif
		} else {
			from_len = (socklen_t)sizeof(struct sockaddr_in6);
			flags = 0;
			infolen = (socklen_t)sizeof(struct sctp_rcvinfo);
			n = usrsctp_recvv(sock, (void*)buffer, BUFFER_SIZE, (struct sockaddr *) &addr, &from_len, (void *)&rcv_info,
			                  &infolen, &infotype, &flags);
			if (n > 0) {
				if (flags & MSG_NOTIFICATION) {
					printf("Notification of length %d received.\n", n);
				} else {
					if (infotype == SCTP_RECVV_RCVINFO) {
						printf("Msg of length %d received from %s:%u on stream %d with SSN %u and TSN %u, PPID %d, context %u, complete %d.\n",
						        n,
						        inet_ntop(AF_INET6, &addr.sin6_addr, name, INET6_ADDRSTRLEN), ntohs(addr.sin6_port),
						        rcv_info.rcv_sid,
						        rcv_info.rcv_ssn,
						        rcv_info.rcv_tsn,
						        ntohl(rcv_info.rcv_ppid),
						        rcv_info.rcv_context,
						        (flags & MSG_EOR) ? 1 : 0);
					} else {
						printf("Msg of length %d received from %s:%u, complete %d.\n",
						        n,
						        inet_ntop(AF_INET6, &addr.sin6_addr, name, INET6_ADDRSTRLEN), ntohs(addr.sin6_port),
						        (flags & MSG_EOR) ? 1 : 0);
					}
				}
			}
		}
	}
	usrsctp_close(sock);
	while (usrsctp_finish() != 0) {
#if defined (__Userspace_os_Windows)
		Sleep(1000);
#else
		sleep(1);
#endif
	}
	return (0);
}
