/*-
 * Copyright (c) 2009-2010 Brad Penoff
 * Copyright (c) 2009-2010 Humaira Kamal
 * Copyright (c) 2011-2012 Irene Ruengeler
 * Copyright (c) 2011-2012 Michael Tuexen
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/types.h>
#if !defined(__Userspace_os_Windows)
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#if !defined(__Userspace_os_FreeBSD)
#include <sys/uio.h>
#else
#include <user_ip6_var.h>
#endif
#endif
#include <netinet/sctp_os.h>
#include <netinet/sctp_var.h>
#include <netinet/sctp_pcb.h>
#if defined(__Userspace_os_Linux)
#include <linux/netlink.h>
#include <linux/if_addr.h>
#endif

/* local macros and datatypes used to get IP addresses system independently */
#if defined IP_RECVDSTADDR
# define DSTADDR_SOCKOPT IP_RECVDSTADDR
# define DSTADDR_DATASIZE (CMSG_SPACE(sizeof(struct in_addr)))
# define dstaddr(x) (CMSG_DATA(x))
#elif defined IP_PKTINFO
# define DSTADDR_SOCKOPT IP_PKTINFO
# define DSTADDR_DATASIZE (CMSG_SPACE(sizeof(struct in_pktinfo)))
# define dstaddr(x) (&(((struct in_pktinfo *)(CMSG_DATA(x)))->ipi_addr))
#else
# error "can't determine socket option to use to get UDP IP"
#endif

void recv_thread_destroy(void);
#define MAXLEN_MBUF_CHAIN 32 /* What should this value be? */
#define ROUNDUP(a, size) (((a) & ((size)-1)) ? (1 + ((a) | ((size)-1))) : (a))
#if !defined(__Userspace_os_Windows)
#define NEXT_SA(ap) ap = (struct sockaddr *) \
	((caddr_t) ap + (ap->sa_len ? ROUNDUP(ap->sa_len, sizeof (uint32_t)) : sizeof(uint32_t)))
#endif

#if !defined(__Userspace_os_Windows) && !defined(__Userspace_os_Linux)
static void
sctp_get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info)
{
	int i;

	for (i = 0; i < RTAX_MAX; i++) {
		if (addrs & (1 << i)) {
			rti_info[i] = sa;
			NEXT_SA(sa);
		} else {
			rti_info[i] = NULL;
		}
	}
}
#endif

static void
sctp_handle_ifamsg(unsigned char type, unsigned short index, struct sockaddr *sa)
{
	int rc;
	struct ifaddrs *ifa, *found_ifa = NULL;

	/* handle only the types we want */
	if ((type != RTM_NEWADDR) && (type != RTM_DELADDR)) {
		return;
	}

	rc = getifaddrs(&g_interfaces);
	if (rc != 0) {
		return;
	}
	for (ifa = g_interfaces; ifa; ifa = ifa->ifa_next) {
		if (index == if_nametoindex(ifa->ifa_name)) {
			found_ifa = ifa;
			break;
		}
	}
	if (found_ifa == NULL) {
		return;
	}

	switch (sa->sa_family) {
#ifdef INET
	case AF_INET:
		ifa->ifa_addr = (struct sockaddr *)malloc(sizeof(struct sockaddr_in));
		memcpy(ifa->ifa_addr, sa, sizeof(struct sockaddr_in));
		break;
#endif
#ifdef INET6
	case AF_INET6:
		ifa->ifa_addr = (struct sockaddr *)malloc(sizeof(struct sockaddr_in6));
		memcpy(ifa->ifa_addr, sa, sizeof(struct sockaddr_in6));
		break;
#endif
	default:
		SCTPDBG(SCTP_DEBUG_USR, "Address family %d not supported.\n", sa->sa_family);
	}

	/* relay the appropriate address change to the base code */
	if (type == RTM_NEWADDR) {
		(void)sctp_add_addr_to_vrf(SCTP_DEFAULT_VRFID, ifa, if_nametoindex(ifa->ifa_name),
		                           0,
		                           ifa->ifa_name,
		                           (void *)ifa,
		                           ifa->ifa_addr,
		                           0,
		                           1);
	} else {
		sctp_del_addr_from_vrf(SCTP_DEFAULT_VRFID, ifa->ifa_addr,
		                       if_nametoindex(ifa->ifa_name),
		                       ifa->ifa_name);
	}
}

#if !defined(__Userspace_os_Windows)
#if !defined(__Userspace_os_Linux)
static void *
recv_function_route(void *arg)
{
	ssize_t ret;
	struct ifa_msghdr *ifa;
	char rt_buffer[1024];
	struct sockaddr *sa, *rti_info[RTAX_MAX];

	while (1) {
		bzero(rt_buffer, sizeof(rt_buffer));
		ret = recv(SCTP_BASE_VAR(userspace_route), rt_buffer, sizeof(rt_buffer), 0);

		if (ret > 0) {
			ifa = (struct ifa_msghdr *) rt_buffer;
			if (ifa->ifam_type != RTM_DELADDR && ifa->ifam_type != RTM_NEWADDR) {
				continue;
			}
			sa = (struct sockaddr *) (ifa + 1);
			sctp_get_rtaddrs(ifa->ifam_addrs, sa, rti_info);
			switch (ifa->ifam_type) {
			case RTM_DELADDR:
			case RTM_NEWADDR:
				sctp_handle_ifamsg(ifa->ifam_type, ifa->ifam_index, rti_info[RTAX_IFA]);
				break;
			default:
				/* ignore this routing event */
				break;
			}
		}
		if (ret < 0) {
			if (errno == EAGAIN) {
				continue;
			} else {
				break;
			}
		}
	}
	pthread_exit(NULL);
}
#else /*Userspace_os_Linux*/
static void *
recv_function_route(void *arg)
{
	int len;
	char buf[4096];
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr msg;
	struct nlmsghdr *nh;
	struct ifaddrmsg *rtmsg;
	struct rtattr *rtatp;
	struct in_addr *inp;
	struct sockaddr_nl sanl;
#ifdef INET
	struct sockaddr_in *sa;
#endif
#ifdef INET6
	struct sockaddr_in6 *sa6;
#endif

	for (;;) {
		memset(&sanl, 0, sizeof(sanl));
		sanl.nl_family = AF_NETLINK;
		sanl.nl_groups = RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_IFADDR;
		memset(&msg, 0, sizeof(struct msghdr));
		msg.msg_name = (void *)&sanl;
		msg.msg_namelen = sizeof(sanl);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;

		len = recvmsg(SCTP_BASE_VAR(userspace_route), &msg, 0);

		if (len < 0) {
			if (errno == EAGAIN) {
				continue;
			} else {
				break;
			}
		}
		for (nh = (struct nlmsghdr *) buf; NLMSG_OK (nh, len);
			nh = NLMSG_NEXT (nh, len)) {
			if (nh->nlmsg_type == NLMSG_DONE)
				break;

			if (nh->nlmsg_type == RTM_NEWADDR || nh->nlmsg_type == RTM_DELADDR) {
				rtmsg = (struct ifaddrmsg *)NLMSG_DATA(nh);
				rtatp = (struct rtattr *)IFA_RTA(rtmsg);
				if(rtatp->rta_type == IFA_ADDRESS) {
					inp = (struct in_addr *)RTA_DATA(rtatp);
					switch (rtmsg->ifa_family) {
#ifdef INET
					case AF_INET:
						sa = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in));
						sa->sin_family = rtmsg->ifa_family;
						sa->sin_port = 0;
						memcpy(&sa->sin_addr, inp, sizeof(struct in_addr));
						sctp_handle_ifamsg(nh->nlmsg_type, rtmsg->ifa_index, (struct sockaddr *)sa);
						break;
#endif
#ifdef INET6
					case AF_INET6:
						sa6 = (struct sockaddr_in6 *)malloc(sizeof(struct sockaddr_in6));
						sa6->sin6_family = rtmsg->ifa_family;
						sa6->sin6_port = 0;
						memcpy(&sa6->sin6_addr, inp, sizeof(struct in6_addr));
						sctp_handle_ifamsg(nh->nlmsg_type, rtmsg->ifa_index, (struct sockaddr *)sa6);
						break;
#endif
					default:
						SCTPDBG(SCTP_DEBUG_USR, "Address family %d not supported.\n", rtmsg->ifa_family);
						break;
					}
				}
			}
		}
	}
	pthread_exit(NULL);
}
#endif
#endif

