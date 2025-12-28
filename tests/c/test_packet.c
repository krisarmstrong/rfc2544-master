/*
 * test_packet.c - Unit Tests for Packet Generation and Parsing
 *
 * Tests packet creation, validation, and field extraction.
 */

#include "test_framework.h"

#include "../../include/rfc2544.h"
#include <arpa/inet.h>
#include <string.h>

/*
 * Local struct definition for testing - matches packet.c internal layout.
 * Using a different name to avoid typedef collision when linking.
 */
typedef struct __attribute__((packed)) {
	uint8_t signature[RFC2544_SIG_LEN];
	uint32_t seq_num;
	uint64_t timestamp;
	uint32_t stream_id;
	uint8_t flags;
} test_payload_t;

/* Forward declarations from packet.c - using void* to avoid typedef issues */
extern void *rfc2544_create_packet_template(uint8_t *buffer, uint32_t frame_size,
                                            const uint8_t *src_mac, const uint8_t *dst_mac,
                                            uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                                            uint16_t dst_port, uint32_t stream_id);

extern void rfc2544_stamp_packet(void *payload, uint32_t seq_num, uint64_t timestamp_ns);

extern bool rfc2544_is_valid_response(const uint8_t *data, uint32_t len);
extern uint32_t rfc2544_get_seq_num(const uint8_t *data, uint32_t len);
extern uint64_t rfc2544_get_tx_timestamp(const uint8_t *data, uint32_t len);
extern uint64_t rfc2544_calc_latency(uint64_t tx_timestamp_ns, uint64_t rx_timestamp_ns);

extern void rfc2544_calc_latency_stats(const uint64_t *samples, uint32_t count,
                                       latency_stats_t *stats);

/* ============================================================================
 * Packet Template Creation Tests
 * ============================================================================ */

