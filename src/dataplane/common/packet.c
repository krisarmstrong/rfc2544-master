/*
 * packet.c - RFC 2544 Packet Generation and Analysis
 *
 * This module handles:
 * - Packet template creation with RFC2544 signature
 * - Sequence number and timestamp insertion
 * - Packet validation and response matching
 */

#include "rfc2544.h"
#include "platform_config.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <linux/if_ether.h> /* ETH_P_IP */
#endif

/* ============================================================================
 * Ethernet/IP/UDP Header Structures
 * ============================================================================ */

/* Ethernet header (14 bytes) */
typedef struct __attribute__((packed)) {
	uint8_t dst_mac[6];
	uint8_t src_mac[6];
	uint16_t ethertype;
} eth_header_t;

/* IPv4 header (20 bytes, no options) */
typedef struct __attribute__((packed)) {
	uint8_t version_ihl;
	uint8_t tos;
	uint16_t total_length;
	uint16_t identification;
	uint16_t flags_fragment;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t checksum;
	uint32_t src_ip;
	uint32_t dst_ip;
} ip_header_t;

/* UDP header (8 bytes) */
typedef struct __attribute__((packed)) {
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t length;
	uint16_t checksum;
} udp_header_t;

/* RFC2544 payload header (24 bytes) */
typedef struct __attribute__((packed)) {
	uint8_t signature[RFC2544_SIG_LEN]; /* "RFC2544" */
	uint32_t seq_num;                   /* Sequence number (network order) */
	uint64_t timestamp;                 /* TX timestamp ns (network order) */
	uint32_t stream_id;                 /* Stream ID (network order) */
	uint8_t flags;                      /* Flags */
} rfc2544_payload_t;

/* Complete packet template */
typedef struct {
	eth_header_t eth;
	ip_header_t ip;
	udp_header_t udp;
	rfc2544_payload_t payload;
	/* Padding follows */
} rfc2544_packet_t;

/* ============================================================================
 * Checksum Calculation
 * ============================================================================ */

static uint16_t ip_checksum(const void *data, size_t len)
{
	const uint16_t *ptr = data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *ptr++;
		len -= 2;
	}

	if (len == 1) {
		sum += *(const uint8_t *)ptr;
	}

	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}

	return ~sum;
}

/* ============================================================================
 * Packet Template Creation
 * ============================================================================ */

/**
 * Create a packet template for RFC2544 testing
 *
 * @param buffer Output buffer (must be at least frame_size bytes)
 * @param frame_size Total frame size including Ethernet header
 * @param src_mac Source MAC address
 * @param dst_mac Destination MAC address
 * @param src_ip Source IP (network order)
 * @param dst_ip Destination IP (network order)
 * @param src_port Source UDP port (host order)
 * @param dst_port Destination UDP port (host order)
 * @param stream_id Stream identifier
 * @return Pointer to payload area, or NULL on error
 */