static void *
recv_function_raw(void *arg)
{
	struct mbuf **recvmbuf;
#if !defined(__Userspace_os_Windows)
	struct iovec recv_iovec[MAXLEN_MBUF_CHAIN];
	int iovcnt = MAXLEN_MBUF_CHAIN;
#else
	WSABUF recv_iovec[MAXLEN_MBUF_CHAIN];
	int nResult, m_ErrorCode;
	DWORD flags;
	struct sockaddr_in from;
	int fromlen;
#endif

	/*Initially the entire set of mbufs is to be allocated.
	  to_fill indicates this amount. */
	int to_fill = MAXLEN_MBUF_CHAIN;
	/* iovlen is the size of each mbuf in the chain */
	int i, n, ncounter = 0;
	int iovlen = MCLBYTES;
	int want_ext = (iovlen > MLEN)? 1 : 0;
	int want_header = 0;

	recvmbuf = malloc(sizeof(struct mbuf *) * MAXLEN_MBUF_CHAIN);

	while (1) {
		for (i = 0; i < to_fill; i++) {
			/* Not getting the packet header. Tests with chain of one run
			   as usual without having the packet header.
			   Have tried both sending and receiving
			 */
			recvmbuf[i] = sctp_get_mbuf_for_msg(iovlen, want_header, M_DONTWAIT, want_ext, MT_DATA);
#if !defined(__Userspace_os_Windows)
			recv_iovec[i].iov_base = (caddr_t)recvmbuf[i]->m_data;
			recv_iovec[i].iov_len = iovlen;
#else
			recv_iovec[i].buf = (caddr_t)recvmbuf[i]->m_data;
			recv_iovec[i].len = iovlen;
#endif
		}
		to_fill = 0;
#if defined(__Userspace_os_Windows)
		flags = 0;
		ncounter = 0;
		fromlen = sizeof(struct sockaddr_in);
		bzero((void *)&from, sizeof(struct sockaddr_in));

		nResult = WSARecvFrom(SCTP_BASE_VAR(userspace_rawsctp), recv_iovec, MAXLEN_MBUF_CHAIN, (LPDWORD)&ncounter, (LPDWORD)&flags, (struct sockaddr*)&from, &fromlen, NULL, NULL);
		if (nResult != 0) {
			m_ErrorCode = WSAGetLastError();
			if (m_ErrorCode == WSAETIMEDOUT) {
				continue;
			}
			if ((m_ErrorCode == WSAENOTSOCK) || (m_ErrorCode == WSAEINTR)) {
				break;
			}
		}
		n = ncounter;
#else
		ncounter = n = readv(SCTP_BASE_VAR(userspace_rawsctp), recv_iovec, iovcnt);
		if (n < 0) {
			if (errno == EAGAIN) {
				continue;
			} else {
				break;
			}
		}
#endif
		SCTP_HEADER_LEN(recvmbuf[0]) = n; /* length of total packet */

		if (n <= iovlen) {
			SCTP_BUF_LEN(recvmbuf[0]) = n;
			(to_fill)++;
		} else {
			i = 0;
			SCTP_BUF_LEN(recvmbuf[0]) = iovlen;

			ncounter -= iovlen;
			(to_fill)++;
			do {
				recvmbuf[i]->m_next = recvmbuf[i+1];
				SCTP_BUF_LEN(recvmbuf[i]->m_next) = min(ncounter, iovlen);
				i++;
				ncounter -= iovlen;
				(to_fill)++;
			} while (ncounter > 0);
		}
		SCTPDBG(SCTP_DEBUG_USR, "%s: Received %d bytes.", __func__, n);
		SCTPDBG(SCTP_DEBUG_USR, " - calling sctp_input with off=%d\n", (int)sizeof(struct ip));

		/* process incoming data */
		/* sctp_input frees this mbuf. */
		sctp_input_with_port(recvmbuf[0], sizeof(struct ip), 0);
	}
	for (i = 0; i < MAXLEN_MBUF_CHAIN; i++) {
		m_free(recvmbuf[i]);
	}
	/* free the array itself */
	free(recvmbuf);
#if defined (__Userspace_os_Windows)
	ExitThread(0);
#else
	pthread_exit(NULL);
#endif
}