TEST(create_packet_null_buffer)
{
	uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
	uint8_t dst_mac[6] = {0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

	test_payload_t *result =
	    rfc2544_create_packet_template(NULL, 64, src_mac, dst_mac, 0x0100000a, 0x0200000a, 12345,
	                                   54321, 1);
	ASSERT_NULL(result);
}

TEST(create_packet_too_small)
{
	uint8_t buffer[64];
	uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
	uint8_t dst_mac[6] = {0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

	/* Frame size below minimum should fail (need 66 bytes for headers + payload) */
	test_payload_t *result =
	    rfc2544_create_packet_template(buffer, 32, src_mac, dst_mac, 0x0100000a, 0x0200000a, 12345,
	                                   54321, 1);
	ASSERT_NULL(result);
}

TEST(create_packet_64byte_too_small)
{
	uint8_t buffer[64];
	uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
	uint8_t dst_mac[6] = {0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

	/* 64-byte frame doesn't fit our 24-byte payload (need 66 minimum) */
	test_payload_t *result =
	    rfc2544_create_packet_template(buffer, 64, src_mac, dst_mac, 0x0100000a, 0x0200000a, 12345,
	                                   54321, 1);
	ASSERT_NULL(result);
}

TEST(create_packet_minimum_size)
{
	uint8_t buffer[68];
	uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
	uint8_t dst_mac[6] = {0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

	/* Minimum valid frame: 14 ETH + 20 IP + 8 UDP + 24 payload = 66 bytes */
	test_payload_t *result =
	    rfc2544_create_packet_template(buffer, 68, src_mac, dst_mac, 0x0100000a, 0x0200000a, 12345,
	                                   54321, 1);
	ASSERT_NOT_NULL(result);
}

TEST(create_packet_signature)
{
	uint8_t buffer[128];
	uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
	uint8_t dst_mac[6] = {0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

	test_payload_t *payload =
	    rfc2544_create_packet_template(buffer, 128, src_mac, dst_mac, 0x0100000a, 0x0200000a, 12345,
	                                   54321, 1);
	ASSERT_NOT_NULL(payload);

	/* Verify signature */
	ASSERT_MEM_EQ(RFC2544_SIGNATURE, payload->signature, RFC2544_SIG_LEN);
}

TEST(create_packet_stream_id)
{
	uint8_t buffer[128];
	uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
	uint8_t dst_mac[6] = {0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

	test_payload_t *payload =
	    rfc2544_create_packet_template(buffer, 128, src_mac, dst_mac, 0x0100000a, 0x0200000a, 12345,
	                                   54321, 42);
	ASSERT_NOT_NULL(payload);

	/* Verify stream ID (network order) */
	ASSERT_EQ(42, ntohl(payload->stream_id));
}

TEST(create_packet_mac_addresses)
{
	uint8_t buffer[128];
	uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
	uint8_t dst_mac[6] = {0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

	test_payload_t *payload =
	    rfc2544_create_packet_template(buffer, 128, src_mac, dst_mac, 0x0100000a, 0x0200000a, 12345,
	                                   54321, 1);
	ASSERT_NOT_NULL(payload);

	/* Verify MACs at start of buffer (Ethernet header) */
	ASSERT_MEM_EQ(dst_mac, buffer, 6);
	ASSERT_MEM_EQ(src_mac, buffer + 6, 6);
}

/* ============================================================================
 * Packet Stamping Tests
 * ============================================================================ */

TEST(stamp_packet_null)
{
	/* Should not crash with NULL */
	rfc2544_stamp_packet(NULL, 100, 1000000000ULL);
	ASSERT_TRUE(1); /* If we get here, didn't crash */
}

TEST(stamp_packet_seq_num)
{
	uint8_t buffer[128];
	uint8_t src_mac[6] = {0};
	uint8_t dst_mac[6] = {0};

	test_payload_t *payload =
	    rfc2544_create_packet_template(buffer, 128, src_mac, dst_mac, 0, 0, 0, 0, 0);
	ASSERT_NOT_NULL(payload);

	rfc2544_stamp_packet(payload, 12345, 0);
	ASSERT_EQ(12345, ntohl(payload->seq_num));
}

TEST(stamp_packet_seq_num_max)
{
	uint8_t buffer[128];
	uint8_t src_mac[6] = {0};
	uint8_t dst_mac[6] = {0};

	test_payload_t *payload =
	    rfc2544_create_packet_template(buffer, 128, src_mac, dst_mac, 0, 0, 0, 0, 0);
	ASSERT_NOT_NULL(payload);

	rfc2544_stamp_packet(payload, 0xFFFFFFFF, 0);
	ASSERT_EQ(0xFFFFFFFF, ntohl(payload->seq_num));
}

/* ============================================================================
 * Packet Validation Tests
 * ============================================================================ */

TEST(is_valid_response_null)
{
	ASSERT_FALSE(rfc2544_is_valid_response(NULL, 64));
}

TEST(is_valid_response_too_short)
{
	uint8_t buffer[32] = {0};
	ASSERT_FALSE(rfc2544_is_valid_response(buffer, 32));
}

TEST(is_valid_response_valid_packet)
{
	uint8_t buffer[128];
	uint8_t src_mac[6] = {0};
	uint8_t dst_mac[6] = {0};

	test_payload_t *payload =
	    rfc2544_create_packet_template(buffer, 128, src_mac, dst_mac, 0, 0, 0, 0, 0);
	ASSERT_NOT_NULL(payload);

	ASSERT_TRUE(rfc2544_is_valid_response(buffer, 128));
}

TEST(is_valid_response_wrong_signature)
{
	uint8_t buffer[128];
	uint8_t src_mac[6] = {0};
	uint8_t dst_mac[6] = {0};

	test_payload_t *payload =
	    rfc2544_create_packet_template(buffer, 128, src_mac, dst_mac, 0, 0, 0, 0, 0);
	ASSERT_NOT_NULL(payload);

	/* Corrupt signature */
	payload->signature[0] = 'X';

	ASSERT_FALSE(rfc2544_is_valid_response(buffer, 128));
}

/* ============================================================================
 * Sequence Number Extraction Tests
 * ============================================================================ */

TEST(get_seq_num_invalid_packet)
{
	uint8_t buffer[32] = {0};
	uint32_t seq = rfc2544_get_seq_num(buffer, 32);
	ASSERT_EQ(0, seq);
}

TEST(get_seq_num_valid)
{
	uint8_t buffer[128];
	uint8_t src_mac[6] = {0};
	uint8_t dst_mac[6] = {0};

	test_payload_t *payload =
	    rfc2544_create_packet_template(buffer, 128, src_mac, dst_mac, 0, 0, 0, 0, 0);
	ASSERT_NOT_NULL(payload);

	rfc2544_stamp_packet(payload, 54321, 0);

	uint32_t seq = rfc2544_get_seq_num(buffer, 128);
	ASSERT_EQ(54321, seq);
}

/* ============================================================================
 * Latency Calculation Tests
 * ============================================================================ */

TEST(calc_latency_normal)
{
	uint64_t tx = 1000000000ULL; /* 1 second */
	uint64_t rx = 1000001000ULL; /* 1 second + 1ms */
	uint64_t latency = rfc2544_calc_latency(tx, rx);
	ASSERT_EQ(1000, latency); /* 1ms = 1000ns? Actually 1000000ns */
}

TEST(calc_latency_zero)
{
	uint64_t tx = 1000000000ULL;
	uint64_t rx = 1000000000ULL;
	uint64_t latency = rfc2544_calc_latency(tx, rx);
	ASSERT_EQ(0, latency);
}

TEST(calc_latency_rx_before_tx)
{
	/* Edge case: RX before TX (clock skew) should return 0 */
	uint64_t tx = 2000000000ULL;
	uint64_t rx = 1000000000ULL;
	uint64_t latency = rfc2544_calc_latency(tx, rx);
	ASSERT_EQ(0, latency);
}

TEST(calc_latency_large_value)
{
	uint64_t tx = 0;
	uint64_t rx = 1000000000000ULL; /* 1000 seconds */
	uint64_t latency = rfc2544_calc_latency(tx, rx);
	ASSERT_EQ(1000000000000ULL, latency);
}

/* ============================================================================
 * Latency Statistics Tests
 * ============================================================================ */

TEST(calc_latency_stats_null)
{
	latency_stats_t stats;
	rfc2544_calc_latency_stats(NULL, 10, &stats);
	ASSERT_FLOAT_EQ(0.0, stats.avg_ns, 0.001);
}

TEST(calc_latency_stats_zero_count)
{
	uint64_t samples[] = {100, 200, 300};
	latency_stats_t stats;
	rfc2544_calc_latency_stats(samples, 0, &stats);
	ASSERT_FLOAT_EQ(0.0, stats.avg_ns, 0.001);
}

TEST(calc_latency_stats_single_sample)
{
	uint64_t samples[] = {1000000}; /* 1ms */
	latency_stats_t stats;
	rfc2544_calc_latency_stats(samples, 1, &stats);
	ASSERT_FLOAT_EQ(1000000.0, stats.avg_ns, 1.0);
	ASSERT_FLOAT_EQ(1000000.0, stats.min_ns, 1.0);
	ASSERT_FLOAT_EQ(1000000.0, stats.max_ns, 1.0);
}

TEST(calc_latency_stats_multiple_samples)
{
	uint64_t samples[] = {1000, 2000, 3000, 4000, 5000};
	latency_stats_t stats;
	rfc2544_calc_latency_stats(samples, 5, &stats);

	ASSERT_FLOAT_EQ(3000.0, stats.avg_ns, 1.0);
	ASSERT_FLOAT_EQ(1000.0, stats.min_ns, 1.0);
	ASSERT_FLOAT_EQ(5000.0, stats.max_ns, 1.0);
}

TEST(calc_latency_stats_jitter)
{
	/* Jitter is mean absolute deviation from average */
	uint64_t samples[] = {1000, 2000, 3000, 4000, 5000};
	latency_stats_t stats;
	rfc2544_calc_latency_stats(samples, 5, &stats);

	/* Average = 3000, deviations: 2000, 1000, 0, 1000, 2000 */
	/* Mean absolute deviation = (2000+1000+0+1000+2000)/5 = 1200 */
	ASSERT_FLOAT_EQ(1200.0, stats.jitter_ns, 1.0);
}

/* ============================================================================
 * Frame Size Validation Tests
 * ============================================================================ */

TEST(frame_size_minimum_valid)
{
	ASSERT_GE(64, RFC2544_MIN_FRAME);
}

TEST(frame_size_standard_sizes)
{
	/* Standard RFC 2544 frame sizes */
	uint32_t sizes[] = {64, 128, 256, 512, 1024, 1280, 1518};
	for (int i = 0; i < 7; i++) {
		ASSERT_GE(sizes[i], RFC2544_MIN_FRAME);
	}
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
	printf("RFC 2544 Test Master - Packet Unit Tests\n");

	TEST_SUITE("Packet Template Creation");
	RUN_TEST(create_packet_null_buffer);
	RUN_TEST(create_packet_too_small);
	RUN_TEST(create_packet_64byte_too_small);
	RUN_TEST(create_packet_minimum_size);
	RUN_TEST(create_packet_signature);
	RUN_TEST(create_packet_stream_id);
	RUN_TEST(create_packet_mac_addresses);

	TEST_SUITE("Packet Stamping");
	RUN_TEST(stamp_packet_null);
	RUN_TEST(stamp_packet_seq_num);
	RUN_TEST(stamp_packet_seq_num_max);

	TEST_SUITE("Packet Validation");
	RUN_TEST(is_valid_response_null);
	RUN_TEST(is_valid_response_too_short);
	RUN_TEST(is_valid_response_valid_packet);
	RUN_TEST(is_valid_response_wrong_signature);

	TEST_SUITE("Sequence Number Extraction");
	RUN_TEST(get_seq_num_invalid_packet);
	RUN_TEST(get_seq_num_valid);

	TEST_SUITE("Latency Calculations");
	RUN_TEST(calc_latency_normal);
	RUN_TEST(calc_latency_zero);
	RUN_TEST(calc_latency_rx_before_tx);
	RUN_TEST(calc_latency_large_value);

	TEST_SUITE("Latency Statistics");
	RUN_TEST(calc_latency_stats_null);
	RUN_TEST(calc_latency_stats_zero_count);
	RUN_TEST(calc_latency_stats_single_sample);
	RUN_TEST(calc_latency_stats_multiple_samples);
	RUN_TEST(calc_latency_stats_jitter);

	TEST_SUITE("Frame Size Validation");
	RUN_TEST(frame_size_minimum_valid);
	RUN_TEST(frame_size_standard_sizes);

	TEST_SUMMARY();

	return test_failures;
}
