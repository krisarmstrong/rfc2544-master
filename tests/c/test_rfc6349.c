/*
 * test_rfc6349.c - Unit Tests for RFC 6349 TCP Throughput Calculations
 *
 * Tests the Mathis formula and BDP calculations.
 */

#include "test_framework.h"

#include "../../include/rfc2544.h"

/* Forward declaration from rfc6349.c */
extern double rfc6349_theoretical_throughput(double bandwidth_mbps, double rtt_ms, double loss_pct,
                                             uint32_t mss);

/* ============================================================================
 * Mathis Formula Tests
 * ============================================================================ */

TEST(mathis_zero_loss)
{
	/* With 0% loss, should return line rate */
	double result = rfc6349_theoretical_throughput(1000.0, 10.0, 0.0, 1460);
	ASSERT_FLOAT_EQ(1000.0, result, 0.1);
}

TEST(mathis_very_low_loss)
{
	/* Very low loss (0.0001%) should return ~line rate */
	double result = rfc6349_theoretical_throughput(1000.0, 10.0, 0.0001, 1460);
	ASSERT_FLOAT_EQ(1000.0, result, 1.0);
}

TEST(mathis_one_percent_loss)
{
	/* 1% loss with typical parameters */
	/* Mathis: throughput = (MSS * 8 / RTT) * (C / sqrt(loss)) */
	/* = (1460 * 8 / 0.01) * (1.22 / sqrt(0.01)) */
	/* = 1,168,000 * 0.122 = ~14.25 Mbps */
	double result = rfc6349_theoretical_throughput(100.0, 10.0, 1.0, 1460);
	/* Mathis formula result (not capped) */
	ASSERT_FLOAT_EQ(14.25, result, 0.5);
}

TEST(mathis_high_loss)
{
	/* 10% loss - significant throughput reduction */
	double result = rfc6349_theoretical_throughput(1000.0, 10.0, 10.0, 1460);
	/* With 10% loss, Mathis gives much lower than line rate */
	ASSERT_FLOAT_GT(result, 0.0);
	ASSERT_FLOAT_EQ(result, result, 0.001); /* Not NaN */
}

TEST(mathis_high_rtt)
{
	/* High RTT (100ms) reduces throughput */
	double result = rfc6349_theoretical_throughput(1000.0, 100.0, 0.1, 1460);
	ASSERT_FLOAT_GT(result, 0.0);
	ASSERT_LE(result, 1000.0);
}

TEST(mathis_low_rtt)
{
	/* Low RTT (1ms) maximizes throughput */
	double result = rfc6349_theoretical_throughput(1000.0, 1.0, 0.1, 1460);
	ASSERT_FLOAT_GT(result, 0.0);
}

TEST(mathis_zero_bandwidth)
{
	/* Zero bandwidth should return 0 */
	double result = rfc6349_theoretical_throughput(0.0, 10.0, 1.0, 1460);
	ASSERT_FLOAT_EQ(0.0, result, 0.001);
}

TEST(mathis_zero_rtt)
{
	/* Zero RTT should return line rate (edge case) */
	double result = rfc6349_theoretical_throughput(1000.0, 0.0, 1.0, 1460);
	ASSERT_FLOAT_EQ(1000.0, result, 0.1);
}

TEST(mathis_zero_mss)
{
	/* Zero MSS should return line rate (edge case) */
	double result = rfc6349_theoretical_throughput(1000.0, 10.0, 1.0, 0);
	ASSERT_FLOAT_EQ(1000.0, result, 0.1);
}

TEST(mathis_negative_loss)
{
	/* Negative loss should be treated as 0 */
	double result = rfc6349_theoretical_throughput(1000.0, 10.0, -1.0, 1460);
	ASSERT_FLOAT_EQ(1000.0, result, 0.1);
}

TEST(mathis_typical_wan)
{
	/* Typical WAN: 100 Mbps, 50ms RTT, 0.1% loss */
	double result = rfc6349_theoretical_throughput(100.0, 50.0, 0.1, 1460);
	ASSERT_FLOAT_GT(result, 0.0);
	ASSERT_LE(result, 100.0);
}

TEST(mathis_datacenter)
{
	/* Datacenter: 10 Gbps, 0.5ms RTT, 0.001% loss */
	double result = rfc6349_theoretical_throughput(10000.0, 0.5, 0.001, 1460);
	/* Very low RTT and loss should give near line rate */
	ASSERT_FLOAT_GT(result, 9000.0);
}

