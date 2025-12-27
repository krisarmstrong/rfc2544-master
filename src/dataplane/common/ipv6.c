/*
 * ipv6.c - RFC 5180 IPv6 Benchmarking Implementation
 *
 * Implements IPv6 packet generation and testing for RFC 2544
 * tests over IPv6 networks as specified in RFC 5180.
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

/* IPv6 header offsets */
#define IPV6_VERSION_OFFSET 0
#define IPV6_TRAFFIC_CLASS_OFFSET 0
#define IPV6_FLOW_LABEL_OFFSET 1
#define IPV6_PAYLOAD_LEN_OFFSET 4
#define IPV6_NEXT_HEADER_OFFSET 6
#define IPV6_HOP_LIMIT_OFFSET 7
#define IPV6_SRC_ADDR_OFFSET 8
#define IPV6_DST_ADDR_OFFSET 24
#define IPV6_HDR_LEN 40

/* IPv6 next header values */
#define IPV6_NH_UDP 17
#define IPV6_NH_TCP 6
#define IPV6_NH_ICMPV6 58

/**
 * Parse IPv6 address from string
 */
int rfc2544_parse_ipv6(const char *str, uint8_t addr[16])
{
	if (!str || !addr)
		return -EINVAL;

	struct in6_addr in6;
	if (inet_pton(AF_INET6, str, &in6) != 1)
		return -EINVAL;

	memcpy(addr, &in6, 16);
	return 0;
}

/**
 * Format IPv6 address to string
 */
static void ipv6_to_string(const uint8_t addr[16], char *str, size_t len)
{
	struct in6_addr in6;
	memcpy(&in6, addr, 16);
	inet_ntop(AF_INET6, &in6, str, len);
}

/**
 * Configure IPv6 test parameters
 */
int rfc2544_ipv6_configure(rfc2544_ctx_t *ctx, const ipv6_config_t *config)
{
	if (!ctx || !config)
		return -EINVAL;

	/* Store IPv6 configuration */
	memcpy(&ctx->config.ipv6, config, sizeof(ipv6_config_t));
	ctx->config.ip_mode = IP_MODE_V6;

	char src_str[INET6_ADDRSTRLEN], dst_str[INET6_ADDRSTRLEN];
	ipv6_to_string(config->src_addr, src_str, sizeof(src_str));
	ipv6_to_string(config->dst_addr, dst_str, sizeof(dst_str));

	rfc2544_log(LOG_INFO, "IPv6 configured: %s -> %s, TC=%u, FL=%u, HL=%u",
	            src_str, dst_str, config->traffic_class,
	            config->flow_label, config->hop_limit);

	return 0;
}

/**
 * Build IPv6 header
 */
int rfc2544_build_ipv6_header(uint8_t *buffer, uint16_t payload_len,
                              const ipv6_config_t *config)
{
	if (!buffer || !config)
		return -EINVAL;

	/* Version (4) | Traffic Class (8) | Flow Label (20) */
	uint32_t ver_tc_fl = (6 << 28) |                          /* Version 6 */
	                     ((config->traffic_class & 0xFF) << 20) |
	                     (config->flow_label & 0xFFFFF);

	buffer[0] = (ver_tc_fl >> 24) & 0xFF;
	buffer[1] = (ver_tc_fl >> 16) & 0xFF;
	buffer[2] = (ver_tc_fl >> 8) & 0xFF;
	buffer[3] = ver_tc_fl & 0xFF;

	/* Payload length */
	buffer[4] = (payload_len >> 8) & 0xFF;
	buffer[5] = payload_len & 0xFF;

	/* Next header (UDP) */
	buffer[6] = IPV6_NH_UDP;

	/* Hop limit */
	buffer[7] = config->hop_limit;

	/* Source address */
	memcpy(&buffer[8], config->src_addr, 16);

	/* Destination address */
	memcpy(&buffer[24], config->dst_addr, 16);

	return IPV6_HDR_LEN;
}

/**
 * Get default IPv6 configuration
 */
void rfc2544_ipv6_default_config(ipv6_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));

	/* Default: link-local addresses */
	/* fe80::1 */
	config->src_addr[0] = 0xfe;
	config->src_addr[1] = 0x80;
	config->src_addr[15] = 0x01;

	/* fe80::2 */
	config->dst_addr[0] = 0xfe;
	config->dst_addr[1] = 0x80;
	config->dst_addr[15] = 0x02;

	config->traffic_class = 0;  /* Best effort */
	config->flow_label = 0;
	config->hop_limit = 64;
}

/**
 * Calculate IPv6 UDP pseudo-header checksum
 */
uint16_t rfc2544_ipv6_udp_checksum(const uint8_t *src_addr, const uint8_t *dst_addr,
                                    uint16_t udp_len, const uint8_t *udp_data)
{
	uint32_t sum = 0;

	/* Pseudo-header: source address */
	for (int i = 0; i < 16; i += 2) {
		sum += (src_addr[i] << 8) | src_addr[i + 1];
	}

	/* Pseudo-header: destination address */
	for (int i = 0; i < 16; i += 2) {
		sum += (dst_addr[i] << 8) | dst_addr[i + 1];
	}

	/* Pseudo-header: UDP length (32-bit) */
	sum += udp_len;

	/* Pseudo-header: Next header (UDP = 17) */
	sum += IPV6_NH_UDP;

	/* UDP header + data */
	const uint16_t *ptr = (const uint16_t *)udp_data;
	int remaining = udp_len;

	while (remaining > 1) {
		sum += *ptr++;
		remaining -= 2;
	}

	/* Handle odd byte */
	if (remaining == 1) {
		sum += *(const uint8_t *)ptr << 8;
	}

	/* Fold 32-bit sum to 16-bit */
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}

	return ~sum;
}