#if defined(INET6)
static void *
recv_function_raw6(void *arg)
{
	struct mbuf **recvmbuf6;
#if !defined(__Userspace_os_Windows)
	struct iovec recv_iovec[MAXLEN_MBUF_CHAIN];
	struct msghdr msg;
	struct cmsghdr *cmsgptr;
	char cmsgbuf[CMSG_SPACE(sizeof (struct in6_pktinfo))];
#else
	WSABUF recv_iovec[MAXLEN_MBUF_CHAIN];
	int nResult, m_ErrorCode;
	DWORD flags;
	struct sockaddr_in6 from;
	int fromlen;
	GUID WSARecvMsg_GUID = WSAID_WSARECVMSG;
	LPFN_WSARECVMSG WSARecvMsg;
	WSACMSGHDR *pCMsgHdr;
	WSAMSG win_msg;
	char ControlBuffer[1024];
#endif
	struct mbuf *ip6_m;
	struct ip6_hdr *ip6;
	struct sockaddr_in6 src, dst;
	int offset;

	/*Initially the entire set of mbufs is to be allocated.
	  to_fill indicates this amount. */
	int to_fill = MAXLEN_MBUF_CHAIN;
	/* iovlen is the size of each mbuf in the chain */
	int i, n, ncounter = 0;
	int iovlen = MCLBYTES;
	int want_ext = (iovlen > MLEN)? 1 : 0;
	int want_header = 0;

	recvmbuf6 = malloc(sizeof(struct mbuf *) * MAXLEN_MBUF_CHAIN);

	for (;;) {
		for (i = 0; i < to_fill; i++) {
			/* Not getting the packet header. Tests with chain of one run
			   as usual without having the packet header.
			   Have tried both sending and receiving
			 */
			recvmbuf6[i] = sctp_get_mbuf_for_msg(iovlen, want_header, M_DONTWAIT, want_ext, MT_DATA);
#if !defined(__Userspace_os_Windows)
			recv_iovec[i].iov_base = (caddr_t)recvmbuf6[i]->m_data;
			recv_iovec[i].iov_len = iovlen;
#else
			recv_iovec[i].buf = (caddr_t)recvmbuf6[i]->m_data;
			recv_iovec[i].len = iovlen;
#endif
		}
		to_fill = 0;
#if defined(__Userspace_os_Windows)
		flags = 0;
		ncounter = 0;
		fromlen = sizeof(struct sockaddr_in6);
		bzero((void *)&from, sizeof(struct sockaddr_in6));
		nResult = WSAIoctl(SCTP_BASE_VAR(userspace_rawsctp6), SIO_GET_EXTENSION_FUNCTION_POINTER,
		                   &WSARecvMsg_GUID, sizeof WSARecvMsg_GUID,
		                   &WSARecvMsg, sizeof WSARecvMsg,
		                   &ncounter, NULL, NULL);
		if (nResult == 0) {
			win_msg.name = (void *)&src;
			win_msg.namelen = sizeof(struct sockaddr_in6);
			win_msg.lpBuffers = recv_iovec;
			win_msg.dwBufferCount = MAXLEN_MBUF_CHAIN;
			win_msg.Control.len = sizeof ControlBuffer;
			win_msg.Control.buf = ControlBuffer;
			win_msg.dwFlags = 0;
			nResult = WSARecvMsg(SCTP_BASE_VAR(userspace_rawsctp6), &win_msg, &ncounter, NULL, NULL);
		}
		if (nResult != 0) {
			m_ErrorCode = WSAGetLastError();
			if (m_ErrorCode == WSAETIMEDOUT)
				continue;
			if (m_ErrorCode == WSAENOTSOCK || m_ErrorCode == WSAEINTR)
				break;
		}
		n = ncounter;
#else
		bzero((void *)&msg, sizeof(struct msghdr));
		bzero((void *)&src, sizeof(struct sockaddr_in6));
		bzero((void *)&dst, sizeof(struct sockaddr_in6));
		bzero((void *)cmsgbuf, CMSG_SPACE(sizeof (struct in6_pktinfo)));
		msg.msg_name = (void *)&src;
		msg.msg_namelen = sizeof(struct sockaddr_in6);
		msg.msg_iov = recv_iovec;
		msg.msg_iovlen = MAXLEN_MBUF_CHAIN;
		msg.msg_control = (void *)cmsgbuf;
		msg.msg_controllen = (socklen_t)CMSG_LEN(sizeof (struct in6_pktinfo));
		msg.msg_flags = 0;

		ncounter = n = recvmsg(SCTP_BASE_VAR(userspace_rawsctp6), &msg, 0);
		if (n < 0) {
			if (errno == EAGAIN) {
				continue;
			} else {
				break;
			}
		}
#endif
		SCTP_HEADER_LEN(recvmbuf6[0]) = n; /* length of total packet */

		if (n <= iovlen) {
			SCTP_BUF_LEN(recvmbuf6[0]) = n;
			(to_fill)++;
		} else {
			i = 0;
			SCTP_BUF_LEN(recvmbuf6[0]) = iovlen;

			ncounter -= iovlen;
			(to_fill)++;
			do {
				recvmbuf6[i]->m_next = recvmbuf6[i+1];
				SCTP_BUF_LEN(recvmbuf6[i]->m_next) = min(ncounter, iovlen);
				i++;
				ncounter -= iovlen;
				(to_fill)++;
			} while (ncounter > 0);
		}

#if !defined(__Userspace_os_Windows)
		for (cmsgptr = CMSG_FIRSTHDR(&msg); cmsgptr != NULL; cmsgptr = CMSG_NXTHDR(&msg, cmsgptr)) {
			if ((cmsgptr->cmsg_level == IPPROTO_IPV6) && (cmsgptr->cmsg_type == IPV6_PKTINFO)) {
				memcpy((void *)&dst.sin6_addr, (const void *) (&((struct in6_pktinfo *)CMSG_DATA(cmsgptr))->ipi6_addr), sizeof(struct in6_addr));
			}
		}
#else
		for (pCMsgHdr = WSA_CMSG_FIRSTHDR(&win_msg); pCMsgHdr != NULL; pCMsgHdr = WSA_CMSG_NXTHDR(&win_msg, pCMsgHdr)) {
			if ((pCMsgHdr->cmsg_level == IPPROTO_IPV6) && (pCMsgHdr->cmsg_type == IPV6_PKTINFO)) {
				memcpy((void *)&dst.sin6_addr, (const void *) dstaddr(pCMsgHdr), sizeof(struct in6_addr));
			}
		}
#endif
		ip6_m = sctp_get_mbuf_for_msg(sizeof(struct ip6_hdr), 1, M_DONTWAIT, 1, MT_DATA);
		ip6 = mtod(ip6_m, struct ip6_hdr *);
		bzero((void *)ip6, sizeof(struct ip6_hdr));
		ip6->ip6_vfc = IPV6_VERSION;
		ip6->ip6_plen = htons(n);
		ip6->ip6_src = src.sin6_addr;
		ip6->ip6_dst = dst.sin6_addr;

		SCTP_HEADER_LEN(ip6_m) = (int)sizeof(struct ip6_hdr) + n;
		SCTP_BUF_LEN(ip6_m) = sizeof(struct ip6_hdr);
		SCTP_BUF_NEXT(ip6_m) = recvmbuf6[0];
		/* process incoming data */
		/* sctp_input frees this mbuf. */
		SCTPDBG(SCTP_DEBUG_USR, "%s: Received %d bytes.", __func__, n);
		SCTPDBG(SCTP_DEBUG_USR, " - calling sctp6_input with off=%d\n", (int)sizeof(struct ip6_hdr));

		offset = sizeof(struct ip6_hdr);
		sctp6_input_with_port(&ip6_m, &offset, 0);
	}
	for (i = 0; i < MAXLEN_MBUF_CHAIN; i++) {
		m_free(recvmbuf6[i]);
	}
	/* free the array itself */
	free(recvmbuf6);
#if defined (__Userspace_os_Windows)
	ExitThread(0);
#else
	pthread_exit(NULL);
#endif
}
#endif

