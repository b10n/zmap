/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

// probe module for performing TCP SYN OPT scans over IPv6
// based on TCP SYN module, with changes by Quirin Scheitle and Markus Sosnowski

// Needed for asprintf
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif


#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "../../lib/includes.h"
#include "../fieldset.h"
#include "probe_modules.h"
#include "packet.h"
#include "../../lib/xalloc.h"
#include "logger.h"

#include "module_tcp_synopt.h"

probe_module_t module_ipv6_tcp_synopt;
static uint32_t num_ports;

#define MAX_OPT_LEN 40

static char *tcp_send_opts = NULL;
static int tcp_send_opts_len = 0;


int ipv6_tcp_synopt_global_initialize(struct state_conf *conf)
{
	// code partly copied from UDP module
	char *args, *c;
	int i;
	unsigned int n;

	num_ports = conf->source_port_last - conf->source_port_first + 1;

	if (!(conf->probe_args && strlen(conf->probe_args) > 0)){
		printf("no args, using empty tcp options\n");
		module_ipv6_tcp_synopt.packet_length = sizeof(struct ether_header) + sizeof(struct ip6_hdr)
				+ sizeof(struct tcphdr);
		return(EXIT_SUCCESS);
	}
	args = strdup(conf->probe_args);
	if (! args) exit(1);

	c = strchr(args, ':');
	if (! c) {
		free(args);
		//free(udp_send_msg);
		printf("tcp synopt usage error\n");
		exit(1);
	}

	*c++ = 0;

	if (strcmp(args, "hex") == 0) {
		tcp_send_opts_len = strlen(c) / 2;
		if(strlen(c)/2 %4 != 0){
			printf("tcp options are not multiple of 4, please pad with NOPs (0x01)!\n");
			exit(1);
		}
		free(tcp_send_opts);
		tcp_send_opts = xmalloc(tcp_send_opts_len);

		for (i=0; i < tcp_send_opts_len; i++) {
			if (sscanf(c + (i*2), "%2x", &n) != 1) {
				free(args);
				free(tcp_send_opts);
				log_fatal("udp", "non-hex character: '%c'", c[i*2]);
				exit(1);
			}
			tcp_send_opts[i] = (n & 0xff);
		}
	} else {
		printf("options given, but not hex, exiting!");
		exit(1);
	}
	if (tcp_send_opts_len > MAX_OPT_LEN) {
		log_warn("udp", "warning: exiting - too long option!\n");
		tcp_send_opts_len = MAX_OPT_LEN;
		exit(1);
	}
	module_ipv6_tcp_synopt.packet_length = sizeof(struct ether_header) + sizeof(struct ip6_hdr)
			+ sizeof(struct tcphdr)+ tcp_send_opts_len;

	return EXIT_SUCCESS;	
}

int ipv6_tcp_synopt_init_perthread(void* buf, macaddr_t *src,
		macaddr_t *gw, port_h_t dst_port,
		__attribute__((unused)) void **arg_ptr)
{
	memset(buf, 0, MAX_PACKET_SIZE);
	struct ether_header *eth_header = (struct ether_header *) buf;
	make_eth_header_ethertype(eth_header, src, gw, ETHERTYPE_IPV6);
	struct ip6_hdr *ip6_header = (struct ip6_hdr*)(&eth_header[1]);
	uint16_t payload_len = sizeof(struct tcphdr)+tcp_send_opts_len;
	make_ip6_header(ip6_header, IPPROTO_TCP, payload_len);
	struct tcphdr *tcp_header = (struct tcphdr*)(&ip6_header[1]);
	make_tcp_header(tcp_header, dst_port, TH_SYN);
	return EXIT_SUCCESS;
}

int ipv6_tcp_synopt_make_packet(void *buf, UNUSED size_t *buf_len, __attribute__((unused)) ipaddr_n_t src_ip, __attribute__((unused)) ipaddr_n_t dst_ip,
        uint8_t ttl, uint32_t *validation, int probe_num, void *arg)
{
	struct ether_header *eth_header = (struct ether_header *) buf;
	struct ip6_hdr *ip6_header = (struct ip6_hdr*) (&eth_header[1]);
	struct tcphdr *tcp_header = (struct tcphdr*) (&ip6_header[1]);
	unsigned char* opts = (unsigned char*)&tcp_header[1];
	uint32_t tcp_seq = validation[0];

	ip6_header->ip6_src = ((struct in6_addr *) arg)[0];
	ip6_header->ip6_dst = ((struct in6_addr *) arg)[1];
	ip6_header->ip6_ctlun.ip6_un1.ip6_un1_hlim = ttl;

	tcp_header->th_sport = htons(get_src_port(num_ports,
				probe_num, validation));
	tcp_header->th_seq = tcp_seq;

    memcpy(opts, tcp_send_opts, tcp_send_opts_len);

    tcp_header->th_off = 5+tcp_send_opts_len/4; // default length = 5 + 9*32 bit options

	
	tcp_header->th_sum = 0;
	tcp_header->th_sum = tcp6_checksum(sizeof(struct tcphdr)+tcp_send_opts_len,
			&ip6_header->ip6_src, &ip6_header->ip6_dst, tcp_header);

	return EXIT_SUCCESS;
}

