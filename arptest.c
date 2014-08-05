/*
 * Copyright (C) 2014 Robin McCorkell <rmccorkell@karoshi.org.uk>
 *
 * This file is part of arptest
 *
 * arptest is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * arptest is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with arptest.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <netinet/if_ether.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>

#include "find_device.h"

#define ERR_SUCCESS 0
#define ERR_FAIL 1
#define ERR_ARGS 2
#define ERR_SYS 3

void usage(const char *prog) {
	fprintf(stderr,
		"Usage:\n"
		"  %s [options] iface ipaddr\n"
		"\n"
		"Options:\n"
		"  -w timeout: set timeout in seconds\n"
		, prog);
}

void null_sighandler(int sig) {
}

int check_reply(const struct ether_arp *req, const struct ether_arp *reply) {
	if (reply->arp_hrd != htons(ARPHRD_ETHER)
	 || reply->arp_pro != htons(ETH_P_IP)
	 || reply->arp_hln != ETHER_ADDR_LEN
	 || reply->arp_pln != sizeof(in_addr_t)
	 || reply->arp_op != htons(ARPOP_REPLY)
	) {
		return ERR_FAIL;
	}
	if (strncmp(reply->arp_spa, req->arp_tpa, sizeof(req->arp_tpa)) == 0
	 && strncmp(reply->arp_tha, req->arp_sha, sizeof(req->arp_sha)) == 0
	 && strncmp(reply->arp_tpa, req->arp_spa, sizeof(req->arp_spa)) == 0
	) {
		return ERR_SUCCESS;
	} else {
		return ERR_FAIL;
	}
}

int main(int argc, char **argv) {
	struct device iface;
	struct in_addr ipaddr;
	unsigned int timeout = 1;

	int ch;
	while ((ch = getopt(argc, argv, "w:")) != -1) {
		switch (ch) {
		case 'w':
			timeout = atoi(optarg);
			if (timeout == 0) {
				fprintf(stderr, "Invalid timeout '%s'\n", optarg);
				return ERR_ARGS;
			}
			break;
		default:
			usage(argv[0]);
			return ERR_ARGS;
		}
	}

	switch (argc-optind) {
	case 2:
		if (inet_aton(argv[optind+1], &ipaddr) != 1) {
			fprintf(stderr, "Invalid IP address %s\n", argv[optind+1]);
			usage(argv[0]);
			return ERR_ARGS;
		}
		iface.name = argv[optind];
		break;
	default:
		usage(argv[0]);
		return ERR_ARGS;
	}

	if (find_device(&iface) != 0) {
		fprintf(stderr, "Invalid interface %s\n", iface.name);
		return ERR_SYS;
	}

	/* prepare socket */
	int sock = socket(AF_PACKET, SOCK_DGRAM, 0);
	if (sock == -1) {
		perror("socket");
		fprintf(stderr, "Check this program has sufficient privileges\n");
		return ERR_SYS;
	}

	struct sockaddr_ll addr = {0};
	addr.sll_family = AF_PACKET;
	addr.sll_ifindex = iface.ifindex;
	addr.sll_protocol = htons(ETH_P_ARP);

	if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		perror("bind");
		return ERR_SYS;
	}

	struct sockaddr_ll me = addr;
	socklen_t me_len = sizeof(me);
	if (getsockname(sock, (struct sockaddr*)&me, &me_len) == -1) {
		perror("getsockname");
		return ERR_SYS;
	}
	if (me.sll_halen == 0) {
		fprintf(stderr, "Interface %s has no ll address\n", iface.name);
		return ERR_SYS;
	}

	addr.sll_halen = ETHER_ADDR_LEN;
	memset(addr.sll_addr, -1, ETHER_ADDR_LEN);

	/* construct arp request */
	struct ether_arp req;
	req.arp_hrd = htons(ARPHRD_ETHER);
	req.arp_pro = htons(ETH_P_IP);
	req.arp_hln = ETHER_ADDR_LEN;
	req.arp_pln = sizeof(in_addr_t);
	req.arp_op = htons(ARPOP_REQUEST);

	memcpy(req.arp_sha, me.sll_addr, sizeof(req.arp_sha));
	memset(req.arp_spa, 0, sizeof(req.arp_spa));
	memset(req.arp_tha, 0, sizeof(req.arp_tha));
	memcpy(req.arp_tpa, &ipaddr.s_addr, sizeof(req.arp_tpa));

	/* send and receive */
	if (sendto(sock, &req, sizeof(req), 0, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		perror("sendto");
		return ERR_SYS;
	}

	struct sigaction act;
	act.sa_handler = null_sighandler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (sigaction(SIGALRM, &act, NULL) < 0) {
		perror("sigaction");
		return ERR_SYS;
	}
	alarm(timeout);

	struct ether_arp response;

	do {
		int recv_ret = recvfrom(sock, &response, sizeof(response), 0, NULL, NULL);
		if (recv_ret <= 0) {
			if (errno == EINTR) {
				/* no reply, no such host */
				return ERR_FAIL;
			} else {
				perror("recvfrom");
				return ERR_SYS;
			}
		}
	} while (check_reply(&req, &response));

	/* success! */
	printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
		response.arp_sha[0],
		response.arp_sha[1],
		response.arp_sha[2],
		response.arp_sha[3],
		response.arp_sha[4],
		response.arp_sha[5]);

	return ERR_SUCCESS;
}