static void *
recv_function_udp(void *arg)
{
	struct mbuf **udprecvmbuf;
	/*Initially the entire set of mbufs is to be allocated.
	  to_fill indicates this amount. */
	int to_fill = MAXLEN_MBUF_CHAIN;
	/* iovlen is the size of each mbuf in the chain */
	int i, n, ncounter;
	int iovlen = MCLBYTES;
	int want_ext = (iovlen > MLEN)? 1 : 0;
	int want_header = 0;
	struct ip *ip;
	struct mbuf *ip_m;
	struct sockaddr_in src, dst;
	char cmsgbuf[DSTADDR_DATASIZE];
#if !defined(__Userspace_os_Windows)
	struct iovec iov[MAXLEN_MBUF_CHAIN];
	struct msghdr msg;
	struct cmsghdr *cmsgptr;
#else
	GUID WSARecvMsg_GUID = WSAID_WSARECVMSG;
	LPFN_WSARECVMSG WSARecvMsg;
	char ControlBuffer[1024];
	WSABUF iov[MAXLEN_MBUF_CHAIN];
	WSAMSG win_msg;
	int nResult, m_ErrorCode;
	WSACMSGHDR *pCMsgHdr;
#endif

	udprecvmbuf = malloc(sizeof(struct mbuf *) * MAXLEN_MBUF_CHAIN);

	while (1) {
		for (i = 0; i < to_fill; i++) {
			/* Not getting the packet header. Tests with chain of one run
			   as usual without having the packet header.
			   Have tried both sending and receiving
			 */
			udprecvmbuf[i] = sctp_get_mbuf_for_msg(iovlen, want_header, M_DONTWAIT, want_ext, MT_DATA);
#if !defined(__Userspace_os_Windows)
			iov[i].iov_base = (caddr_t)udprecvmbuf[i]->m_data;
			iov[i].iov_len = iovlen;
#else
			iov[i].buf = (caddr_t)udprecvmbuf[i]->m_data;
			iov[i].len = iovlen;
#endif
		}
		to_fill = 0;
#if !defined(__Userspace_os_Windows)
		bzero((void *)&msg, sizeof(struct msghdr));
#else
		bzero((void *)&win_msg, sizeof(WSAMSG));
#endif
		bzero((void *)&src, sizeof(struct sockaddr_in));
		bzero((void *)&dst, sizeof(struct sockaddr_in));
		bzero((void *)cmsgbuf, DSTADDR_DATASIZE);

#if !defined(__Userspace_os_Windows)
		msg.msg_name = (void *)&src;
		msg.msg_namelen = sizeof(struct sockaddr_in);
		msg.msg_iov = iov;
		msg.msg_iovlen = MAXLEN_MBUF_CHAIN;
		msg.msg_control = (void *)cmsgbuf;
		msg.msg_controllen = DSTADDR_DATASIZE;
		msg.msg_flags = 0;

		ncounter = n = recvmsg(SCTP_BASE_VAR(userspace_udpsctp), &msg, 0);
		if (n < 0) {
			if (errno == EAGAIN) {
				continue;
			} else {
				break;
			}
		}
#else
		nResult = WSAIoctl(SCTP_BASE_VAR(userspace_udpsctp), SIO_GET_EXTENSION_FUNCTION_POINTER,
		 &WSARecvMsg_GUID, sizeof WSARecvMsg_GUID,
		 &WSARecvMsg, sizeof WSARecvMsg,
		 &ncounter, NULL, NULL);
		if (nResult == 0) {
			win_msg.name = (void *)&src;
			win_msg.namelen = sizeof(struct sockaddr_in);
			win_msg.lpBuffers = iov;
			win_msg.dwBufferCount = MAXLEN_MBUF_CHAIN;
			win_msg.Control.len = sizeof ControlBuffer;
			win_msg.Control.buf = ControlBuffer;
			win_msg.dwFlags = 0;
			nResult = WSARecvMsg(SCTP_BASE_VAR(userspace_udpsctp), &win_msg, &ncounter, NULL, NULL);
		}
		if (nResult != 0) {
			m_ErrorCode = WSAGetLastError();
			if (m_ErrorCode == WSAETIMEDOUT) {
				continue;
			}
			if ((m_ErrorCode == WSAENOTSOCK) || (m_ErrorCode == WSAEINTR)) {
				break;
			}
		}
		n = ncounter;
#endif
		SCTP_HEADER_LEN(udprecvmbuf[0]) = n; /* length of total packet */

		if (n <= iovlen) {
			SCTP_BUF_LEN(udprecvmbuf[0]) = n;
			(to_fill)++;
		} else {
			i = 0;
			SCTP_BUF_LEN(udprecvmbuf[0]) = iovlen;

			ncounter -= iovlen;
			(to_fill)++;
			do {
				udprecvmbuf[i]->m_next = udprecvmbuf[i+1];
				SCTP_BUF_LEN(udprecvmbuf[i]->m_next) = min(ncounter, iovlen);
				i++;
				ncounter -= iovlen;
				(to_fill)++;
			} while (ncounter > 0);
		}

#if !defined(__Userspace_os_Windows)
		for (cmsgptr = CMSG_FIRSTHDR(&msg); cmsgptr != NULL; cmsgptr = CMSG_NXTHDR(&msg, cmsgptr)) {
			if ((cmsgptr->cmsg_level == IPPROTO_IP) && (cmsgptr->cmsg_type == DSTADDR_SOCKOPT)) {
				dst.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
				dst.sin_len = sizeof(struct sockaddr_in);
#endif
				dst.sin_port = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
				memcpy((void *)&dst.sin_addr, (const void *) dstaddr(cmsgptr), sizeof(struct in_addr));
			}
		}
#else
		for (pCMsgHdr = WSA_CMSG_FIRSTHDR(&win_msg); pCMsgHdr != NULL; pCMsgHdr = WSA_CMSG_NXTHDR(&win_msg, pCMsgHdr)) {
			if ((pCMsgHdr->cmsg_level == IPPROTO_IP) && (pCMsgHdr->cmsg_type == DSTADDR_SOCKOPT)) {
				dst.sin_family = AF_INET;
				dst.sin_port = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
				memcpy((void *)&dst.sin_addr, (const void *) dstaddr(pCMsgHdr), sizeof(struct in_addr));
			}
		}
#endif

		ip_m = sctp_get_mbuf_for_msg(sizeof(struct ip), 1, M_DONTWAIT, 1, MT_DATA);

		ip = mtod(ip_m, struct ip *);
		bzero((void *)ip, sizeof(struct ip));
		ip->ip_v = IPVERSION;
		ip->ip_len = n;
#if defined(__Userspace_os_Linux) ||  defined(__Userspace_os_Windows)
		ip->ip_len += sizeof(struct ip);
		ip->ip_len = htons(ip->ip_len);
#endif
		ip->ip_src = src.sin_addr;
		ip->ip_dst = dst.sin_addr;
		SCTP_HEADER_LEN(ip_m) = sizeof(struct ip) + n;
		SCTP_BUF_LEN(ip_m) = sizeof(struct ip);
		SCTP_BUF_NEXT(ip_m) = udprecvmbuf[0];

		SCTPDBG(SCTP_DEBUG_USR, "%s: Received %d bytes.", __func__, n);
		SCTPDBG(SCTP_DEBUG_USR, " - calling sctp_input with off=%d\n", (int)sizeof(struct ip));

		/* process incoming data */
		/* sctp_input frees this mbuf. */
		sctp_input_with_port(ip_m, sizeof(struct ip), src.sin_port);
	}
	for (i = 0; i < MAXLEN_MBUF_CHAIN; i++) {
		m_free(udprecvmbuf[i]);
	}
	/* free the array itself */
	free(udprecvmbuf);
#if defined (__Userspace_os_Windows)
	ExitThread(0);
#else
	pthread_exit(NULL);
#endif
}