void ipv6_tcp_synopt_print_packet(FILE *fp, void* packet)
{
	struct ether_header *ethh = (struct ether_header *) packet;
	struct ip6_hdr *iph = (struct ip6_hdr *) &ethh[1];
	struct tcphdr *tcph = (struct tcphdr *) &iph[1];
	fprintf(fp, "tcp { source: %u | dest: %u | seq: %u | checksum: %#04X }\n",
			ntohs(tcph->th_sport),
			ntohs(tcph->th_dport),
			ntohl(tcph->th_seq),
			ntohs(tcph->th_sum));
	fprintf_ipv6_header(fp, iph);
	fprintf_eth_header(fp, ethh);
	fprintf(fp, "------------------------------------------------------\n");
}

int ipv6_tcp_synopt_validate_packet(const struct ip *ip_hdr, uint32_t len,
		__attribute__((unused))uint32_t *src_ip,
		uint32_t *validation)
{
	struct ip6_hdr *ipv6_hdr = (struct ip6_hdr *) ip_hdr;

	if (ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_nxt != IPPROTO_TCP) {
		return 0;
	}
	if ((ntohs(ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_plen)) > len) {
		// buffer not large enough to contain expected tcp header, i.e. IPv6 payload
		return 0;
	}
	struct tcphdr *tcp_hdr = (struct tcphdr*) (&ipv6_hdr[1]);
	uint16_t sport = tcp_hdr->th_sport;
	uint16_t dport = tcp_hdr->th_dport;
	// validate source port
	if (ntohs(sport) != zconf.target_port) {
		return 0;
	}
	// validate destination port
	if (!check_dst_port(ntohs(dport), num_ports, validation)) {
		return 0;
	}
	// validate tcp acknowledgement number
	if (htonl(tcp_hdr->th_ack) != htonl(validation[0])+1) {
		return 0;
	}
	return 1;
}

void ipv6_tcp_synopt_process_packet(const u_char *packet,
		__attribute__((unused)) uint32_t len, fieldset_t *fs,
		__attribute__((unused)) uint32_t *validation)
{
	struct ether_header *eth_hdr = (struct ether_header *) packet;
	struct ip6_hdr *ipv6_hdr = (struct ip6_hdr *) (&eth_hdr[1]);
	struct tcphdr *tcp_hdr = (struct tcphdr*) (&ipv6_hdr[1]);
	//	unsigned int optionbytes2=len-(sizeof(struct ether_header)+ntohs(ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_plen) + sizeof(struct tcphdr));
	unsigned int optionbytes2=len-(sizeof(struct ether_header)+sizeof(struct ip6_hdr) + sizeof(struct tcphdr));
	tcpsynopt_process_packet_parse(len, fs,tcp_hdr,optionbytes2);
	return;
}

probe_module_t module_ipv6_tcp_synopt = {
	.name = "ipv6_tcp_synopt",
	.packet_length = 74, // will be extended at runtime
	.pcap_filter = "ip6 proto 6 && (ip6[53] & 4 != 0 || ip6[53] == 18)",
	.pcap_snaplen = 116+10*4, // max option len
	.port_args = 1,
	.global_initialize = &ipv6_tcp_synopt_global_initialize,
	.thread_initialize = &ipv6_tcp_synopt_init_perthread,
	.make_packet = &ipv6_tcp_synopt_make_packet,
	.print_packet = &ipv6_tcp_synopt_print_packet,
	.process_packet = &ipv6_tcp_synopt_process_packet,
	.validate_packet = &ipv6_tcp_synopt_validate_packet,
	.close = NULL,
	.helptext = "Probe module that sends an IPv6+TCP SYN packet to a specific "
		"port. Possible classifications are: synack and rst. A "
		"SYN-ACK packet is considered a success and a reset packet "
		"is considered a failed response.",
	.fields = fields,
	.numfields = sizeof(fields)/sizeof(fields[0])
	};
//	.numfields = 10};