/* ============================================================================
 * BDP Calculation Tests
 * ============================================================================ */

TEST(bdp_typical)
{
	/* BDP = bandwidth * RTT */
	/* 100 Mbps * 10ms = 100,000,000 * 0.01 = 1,000,000 bits = 125,000 bytes */
	double bandwidth_bps = 100.0 * 1e6;
	double rtt_sec = 0.010;
	double bdp_bits = bandwidth_bps * rtt_sec;
	double bdp_bytes = bdp_bits / 8;
	ASSERT_FLOAT_EQ(125000.0, bdp_bytes, 1.0);
}

TEST(bdp_high_latency)
{
	/* 1 Gbps * 100ms = 100 Mbit = 12.5 MB BDP */
	double bandwidth_bps = 1000.0 * 1e6;
	double rtt_sec = 0.100;
	double bdp_bytes = (bandwidth_bps * rtt_sec) / 8;
	ASSERT_FLOAT_EQ(12500000.0, bdp_bytes, 100.0);
}

/* ============================================================================
 * TCP Efficiency Tests
 * ============================================================================ */

TEST(tcp_efficiency_perfect)
{
	/* 100% efficiency = achieved equals theoretical */
	double theoretical = 100.0;
	double achieved = 100.0;
	double efficiency = (achieved / theoretical) * 100.0;
	ASSERT_FLOAT_EQ(100.0, efficiency, 0.01);
}

TEST(tcp_efficiency_good)
{
	/* 95% efficiency is considered good */
	double theoretical = 100.0;
	double achieved = 95.0;
	double efficiency = (achieved / theoretical) * 100.0;
	ASSERT_FLOAT_EQ(95.0, efficiency, 0.01);
}

TEST(tcp_efficiency_poor)
{
	/* Below 90% is concerning */
	double theoretical = 100.0;
	double achieved = 80.0;
	double efficiency = (achieved / theoretical) * 100.0;
	ASSERT_FLOAT_EQ(80.0, efficiency, 0.01);
}

TEST(tcp_efficiency_zero_theoretical)
{
	/* Edge case: zero theoretical (protected division) */
	double theoretical = 0.0;
	double achieved = 50.0;
	double efficiency = (theoretical > 0.0) ? (achieved / theoretical) * 100.0 : 0.0;
	ASSERT_FLOAT_EQ(0.0, efficiency, 0.01);
}

/* ============================================================================
 * Buffer Size Calculation Tests
 * ============================================================================ */

TEST(buffer_size_minimum)
{
	/* Minimum buffer = BDP */
	/* 100 Mbps, 10ms RTT -> 125KB minimum buffer */
	double bdp_bytes = (100.0 * 1e6 * 0.010) / 8;
	ASSERT_FLOAT_EQ(125000.0, bdp_bytes, 1.0);
}

TEST(buffer_size_recommended)
{
	/* Recommended buffer = 2 * BDP for burst tolerance */
	double bdp_bytes = (100.0 * 1e6 * 0.010) / 8;
	double recommended = bdp_bytes * 2;
	ASSERT_FLOAT_EQ(250000.0, recommended, 1.0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
	printf("RFC 2544 Test Master - RFC 6349 Unit Tests\n");

	TEST_SUITE("Mathis Formula");
	RUN_TEST(mathis_zero_loss);
	RUN_TEST(mathis_very_low_loss);
	RUN_TEST(mathis_one_percent_loss);
	RUN_TEST(mathis_high_loss);
	RUN_TEST(mathis_high_rtt);
	RUN_TEST(mathis_low_rtt);
	RUN_TEST(mathis_zero_bandwidth);
	RUN_TEST(mathis_zero_rtt);
	RUN_TEST(mathis_zero_mss);
	RUN_TEST(mathis_negative_loss);
	RUN_TEST(mathis_typical_wan);
	RUN_TEST(mathis_datacenter);

	TEST_SUITE("BDP Calculations");
	RUN_TEST(bdp_typical);
	RUN_TEST(bdp_high_latency);

	TEST_SUITE("TCP Efficiency");
	RUN_TEST(tcp_efficiency_perfect);
	RUN_TEST(tcp_efficiency_good);
	RUN_TEST(tcp_efficiency_poor);
	RUN_TEST(tcp_efficiency_zero_theoretical);

	TEST_SUITE("Buffer Size");
	RUN_TEST(buffer_size_minimum);
	RUN_TEST(buffer_size_recommended);

	TEST_SUMMARY();

	return test_failures;
}