#if defined(INET6)
static void *
recv_function_udp6(void *arg)
{
	struct mbuf **udprecvmbuf6;
	/*Initially the entire set of mbufs is to be allocated.
	  to_fill indicates this amount. */
	int to_fill = MAXLEN_MBUF_CHAIN;
	/* iovlen is the size of each mbuf in the chain */
	int i, n, ncounter, offset;
	int iovlen = MCLBYTES;
	int want_ext = (iovlen > MLEN)? 1 : 0;
	int want_header = 0;
	struct ip6_hdr *ip6;
	struct mbuf *ip6_m;
	struct sockaddr_in6 src, dst;
	char cmsgbuf[CMSG_SPACE(sizeof (struct in6_pktinfo))];
#if !defined(__Userspace_os_Windows)
	struct iovec iov[MAXLEN_MBUF_CHAIN];
	struct msghdr msg;
	struct cmsghdr *cmsgptr;
#else
	GUID WSARecvMsg_GUID = WSAID_WSARECVMSG;
	LPFN_WSARECVMSG WSARecvMsg;
	char ControlBuffer[1024];
	WSABUF iov[MAXLEN_MBUF_CHAIN];
	WSAMSG win_msg;
	int nResult, m_ErrorCode;
	WSACMSGHDR *pCMsgHdr;
#endif

	udprecvmbuf6 = malloc(sizeof(struct mbuf *) * MAXLEN_MBUF_CHAIN);

	for (;;) {
		for (i = 0; i < to_fill; i++) {
			/* Not getting the packet header. Tests with chain of one run
			   as usual without having the packet header.
			   Have tried both sending and receiving
			 */
			udprecvmbuf6[i] = sctp_get_mbuf_for_msg(iovlen, want_header, M_DONTWAIT, want_ext, MT_DATA);
#if !defined(__Userspace_os_Windows)
			iov[i].iov_base = (caddr_t)udprecvmbuf6[i]->m_data;
			iov[i].iov_len = iovlen;
#else
			iov[i].buf = (caddr_t)udprecvmbuf6[i]->m_data;
			iov[i].len = iovlen;
#endif
		}
		to_fill = 0;

#if !defined(__Userspace_os_Windows)
		bzero((void *)&msg, sizeof(struct msghdr));
#else
		bzero((void *)&win_msg, sizeof(WSAMSG));
#endif
		bzero((void *)&src, sizeof(struct sockaddr_in6));
		bzero((void *)&dst, sizeof(struct sockaddr_in6));
		bzero((void *)cmsgbuf, CMSG_SPACE(sizeof (struct in6_pktinfo)));

#if !defined(__Userspace_os_Windows)
		msg.msg_name = (void *)&src;
		msg.msg_namelen = sizeof(struct sockaddr_in6);
		msg.msg_iov = iov;
		msg.msg_iovlen = MAXLEN_MBUF_CHAIN;
		msg.msg_control = (void *)cmsgbuf;
		msg.msg_controllen = (socklen_t)CMSG_LEN(sizeof (struct in6_pktinfo));
		msg.msg_flags = 0;

		ncounter = n = recvmsg(SCTP_BASE_VAR(userspace_udpsctp6), &msg, 0);
		if (n < 0) {
			if (errno == EAGAIN) {
				continue;
			} else {
				break;
			}
		}
#else
		nResult = WSAIoctl(SCTP_BASE_VAR(userspace_udpsctp6), SIO_GET_EXTENSION_FUNCTION_POINTER,
		                   &WSARecvMsg_GUID, sizeof WSARecvMsg_GUID,
		                   &WSARecvMsg, sizeof WSARecvMsg,
		                   &ncounter, NULL, NULL);
		if (nResult == 0) {
			win_msg.name = (void *)&src;
			win_msg.namelen = sizeof(struct sockaddr_in6);
			win_msg.lpBuffers = iov;
			win_msg.dwBufferCount = MAXLEN_MBUF_CHAIN;
			win_msg.Control.len = sizeof ControlBuffer;
			win_msg.Control.buf = ControlBuffer;
			win_msg.dwFlags = 0;
			nResult = WSARecvMsg(SCTP_BASE_VAR(userspace_udpsctp6), &win_msg, &ncounter, NULL, NULL);
		}
		if (nResult != 0) {
			m_ErrorCode = WSAGetLastError();
			if (m_ErrorCode == WSAETIMEDOUT) {
				continue;
			}
			if ((m_ErrorCode == WSAENOTSOCK) || (m_ErrorCode == WSAEINTR)) {
				break;
			}
		}
		n = ncounter;
#endif
		SCTP_HEADER_LEN(udprecvmbuf6[0]) = n; /* length of total packet */

		if (n <= iovlen) {
			SCTP_BUF_LEN(udprecvmbuf6[0]) = n;
			(to_fill)++;
		} else {
			i = 0;
			SCTP_BUF_LEN(udprecvmbuf6[0]) = iovlen;

			ncounter -= iovlen;
			(to_fill)++;
			do {
				udprecvmbuf6[i]->m_next = udprecvmbuf6[i+1];
				SCTP_BUF_LEN(udprecvmbuf6[i]->m_next) = min(ncounter, iovlen);
				i++;
				ncounter -= iovlen;
				(to_fill)++;
			} while (ncounter > 0);
		}

#if !defined(__Userspace_os_Windows)
		for (cmsgptr = CMSG_FIRSTHDR(&msg); cmsgptr != NULL; cmsgptr = CMSG_NXTHDR(&msg, cmsgptr)) {
			if ((cmsgptr->cmsg_level == IPPROTO_IPV6) && (cmsgptr->cmsg_type == IPV6_PKTINFO)) {
				dst.sin6_family = AF_INET6;
#if !defined(__Userspace_os_Linux)
				dst.sin6_len = sizeof(struct sockaddr_in6);
#endif
				dst.sin6_port = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
				memcpy((void *)&dst.sin6_addr, (const void *) (&((struct in6_pktinfo *)CMSG_DATA(cmsgptr))->ipi6_addr), sizeof(struct in6_addr));
			}
		}
#else
    for (pCMsgHdr = WSA_CMSG_FIRSTHDR(&win_msg); pCMsgHdr != NULL; pCMsgHdr = WSA_CMSG_NXTHDR(&win_msg, pCMsgHdr)) {
			if ((pCMsgHdr->cmsg_level == IPPROTO_IPV6) && (pCMsgHdr->cmsg_type == IPV6_PKTINFO)) {
				dst.sin6_family = AF_INET6;
				dst.sin6_port = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
				memcpy((void *)&dst.sin6_addr, (const void *) dstaddr(pCMsgHdr), sizeof(struct in6_addr));
			}
		}
#endif

		ip6_m = sctp_get_mbuf_for_msg(sizeof(struct ip6_hdr), 1, M_DONTWAIT, 1, MT_DATA);

		ip6 = mtod(ip6_m, struct ip6_hdr *);
		bzero((void *)ip6, sizeof(struct ip6_hdr));
		ip6->ip6_vfc = IPV6_VERSION;
		ip6->ip6_plen = htons(n);
		ip6->ip6_src = src.sin6_addr;
		ip6->ip6_dst = dst.sin6_addr;
		SCTP_HEADER_LEN(ip6_m) = (int)sizeof(struct ip6_hdr) + n;
		SCTP_BUF_LEN(ip6_m) = sizeof(struct ip6_hdr);
		SCTP_BUF_NEXT(ip6_m) = udprecvmbuf6[0];

		SCTPDBG(SCTP_DEBUG_USR, "%s: Received %d bytes.", __func__, n);
		SCTPDBG(SCTP_DEBUG_USR, " - calling sctp_input with off=%d\n", (int)sizeof(struct ip));

		/* process incoming data */
		/* sctp_input frees this mbuf. */
		offset = sizeof(struct ip6_hdr);
		sctp6_input_with_port(&ip6_m, &offset, src.sin6_port);
	}
	for (i = 0; i < MAXLEN_MBUF_CHAIN; i++) {
		m_free(udprecvmbuf6[i]);
	}
	/* free the array itself */
	free(udprecvmbuf6);
#if defined (__Userspace_os_Windows)
	ExitThread(0);
#else
	pthread_exit(NULL);
#endif
}
#endif