rfc2544_payload_t *rfc2544_create_packet_template(uint8_t *buffer, uint32_t frame_size,
                                                   const uint8_t *src_mac, const uint8_t *dst_mac,
                                                   uint32_t src_ip, uint32_t dst_ip,
                                                   uint16_t src_port, uint16_t dst_port,
                                                   uint32_t stream_id)
{
	if (!buffer || frame_size < RFC2544_MIN_FRAME) {
		return NULL;
	}

	/* Clear buffer */
	memset(buffer, 0, frame_size);

	/* Ethernet header */
	eth_header_t *eth = (eth_header_t *)buffer;
	memcpy(eth->dst_mac, dst_mac, 6);
	memcpy(eth->src_mac, src_mac, 6);
	eth->ethertype = htons(ETH_P_IP);

	/* IP header */
	ip_header_t *ip = (ip_header_t *)(buffer + sizeof(eth_header_t));
	ip->version_ihl = 0x45; /* IPv4, 20 byte header */
	ip->tos = 0;
	ip->total_length = htons(frame_size - sizeof(eth_header_t));
	ip->identification = htons(0x1234);
	ip->flags_fragment = htons(0x4000); /* Don't fragment */
	ip->ttl = 64;
	ip->protocol = IPPROTO_UDP;
	ip->src_ip = src_ip;
	ip->dst_ip = dst_ip;
	ip->checksum = 0;
	ip->checksum = ip_checksum(ip, sizeof(ip_header_t));

	/* UDP header */
	udp_header_t *udp =
	    (udp_header_t *)(buffer + sizeof(eth_header_t) + sizeof(ip_header_t));
	udp->src_port = htons(src_port);
	udp->dst_port = htons(dst_port);
	udp->length = htons(frame_size - sizeof(eth_header_t) - sizeof(ip_header_t));
	udp->checksum = 0; /* Optional for IPv4 */

	/* RFC2544 payload */
	rfc2544_payload_t *payload =
	    (rfc2544_payload_t *)(buffer + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                          sizeof(udp_header_t));
	memcpy(payload->signature, RFC2544_SIGNATURE, RFC2544_SIG_LEN);
	payload->seq_num = 0;   /* Will be set per-packet */
	payload->timestamp = 0; /* Will be set per-packet */
	payload->stream_id = htonl(stream_id);
	payload->flags = RFC2544_FLAG_REQ_TIMESTAMP;

	/* Fill padding with pattern */
	uint8_t *padding = buffer + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                   sizeof(udp_header_t) + sizeof(rfc2544_payload_t);
	size_t padding_len = frame_size - sizeof(eth_header_t) - sizeof(ip_header_t) -
	                     sizeof(udp_header_t) - sizeof(rfc2544_payload_t);

	for (size_t i = 0; i < padding_len; i++) {
		padding[i] = (uint8_t)(i & 0xFF);
	}

	return payload;
}

/**
 * Update packet with new sequence number and timestamp
 *
 * @param payload Pointer to payload (from create_packet_template)
 * @param seq_num Sequence number
 * @param timestamp_ns TX timestamp in nanoseconds
 */
void rfc2544_stamp_packet(rfc2544_payload_t *payload, uint32_t seq_num, uint64_t timestamp_ns)
{
	if (!payload)
		return;

	payload->seq_num = htonl(seq_num);

	/* Store timestamp in network byte order (big-endian) */
	uint64_t ts_be = ((uint64_t)htonl(timestamp_ns & 0xFFFFFFFF) << 32) |
	                 htonl(timestamp_ns >> 32);
	payload->timestamp = ts_be;
}

/**
 * Check if packet is a valid RFC2544 response
 *
 * @param data Packet data
 * @param len Packet length
 * @return true if valid RFC2544 packet
 */
bool rfc2544_is_valid_response(const uint8_t *data, uint32_t len)
{
	if (!data || len < RFC2544_MIN_FRAME) {
		return false;
	}

	/* Skip to payload */
	const rfc2544_payload_t *payload =
	    (const rfc2544_payload_t *)(data + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                                sizeof(udp_header_t));

	/* Check signature */
	if (memcmp(payload->signature, RFC2544_SIGNATURE, RFC2544_SIG_LEN) != 0) {
		return false;
	}

	return true;
}

/**
 * Extract sequence number from received packet
 *
 * @param data Packet data
 * @param len Packet length
 * @return Sequence number, or 0 on error
 */
uint32_t rfc2544_get_seq_num(const uint8_t *data, uint32_t len)
{
	if (!rfc2544_is_valid_response(data, len)) {
		return 0;
	}

	const rfc2544_payload_t *payload =
	    (const rfc2544_payload_t *)(data + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                                sizeof(udp_header_t));

	return ntohl(payload->seq_num);
}

/**
 * Extract TX timestamp from received packet
 *
 * @param data Packet data
 * @param len Packet length
 * @return TX timestamp in nanoseconds, or 0 on error
 */
uint64_t rfc2544_get_tx_timestamp(const uint8_t *data, uint32_t len)
{
	if (!rfc2544_is_valid_response(data, len)) {
		return 0;
	}

	const rfc2544_payload_t *payload =
	    (const rfc2544_payload_t *)(data + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                                sizeof(udp_header_t));

	/* Convert from network byte order */
	uint64_t ts_be = payload->timestamp;
	return ((uint64_t)ntohl(ts_be & 0xFFFFFFFF) << 32) | ntohl(ts_be >> 32);
}

/**
 * Calculate round-trip latency
 *
 * @param tx_timestamp_ns TX timestamp from packet
 * @param rx_timestamp_ns RX timestamp (current time)
 * @return Round-trip latency in nanoseconds
 */