static void
setReceiveBufferSize(int sfd, int new_size)
{
	int ch = new_size;

	if (setsockopt (sfd, SOL_SOCKET, SO_RCVBUF, (void*)&ch, sizeof(ch)) < 0) {
		SCTPDBG(SCTP_DEBUG_USR, "Can't set recv-buffers size (errno = %d).\n", errno);
	}
	return;
}

static void
setSendBufferSize(int sfd, int new_size)
{
	int ch = new_size;

	if (setsockopt (sfd, SOL_SOCKET, SO_SNDBUF, (void*)&ch, sizeof(ch)) < 0) {
		SCTPDBG(SCTP_DEBUG_USR, "Can't set recv-buffers size (errno = %d).\n", errno);
	}
	return;
}

#define SOCKET_TIMEOUT 100 /* in ms */
void
recv_thread_init(void)
{
#if defined(INET)
	struct sockaddr_in addr_ipv4;
	const int hdrincl = 1;
#endif
#if defined(INET6)
	struct sockaddr_in6 addr_ipv6;
#endif
#if defined(INET) || defined(INET6)
	const int on = 1;
#endif
#if !defined(__Userspace_os_Windows)
	struct timeval timeout;

	timeout.tv_sec  = (SOCKET_TIMEOUT / 1000);
	timeout.tv_usec = (SOCKET_TIMEOUT % 1000) * 1000;
#else
	unsigned int timeout = SOCKET_TIMEOUT; /* Timeout in milliseconds */
#endif
#if !defined(__Userspace_os_Windows)
	if (SCTP_BASE_VAR(userspace_route) == -1) {
#if !defined(__Userspace_os_Linux)
		if ((SCTP_BASE_VAR(userspace_route) = socket(AF_ROUTE, SOCK_RAW, 0)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't create routing socket (errno = %d).\n", errno);
		}
#else
		struct sockaddr_nl sanl;

		if ((SCTP_BASE_VAR(userspace_route) = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't create routing socket (errno = %d.\n", errno);
		}
		memset(&sanl, 0, sizeof(sanl));
		sanl.nl_family = AF_NETLINK;
		sanl.nl_groups = 0;
#ifdef INET
		sanl.nl_groups |= RTMGRP_IPV4_IFADDR;
#endif
#ifdef INET6
		sanl.nl_groups |= RTMGRP_IPV6_IFADDR;
#endif
		if (bind(SCTP_BASE_VAR(userspace_route), (struct sockaddr *) &sanl, sizeof(sanl)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't bind routing socket (errno = %d).\n", errno);
			close(SCTP_BASE_VAR(userspace_route));
			SCTP_BASE_VAR(userspace_route) = -1;
		}
#endif
		if (SCTP_BASE_VAR(userspace_route) != -1) {
			if (setsockopt(SCTP_BASE_VAR(userspace_route), SOL_SOCKET, SO_RCVTIMEO,(const void*)&timeout, sizeof(struct timeval)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set timeout on routing socket (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
				closesocket(SCTP_BASE_VAR(userspace_route));
#else
				close(SCTP_BASE_VAR(userspace_route));
#endif
				SCTP_BASE_VAR(userspace_route) = -1;
			}
		}
	}
#endif
#if defined(INET)
	if (SCTP_BASE_VAR(userspace_rawsctp) == -1) {
		if ((SCTP_BASE_VAR(userspace_rawsctp) = socket(AF_INET, SOCK_RAW, IPPROTO_SCTP)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't create raw socket for IPv4 (errno = %d).\n", errno);
		} else {
			/* complete setting up the raw SCTP socket */
			if (setsockopt(SCTP_BASE_VAR(userspace_rawsctp), IPPROTO_IP, IP_HDRINCL,(const void*)&hdrincl, sizeof(int)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set IP_HDRINCL (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
				closesocket(SCTP_BASE_VAR(userspace_rawsctp));
#else
				close(SCTP_BASE_VAR(userspace_rawsctp));
#endif
				SCTP_BASE_VAR(userspace_rawsctp) = -1;
			} else if (setsockopt(SCTP_BASE_VAR(userspace_rawsctp), SOL_SOCKET, SO_RCVTIMEO, (const void *)&timeout, sizeof(timeout)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set timeout on socket for SCTP/IPv4 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
				closesocket(SCTP_BASE_VAR(userspace_rawsctp));
#else
				close(SCTP_BASE_VAR(userspace_rawsctp));
#endif
				SCTP_BASE_VAR(userspace_rawsctp) = -1;
			} else {
				memset((void *)&addr_ipv4, 0, sizeof(struct sockaddr_in));
#ifdef HAVE_SIN_LEN
				addr_ipv4.sin_len         = sizeof(struct sockaddr_in);
#endif
				addr_ipv4.sin_family      = AF_INET;
				addr_ipv4.sin_port        = htons(0);
				addr_ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
				if (bind(SCTP_BASE_VAR(userspace_rawsctp), (const struct sockaddr *)&addr_ipv4, sizeof(struct sockaddr_in)) < 0) {
					SCTPDBG(SCTP_DEBUG_USR, "Can't bind socket for SCTP/IPv4 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
					closesocket(SCTP_BASE_VAR(userspace_rawsctp));
#else
					close(SCTP_BASE_VAR(userspace_rawsctp));
#endif
					SCTP_BASE_VAR(userspace_rawsctp) = -1;
				} else {
					setReceiveBufferSize(SCTP_BASE_VAR(userspace_rawsctp), SB_RAW); /* 128K */
					setSendBufferSize(SCTP_BASE_VAR(userspace_rawsctp), SB_RAW); /* 128K Is this setting net.inet.raw.maxdgram value? Should it be set to 64K? */
				}
			}
		}
	}
	if (SCTP_BASE_VAR(userspace_udpsctp) == -1) {
		if ((SCTP_BASE_VAR(userspace_udpsctp) = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't create socket for SCTP/UDP/IPv4 (errno = %d).\n", errno);
		} else {
			if (setsockopt(SCTP_BASE_VAR(userspace_udpsctp), IPPROTO_IP, DSTADDR_SOCKOPT, (const void *)&on, (int)sizeof(int)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set DSTADDR_SOCKOPT on socket for SCTP/UDP/IPv4 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
				closesocket(SCTP_BASE_VAR(userspace_udpsctp));
#else
				close(SCTP_BASE_VAR(userspace_udpsctp));
#endif
				SCTP_BASE_VAR(userspace_udpsctp) = -1;
			} else if (setsockopt(SCTP_BASE_VAR(userspace_udpsctp), SOL_SOCKET, SO_RCVTIMEO, (const void *)&timeout, sizeof(timeout)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set timeout on socket for SCTP/UDP/IPv4 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
				closesocket(SCTP_BASE_VAR(userspace_udpsctp));
#else
				close(SCTP_BASE_VAR(userspace_udpsctp));
#endif
				SCTP_BASE_VAR(userspace_udpsctp) = -1;
			} else {
				memset((void *)&addr_ipv4, 0, sizeof(struct sockaddr_in));
#ifdef HAVE_SIN_LEN
				addr_ipv4.sin_len         = sizeof(struct sockaddr_in);
#endif
				addr_ipv4.sin_family      = AF_INET;
				addr_ipv4.sin_port        = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
				addr_ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
				if (bind(SCTP_BASE_VAR(userspace_udpsctp), (const struct sockaddr *)&addr_ipv4, sizeof(struct sockaddr_in)) < 0) {
					SCTPDBG(SCTP_DEBUG_USR, "Can't bind socket for SCTP/UDP/IPv4 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
					closesocket(SCTP_BASE_VAR(userspace_udpsctp));
#else
					close(SCTP_BASE_VAR(userspace_udpsctp));
#endif
					SCTP_BASE_VAR(userspace_udpsctp) = -1;
				} else {
					setReceiveBufferSize(SCTP_BASE_VAR(userspace_udpsctp), SB_RAW); /* 128K */
					setSendBufferSize(SCTP_BASE_VAR(userspace_udpsctp), SB_RAW); /* 128K Is this setting net.inet.raw.maxdgram value? Should it be set to 64K? */
				}
			}
		}
	}
#endif
#if defined(INET6)
	if (SCTP_BASE_VAR(userspace_rawsctp6) == -1) {
		if ((SCTP_BASE_VAR(userspace_rawsctp6) = socket(AF_INET6, SOCK_RAW, IPPROTO_SCTP)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't create socket for SCTP/IPv6 (errno = %d).\n", errno);
		} else {
			/* complete setting up the raw SCTP socket */
#if defined(IPV6_RECVPKTINFO)
			if (setsockopt(SCTP_BASE_VAR(userspace_rawsctp6), IPPROTO_IPV6, IPV6_RECVPKTINFO, (const void *)&on, (int)sizeof(int)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set IPV6_RECVPKTINFO on socket for SCTP/IPv6 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
				closesocket(SCTP_BASE_VAR(userspace_rawsctp6));
#else
				close(SCTP_BASE_VAR(userspace_rawsctp6));
#endif
				SCTP_BASE_VAR(userspace_rawsctp6) = -1;
			} else {
#else
			if (setsockopt(SCTP_BASE_VAR(userspace_rawsctp6), IPPROTO_IPV6, IPV6_PKTINFO,(const void*)&on, sizeof(on)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set IPV6_PKTINFO on socket for SCTP/IPv6 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
				closesocket(SCTP_BASE_VAR(userspace_rawsctp6));
#else
				close(SCTP_BASE_VAR(userspace_rawsctp6));
#endif
				SCTP_BASE_VAR(userspace_rawsctp6) = -1;
			} else {
#endif
				if (setsockopt(SCTP_BASE_VAR(userspace_rawsctp6), IPPROTO_IPV6, IPV6_V6ONLY, (const void*)&on, (socklen_t)sizeof(on)) < 0) {
					SCTPDBG(SCTP_DEBUG_USR, "Can't set IPV6_V6ONLY on socket for SCTP/IPv6 (errno = %d).\n", errno);
				}
				if (setsockopt(SCTP_BASE_VAR(userspace_rawsctp6), SOL_SOCKET, SO_RCVTIMEO, (const void *)&timeout, sizeof(timeout)) < 0) {
					SCTPDBG(SCTP_DEBUG_USR, "Can't set timeout on socket for SCTP/IPv6 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
					closesocket(SCTP_BASE_VAR(userspace_rawsctp6));
#else
					close(SCTP_BASE_VAR(userspace_rawsctp6));
#endif
					SCTP_BASE_VAR(userspace_rawsctp6) = -1;
				} else {
					memset((void *)&addr_ipv6, 0, sizeof(struct sockaddr_in6));
#if !defined(__Userspace_os_Linux) && !defined(__Userspace_os_Windows)
					addr_ipv6.sin6_len         = sizeof(struct sockaddr_in6);
#endif
					addr_ipv6.sin6_family      = AF_INET6;
					addr_ipv6.sin6_port        = htons(0);
					addr_ipv6.sin6_addr        = in6addr_any;
					if (bind(SCTP_BASE_VAR(userspace_rawsctp6), (const struct sockaddr *)&addr_ipv6, sizeof(struct sockaddr_in6)) < 0) {
						SCTPDBG(SCTP_DEBUG_USR, "Can't bind socket for SCTP/IPv6 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
						closesocket(SCTP_BASE_VAR(userspace_rawsctp6));
#else
						close(SCTP_BASE_VAR(userspace_rawsctp6));
#endif
						SCTP_BASE_VAR(userspace_rawsctp6) = -1;
					} else {
						setReceiveBufferSize(SCTP_BASE_VAR(userspace_rawsctp6), SB_RAW); /* 128K */
						setSendBufferSize(SCTP_BASE_VAR(userspace_rawsctp6), SB_RAW); /* 128K Is this setting net.inet.raw.maxdgram value? Should it be set to 64K? */
					}
				}
			}
		}
	}
	if (SCTP_BASE_VAR(userspace_udpsctp6) == -1) {
		if ((SCTP_BASE_VAR(userspace_udpsctp6) = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't create socket for SCTP/UDP/IPv6 (errno = %d).\n", errno);
		}
#if defined(IPV6_RECVPKTINFO)
		if (setsockopt(SCTP_BASE_VAR(userspace_udpsctp6), IPPROTO_IPV6, IPV6_RECVPKTINFO, (const void *)&on, (int)sizeof(int)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't set IPV6_RECVPKTINFO on socket for SCTP/UDP/IPv6 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
			closesocket(SCTP_BASE_VAR(userspace_udpsctp6));
#else
			close(SCTP_BASE_VAR(userspace_udpsctp6));
#endif
			SCTP_BASE_VAR(userspace_udpsctp6) = -1;
		} else {
#else
		if (setsockopt(SCTP_BASE_VAR(userspace_udpsctp6), IPPROTO_IPV6, IPV6_PKTINFO, (const void *)&on, (int)sizeof(int)) < 0) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't set IPV6_PKTINFO on socket for SCTP/UDP/IPv6 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
			closesocket(SCTP_BASE_VAR(userspace_udpsctp6));
#else
			close(SCTP_BASE_VAR(userspace_udpsctp6));
#endif
			SCTP_BASE_VAR(userspace_udpsctp6) = -1;
		} else {
#endif
			if (setsockopt(SCTP_BASE_VAR(userspace_udpsctp6), IPPROTO_IPV6, IPV6_V6ONLY, (const void *)&on, (socklen_t)sizeof(on)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set IPV6_V6ONLY on socket for SCTP/UDP/IPv6 (errno = %d).\n", errno);
			}
			if (setsockopt(SCTP_BASE_VAR(userspace_udpsctp6), SOL_SOCKET, SO_RCVTIMEO, (const void *)&timeout, sizeof(timeout)) < 0) {
				SCTPDBG(SCTP_DEBUG_USR, "Can't set timeout on socket for SCTP/UDP/IPv6 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
				closesocket(SCTP_BASE_VAR(userspace_udpsctp6));
#else
				close(SCTP_BASE_VAR(userspace_udpsctp6));
#endif
				SCTP_BASE_VAR(userspace_udpsctp6) = -1;
			} else {
				memset((void *)&addr_ipv6, 0, sizeof(struct sockaddr_in6));
#if !defined(__Userspace_os_Linux) && !defined(__Userspace_os_Windows)
				addr_ipv6.sin6_len         = sizeof(struct sockaddr_in6);
#endif
				addr_ipv6.sin6_family      = AF_INET6;
				addr_ipv6.sin6_port        = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
				addr_ipv6.sin6_addr        = in6addr_any;
				if (bind(SCTP_BASE_VAR(userspace_udpsctp6), (const struct sockaddr *)&addr_ipv6, sizeof(struct sockaddr_in6)) < 0) {
					SCTPDBG(SCTP_DEBUG_USR, "Can't bind socket for SCTP/UDP/IPv6 (errno = %d).\n", errno);
#if defined(__Userspace_os_Windows)
					closesocket(SCTP_BASE_VAR(userspace_udpsctp6));
#else
					close(SCTP_BASE_VAR(userspace_udpsctp6));
#endif
					SCTP_BASE_VAR(userspace_udpsctp6) = -1;
				} else {
					setReceiveBufferSize(SCTP_BASE_VAR(userspace_udpsctp6), SB_RAW); /* 128K */
					setSendBufferSize(SCTP_BASE_VAR(userspace_udpsctp6), SB_RAW); /* 128K Is this setting net.inet.raw.maxdgram value? Should it be set to 64K? */
				}
			}
		}
	}
#endif
#if !defined(__Userspace_os_Windows)
#if defined(INET) || defined(INET6)
	if (SCTP_BASE_VAR(userspace_route) != -1) {
		int rc;

		if ((rc = pthread_create(&SCTP_BASE_VAR(recvthreadroute), NULL, &recv_function_route, NULL))) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start routing thread (%d).\n", rc);
			close(SCTP_BASE_VAR(userspace_route));
			SCTP_BASE_VAR(userspace_route) = -1;
		}
	}
#endif
#if defined(INET)
	if (SCTP_BASE_VAR(userspace_rawsctp) != -1) {
		int rc;

		if ((rc = pthread_create(&SCTP_BASE_VAR(recvthreadraw), NULL, &recv_function_raw, NULL))) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start SCTP/IPv4 recv thread (%d).\n", rc);
			close(SCTP_BASE_VAR(userspace_rawsctp));
			SCTP_BASE_VAR(userspace_rawsctp) = -1;
		}
	}
	if (SCTP_BASE_VAR(userspace_udpsctp) != -1) {
		int rc;

		if ((rc = pthread_create(&SCTP_BASE_VAR(recvthreadudp), NULL, &recv_function_udp, NULL))) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start SCTP/UDP/IPv4 recv thread (%d).\n", rc);
			close(SCTP_BASE_VAR(userspace_udpsctp));
			SCTP_BASE_VAR(userspace_udpsctp) = -1;
		}
	}
#endif
#if defined(INET6)
	if (SCTP_BASE_VAR(userspace_rawsctp6) != -1) {
		int rc;

		if ((rc = pthread_create(&SCTP_BASE_VAR(recvthreadraw6), NULL, &recv_function_raw6, NULL))) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start SCTP/IPv6 recv thread (%d).\n", rc);
			close(SCTP_BASE_VAR(userspace_rawsctp6));
			SCTP_BASE_VAR(userspace_rawsctp6) = -1;
		}
	}
	if (SCTP_BASE_VAR(userspace_udpsctp6) != -1) {
		int rc;

		if ((rc = pthread_create(&SCTP_BASE_VAR(recvthreadudp6), NULL, &recv_function_udp6, NULL))) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start SCTP/UDP/IPv6 recv thread (%d).\n", rc);
			close(SCTP_BASE_VAR(userspace_udpsctp6));
			SCTP_BASE_VAR(userspace_udpsctp6) = -1;
		}
	}
#endif
#else
#if defined(INET)
	if (SCTP_BASE_VAR(userspace_rawsctp) != -1) {
		if ((SCTP_BASE_VAR(recvthreadraw) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&recv_function_raw, NULL, 0, NULL)) == NULL) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start SCTP/IPv4 recv thread.\n");
			closesocket(SCTP_BASE_VAR(userspace_rawsctp));
			SCTP_BASE_VAR(userspace_rawsctp) = -1;
		}
	}
	if (SCTP_BASE_VAR(userspace_udpsctp) != -1) {
		if ((SCTP_BASE_VAR(recvthreadudp) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&recv_function_udp, NULL, 0, NULL)) == NULL) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start SCTP/UDP/IPv4 recv thread.\n");
			closesocket(SCTP_BASE_VAR(userspace_udpsctp));
			SCTP_BASE_VAR(userspace_udpsctp) = -1;
		}
	}
#endif
#if defined(INET6)
	if (SCTP_BASE_VAR(userspace_rawsctp6) != -1) {
		if ((SCTP_BASE_VAR(recvthreadraw6) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&recv_function_raw6, NULL, 0, NULL)) == NULL) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start SCTP/IPv6 recv thread.\n");
			closesocket(SCTP_BASE_VAR(userspace_rawsctp6));
			SCTP_BASE_VAR(userspace_rawsctp6) = -1;
		}
	}
	if (SCTP_BASE_VAR(userspace_udpsctp6) != -1) {
		if ((SCTP_BASE_VAR(recvthreadudp6) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&recv_function_udp6, NULL, 0, NULL)) == NULL) {
			SCTPDBG(SCTP_DEBUG_USR, "Can't start SCTP/UDP/IPv6 recv thread.\n");
			closesocket(SCTP_BASE_VAR(userspace_udpsctp6));
			SCTP_BASE_VAR(userspace_udpsctp6) = -1;
		}
	}
#endif
#endif
}

void
recv_thread_destroy(void)
{
#if !defined(__Userspace_os_Windows)
#if defined(INET) || defined(INET6)
	if (SCTP_BASE_VAR(userspace_route) != -1) {
		close(SCTP_BASE_VAR(userspace_route));
	}
#endif
#endif
#if defined(INET)
	if (SCTP_BASE_VAR(userspace_rawsctp) != -1) {
#if defined(__Userspace_os_Windows)
		closesocket(SCTP_BASE_VAR(userspace_rawsctp));
#else
		close(SCTP_BASE_VAR(userspace_rawsctp));
#endif
	}
	if (SCTP_BASE_VAR(userspace_udpsctp) != -1) {
#if defined(__Userspace_os_Windows)
		closesocket(SCTP_BASE_VAR(userspace_udpsctp));
#else
		close(SCTP_BASE_VAR(userspace_udpsctp));
#endif
	}
#endif
#if defined(INET6)
	if (SCTP_BASE_VAR(userspace_rawsctp6) != -1) {
#if defined(__Userspace_os_Windows)
		closesocket(SCTP_BASE_VAR(userspace_rawsctp6));
#else
		close(SCTP_BASE_VAR(userspace_rawsctp6));
#endif
	}
	if (SCTP_BASE_VAR(userspace_udpsctp6) != -1) {
#if defined(__Userspace_os_Windows)
		closesocket(SCTP_BASE_VAR(userspace_udpsctp6));
#else
		close(SCTP_BASE_VAR(userspace_udpsctp6));
#endif
	}
#endif
}