uint64_t rfc2544_calc_latency(uint64_t tx_timestamp_ns, uint64_t rx_timestamp_ns)
{
	if (rx_timestamp_ns > tx_timestamp_ns) {
		return rx_timestamp_ns - tx_timestamp_ns;
	}
	return 0;
}

/* ============================================================================
 * Sequence Tracking
 * ============================================================================ */

/* Bitmap for tracking received sequences */
struct seq_tracker {
	uint64_t *bitmap;
	uint32_t base_seq;
	uint32_t capacity;
	uint32_t received;
	uint32_t duplicates;
	uint32_t out_of_order;
};
typedef struct seq_tracker seq_tracker_t;

/**
 * Create sequence tracker
 *
 * @param capacity Maximum number of sequences to track
 * @return Tracker, or NULL on error
 */
seq_tracker_t *rfc2544_seq_tracker_create(uint32_t capacity)
{
	seq_tracker_t *tracker = calloc(1, sizeof(seq_tracker_t));
	if (!tracker)
		return NULL;

	/* Allocate bitmap (64 sequences per uint64_t) */
	size_t bitmap_size = (capacity + 63) / 64;
	tracker->bitmap = calloc(bitmap_size, sizeof(uint64_t));
	if (!tracker->bitmap) {
		free(tracker);
		return NULL;
	}

	tracker->capacity = capacity;
	tracker->base_seq = 0;
	tracker->received = 0;
	tracker->duplicates = 0;
	tracker->out_of_order = 0;

	return tracker;
}

/**
 * Record received sequence number
 *
 * @param tracker Sequence tracker
 * @param seq_num Sequence number
 */
void rfc2544_seq_tracker_record(seq_tracker_t *tracker, uint32_t seq_num)
{
	if (!tracker)
		return;

	uint32_t offset = seq_num - tracker->base_seq;
	if (offset >= tracker->capacity) {
		/* Out of range - could be reordering or very late */
		tracker->out_of_order++;
		return;
	}

	uint32_t word = offset / 64;
	uint32_t bit = offset % 64;
	uint64_t mask = 1ULL << bit;

	if (tracker->bitmap[word] & mask) {
		/* Already received - duplicate */
		tracker->duplicates++;
	} else {
		tracker->bitmap[word] |= mask;
		tracker->received++;
	}
}

/**
 * Get loss statistics
 *
 * @param tracker Sequence tracker
 * @param expected Expected number of packets
 * @param received Output: received count
 * @param lost Output: lost count
 * @param loss_pct Output: loss percentage
 */
void rfc2544_seq_tracker_stats(const seq_tracker_t *tracker, uint32_t expected, uint32_t *received,
                               uint32_t *lost, double *loss_pct)
{
	if (!tracker)
		return;

	if (received)
		*received = tracker->received;
	if (lost)
		*lost = expected - tracker->received;
	if (loss_pct && expected > 0)
		*loss_pct = 100.0 * (expected - tracker->received) / expected;
}

/**
 * Destroy sequence tracker
 */
void rfc2544_seq_tracker_destroy(seq_tracker_t *tracker)
{
	if (tracker) {
		free(tracker->bitmap);
		free(tracker);
	}
}

/* ============================================================================
 * Latency Statistics
 * ============================================================================ */

/**
 * Calculate latency statistics from samples
 *
 * @param samples Array of latency samples (nanoseconds)
 * @param count Number of samples
 * @param stats Output statistics
 */
void rfc2544_calc_latency_stats(const uint64_t *samples, uint32_t count, latency_stats_t *stats)
{
	if (!samples || count == 0 || !stats) {
		if (stats)
			memset(stats, 0, sizeof(*stats));
		return;
	}

	/* Find min, max, sum */
	uint64_t min_ns = UINT64_MAX;
	uint64_t max_ns = 0;
	uint64_t sum_ns = 0;

	for (uint32_t i = 0; i < count; i++) {
		if (samples[i] < min_ns)
			min_ns = samples[i];
		if (samples[i] > max_ns)
			max_ns = samples[i];
		sum_ns += samples[i];
	}

	stats->count = count;
	stats->min_ns = (double)min_ns;
	stats->max_ns = (double)max_ns;
	stats->avg_ns = (double)sum_ns / count;

	/* Calculate jitter (mean absolute deviation) */
	double jitter_sum = 0;
	for (uint32_t i = 0; i < count; i++) {
		double diff = (double)samples[i] - stats->avg_ns;
		jitter_sum += (diff > 0) ? diff : -diff;
	}
	stats->jitter_ns = jitter_sum / count;

	/* Calculate percentiles (requires sorting - simplified version) */
	/* For accurate percentiles, would need to sort samples */
	stats->p50_ns = stats->avg_ns; /* Approximation */
	stats->p95_ns = stats->avg_ns + 2 * stats->jitter_ns;
	stats->p99_ns = stats->max_ns;
}

/* ============================================================================
 * ITU-T Y.1564 Packet Generation
 * ============================================================================
 *
 * Y.1564 packets use the same structure as RFC2544 with:
 * - Different signature ("Y.1564 ")
 * - DSCP marking in IP header for CoS differentiation
 * - Service ID field for multi-service testing
 */

/* Y.1564 payload header (24 bytes) - same layout as RFC2544 */
typedef struct __attribute__((packed)) {
	uint8_t signature[Y1564_SIG_LEN]; /* "Y.1564 " (space-padded) */
	uint32_t seq_num;                  /* Sequence number (network order) */
	uint64_t timestamp;                /* TX timestamp ns (network order) */
	uint32_t service_id;               /* Service ID (1-8, network order) */
	uint8_t flags;                     /* Flags */
} y1564_payload_t;

/**
 * Create a packet template for Y.1564 testing
 *
 * @param buffer Output buffer (must be at least frame_size bytes)
 * @param frame_size Total frame size including Ethernet header
 * @param src_mac Source MAC address
 * @param dst_mac Destination MAC address
 * @param src_ip Source IP (network order)
 * @param dst_ip Destination IP (network order)
 * @param src_port Source UDP port (host order)
 * @param dst_port Destination UDP port (host order)
 * @param service_id Service identifier (1-8)
 * @param dscp DSCP value for CoS marking (0-63)
 * @return Pointer to payload area, or NULL on error
 */
y1564_payload_t *y1564_create_packet_template(uint8_t *buffer, uint32_t frame_size,
                                               const uint8_t *src_mac, const uint8_t *dst_mac,
                                               uint32_t src_ip, uint32_t dst_ip,
                                               uint16_t src_port, uint16_t dst_port,
                                               uint32_t service_id, uint8_t dscp)
{
	if (!buffer || frame_size < Y1564_MIN_FRAME) {
		return NULL;
	}

	/* Clear buffer */
	memset(buffer, 0, frame_size);

	/* Ethernet header */
	eth_header_t *eth = (eth_header_t *)buffer;
	memcpy(eth->dst_mac, dst_mac, 6);
	memcpy(eth->src_mac, src_mac, 6);
	eth->ethertype = htons(ETH_P_IP);

	/* IP header */
	ip_header_t *ip = (ip_header_t *)(buffer + sizeof(eth_header_t));
	ip->version_ihl = 0x45; /* IPv4, 20 byte header */
	ip->tos = (dscp << 2);  /* DSCP in upper 6 bits, ECN = 0 */
	ip->total_length = htons(frame_size - sizeof(eth_header_t));
	ip->identification = htons(0x1564); /* Y.1564 marker */
	ip->flags_fragment = htons(0x4000); /* Don't fragment */
	ip->ttl = 64;
	ip->protocol = IPPROTO_UDP;
	ip->src_ip = src_ip;
	ip->dst_ip = dst_ip;
	ip->checksum = 0;
	ip->checksum = ip_checksum(ip, sizeof(ip_header_t));

	/* UDP header */
	udp_header_t *udp =
	    (udp_header_t *)(buffer + sizeof(eth_header_t) + sizeof(ip_header_t));
	udp->src_port = htons(src_port);
	udp->dst_port = htons(dst_port);
	udp->length = htons(frame_size - sizeof(eth_header_t) - sizeof(ip_header_t));
	udp->checksum = 0; /* Optional for IPv4 */

	/* Y.1564 payload */
	y1564_payload_t *payload =
	    (y1564_payload_t *)(buffer + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                        sizeof(udp_header_t));
	memcpy(payload->signature, Y1564_SIGNATURE, Y1564_SIG_LEN);
	payload->seq_num = 0;   /* Will be set per-packet */
	payload->timestamp = 0; /* Will be set per-packet */
	payload->service_id = htonl(service_id);
	payload->flags = Y1564_FLAG_REQ_TIMESTAMP;

	/* Fill padding with pattern */
	uint8_t *padding = buffer + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                   sizeof(udp_header_t) + sizeof(y1564_payload_t);
	size_t padding_len = frame_size - sizeof(eth_header_t) - sizeof(ip_header_t) -
	                     sizeof(udp_header_t) - sizeof(y1564_payload_t);

	for (size_t i = 0; i < padding_len; i++) {
		padding[i] = (uint8_t)(i & 0xFF);
	}

	return payload;
}

/**
 * Update Y.1564 packet with new sequence number and timestamp
 *
 * @param payload Pointer to payload (from y1564_create_packet_template)
 * @param seq_num Sequence number
 * @param timestamp_ns TX timestamp in nanoseconds
 */
void y1564_stamp_packet(y1564_payload_t *payload, uint32_t seq_num, uint64_t timestamp_ns)
{
	if (!payload)
		return;

	payload->seq_num = htonl(seq_num);

	/* Store timestamp in network byte order (big-endian) */
	uint64_t ts_be = ((uint64_t)htonl(timestamp_ns & 0xFFFFFFFF) << 32) |
	                 htonl(timestamp_ns >> 32);
	payload->timestamp = ts_be;
}

/**
 * Check if packet is a valid Y.1564 response
 *
 * @param data Packet data
 * @param len Packet length
 * @return true if valid Y.1564 packet
 */
bool y1564_is_valid_response(const uint8_t *data, uint32_t len)
{
	if (!data || len < Y1564_MIN_FRAME) {
		return false;
	}

	/* Skip to payload */
	const y1564_payload_t *payload =
	    (const y1564_payload_t *)(data + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                              sizeof(udp_header_t));

	/* Check signature */
	if (memcmp(payload->signature, Y1564_SIGNATURE, Y1564_SIG_LEN) != 0) {
		return false;
	}

	return true;
}

/**
 * Extract sequence number from received Y.1564 packet
 *
 * @param data Packet data
 * @param len Packet length
 * @return Sequence number, or 0 on error
 */
uint32_t y1564_get_seq_num(const uint8_t *data, uint32_t len)
{
	if (!y1564_is_valid_response(data, len)) {
		return 0;
	}

	const y1564_payload_t *payload =
	    (const y1564_payload_t *)(data + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                              sizeof(udp_header_t));

	return ntohl(payload->seq_num);
}

/**
 * Extract TX timestamp from received Y.1564 packet
 *
 * @param data Packet data
 * @param len Packet length
 * @return TX timestamp in nanoseconds, or 0 on error
 */
uint64_t y1564_get_tx_timestamp(const uint8_t *data, uint32_t len)
{
	if (!y1564_is_valid_response(data, len)) {
		return 0;
	}

	const y1564_payload_t *payload =
	    (const y1564_payload_t *)(data + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                              sizeof(udp_header_t));

	/* Convert from network byte order */
	uint64_t ts_be = payload->timestamp;
	return ((uint64_t)ntohl(ts_be & 0xFFFFFFFF) << 32) | ntohl(ts_be >> 32);
}

/**
 * Extract service ID from received Y.1564 packet
 *
 * @param data Packet data
 * @param len Packet length
 * @return Service ID (1-8), or 0 on error
 */
uint32_t y1564_get_service_id(const uint8_t *data, uint32_t len)
{
	if (!y1564_is_valid_response(data, len)) {
		return 0;
	}

	const y1564_payload_t *payload =
	    (const y1564_payload_t *)(data + sizeof(eth_header_t) + sizeof(ip_header_t) +
	                              sizeof(udp_header_t));

	return ntohl(payload->service_id);
}

/**
 * Calculate round-trip latency for Y.1564
 *
 * @param tx_timestamp_ns TX timestamp from packet
 * @param rx_timestamp_ns RX timestamp (current time)
 * @return Round-trip latency in nanoseconds
 */
uint64_t y1564_calc_latency(uint64_t tx_timestamp_ns, uint64_t rx_timestamp_ns)
{
	if (rx_timestamp_ns > tx_timestamp_ns) {
		return rx_timestamp_ns - tx_timestamp_ns;
	}
	return 0;
}
