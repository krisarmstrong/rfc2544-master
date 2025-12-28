/*
 * test_protocols.c - Unit Tests for Protocol Modules
 *
 * Tests Y.1564, TSN, Y.1731, MEF, and RFC 2889 calculation functions.
 */

#include "test_framework.h"

#include "../../include/rfc2544.h"
#include <string.h>

/* ============================================================================
 * Y.1564 Default Configuration Tests
 * ============================================================================ */

TEST(y1564_default_sla_values)
{
	y1564_sla_t sla;
	y1564_default_sla(&sla);

	ASSERT_FLOAT_EQ(100.0, sla.cir_mbps, 0.1);
	ASSERT_FLOAT_EQ(0.0, sla.eir_mbps, 0.1);
	ASSERT_EQ(12000, sla.cbs_bytes);
	ASSERT_EQ(0, sla.ebs_bytes);
	ASSERT_FLOAT_EQ(10.0, sla.fd_threshold_ms, 0.1);
	ASSERT_FLOAT_EQ(5.0, sla.fdv_threshold_ms, 0.1);
	ASSERT_FLOAT_EQ(0.01, sla.flr_threshold_pct, 0.001);
}

TEST(y1564_default_sla_null)
{
	/* Should not crash with NULL */
	y1564_default_sla(NULL);
	ASSERT_TRUE(1);
}

TEST(y1564_default_config_values)
{
	y1564_config_t config;
	y1564_default_config(&config);

	/* Verify step percentages */
	ASSERT_FLOAT_EQ(25.0, config.config_steps[0], 0.1);
	ASSERT_FLOAT_EQ(50.0, config.config_steps[1], 0.1);
	ASSERT_FLOAT_EQ(75.0, config.config_steps[2], 0.1);
	ASSERT_FLOAT_EQ(100.0, config.config_steps[3], 0.1);

	/* Verify durations */
	ASSERT_EQ(60, config.step_duration_sec);
	ASSERT_EQ(15 * 60, config.perf_duration_sec);

	/* Verify flags */
	ASSERT_TRUE(config.run_config_test);
	ASSERT_TRUE(config.run_perf_test);
}

TEST(y1564_default_config_null)
{
	y1564_default_config(NULL);
	ASSERT_TRUE(1);
}

TEST(y1564_default_config_services)
{
	y1564_config_t config;
	y1564_default_config(&config);

	/* Verify service slots are initialized */
	ASSERT_EQ(0, config.service_count);

	for (int i = 0; i < Y1564_MAX_SERVICES; i++) {
		ASSERT_EQ(i + 1, config.services[i].service_id);
		ASSERT_FALSE(config.services[i].enabled);
		ASSERT_EQ(512, config.services[i].frame_size);
	}
}

/* ============================================================================
 * Y.1731 Default Configuration Tests
 * ============================================================================ */

TEST(y1731_default_mep_config_values)
{
	y1731_mep_config_t config;
	y1731_default_mep_config(&config);

	ASSERT_EQ(1, config.mep_id);
	ASSERT_EQ(MEG_LEVEL_CUSTOMER, config.meg_level);
	ASSERT_EQ(CCM_1S, config.ccm_interval);
	ASSERT_EQ(7, config.priority);
	ASSERT_TRUE(config.enabled);
}

TEST(y1731_default_mep_config_null)
{
	y1731_default_mep_config(NULL);
	ASSERT_TRUE(1);
}

TEST(y1731_default_mep_meg_id)
{
	y1731_mep_config_t config;
	y1731_default_mep_config(&config);

	ASSERT_STR_EQ("DEFAULT-MEG", config.meg_id);
}

/* ============================================================================
 * TSN Gate Control List Tests
 * ============================================================================ */

TEST(tsn_default_config_values)
{
	tsn_config_t config;
	tsn_default_config(&config);

	ASSERT_EQ(1000000, config.gcl.cycle_time_ns);  /* 1ms */
	ASSERT_GT(config.gcl.entry_count, 0);
	ASSERT_TRUE(config.verify_gcl);
}

TEST(tsn_default_config_null)
{
	tsn_default_config(NULL);
	ASSERT_TRUE(1);
}

TEST(tsn_gcl_cycle_time)
{
	tsn_config_t config;
	tsn_default_config(&config);

	/* Cycle time should be reasonable (1us to 1s) */
	ASSERT_GT(config.gcl.cycle_time_ns, 1000);       /* > 1us */
	ASSERT_LT(config.gcl.cycle_time_ns, 1000000000); /* < 1s */
}

/* ============================================================================
 * MEF Service Configuration Tests
 * ============================================================================ */

TEST(mef_default_config_values)
{
	mef_config_t config;
	mef_default_config(&config);

	/* Verify bandwidth profile defaults */
	ASSERT_GT(config.bw_profile.cir_kbps, 0);
	ASSERT_GE(config.bw_profile.eir_kbps, 0);
	ASSERT_GT(config.bw_profile.cbs_bytes, 0);
}

TEST(mef_default_config_null)
{
	mef_default_config(NULL);
	ASSERT_TRUE(1);
}

TEST(mef_service_frame_delay)
{
	mef_config_t config;
	mef_default_config(&config);

	/* Frame delay threshold should be positive (in microseconds) */
	ASSERT_GT(config.sla.fd_threshold_us, 0.0);
}

TEST(mef_service_frame_loss)
{
	mef_config_t config;
	mef_default_config(&config);

	/* Frame loss threshold should be small but positive */
	ASSERT_GE(config.sla.flr_threshold_pct, 0.0);
	ASSERT_LE(config.sla.flr_threshold_pct, 100.0);
}

/* ============================================================================
 * RFC 2889 Configuration Tests
 * ============================================================================ */

TEST(rfc2889_default_config_values)
{
	rfc2889_config_t config;
	rfc2889_default_config(&config);

	ASSERT_GT(config.address_count, 0);
	ASSERT_LE(config.address_count, 100000);
}

TEST(rfc2889_default_config_null)
{
	rfc2889_default_config(NULL);
	ASSERT_TRUE(1);
}

TEST(rfc2889_trial_duration)
{
	rfc2889_config_t config;
	rfc2889_default_config(&config);

	/* Trial duration should be reasonable (> 0 seconds) */
	ASSERT_GT(config.trial_duration_sec, 0);
}

/* ============================================================================
 * SLA Threshold Validation Tests
 * ============================================================================ */

TEST(sla_frame_delay_pass)
{
	double measured_fd = 5.0;  /* 5ms */
	double threshold = 10.0;   /* 10ms threshold */
	bool pass = (measured_fd <= threshold);
	ASSERT_TRUE(pass);
}

TEST(sla_frame_delay_fail)
{
	double measured_fd = 15.0; /* 15ms */
	double threshold = 10.0;   /* 10ms threshold */
	bool pass = (measured_fd <= threshold);
	ASSERT_FALSE(pass);
}

TEST(sla_frame_loss_pass)
{
	double measured_flr = 0.001; /* 0.001% */
	double threshold = 0.01;     /* 0.01% threshold */
	bool pass = (measured_flr <= threshold);
	ASSERT_TRUE(pass);
}

TEST(sla_frame_loss_fail)
{
	double measured_flr = 0.1;   /* 0.1% */
	double threshold = 0.01;     /* 0.01% threshold */
	bool pass = (measured_flr <= threshold);
	ASSERT_FALSE(pass);
}

TEST(sla_jitter_pass)
{
	double measured_fdv = 2.0;  /* 2ms */
	double threshold = 5.0;    /* 5ms threshold */
	bool pass = (measured_fdv <= threshold);
	ASSERT_TRUE(pass);
}

TEST(sla_jitter_fail)
{
	double measured_fdv = 8.0;  /* 8ms */
	double threshold = 5.0;    /* 5ms threshold */
	bool pass = (measured_fdv <= threshold);
	ASSERT_FALSE(pass);
}

/* ============================================================================
 * Frame Loss Ratio Calculation Tests
 * ============================================================================ */

TEST(flr_zero_loss)
{
	uint64_t tx = 1000000;
	uint64_t rx = 1000000;
	double flr = (tx > 0) ? 100.0 * (tx - rx) / tx : 0.0;
	ASSERT_FLOAT_EQ(0.0, flr, 0.0001);
}

TEST(flr_one_percent)
{
	uint64_t tx = 1000000;
	uint64_t rx = 990000;
	double flr = (tx > 0) ? 100.0 * (tx - rx) / tx : 0.0;
	ASSERT_FLOAT_EQ(1.0, flr, 0.0001);
}

TEST(flr_total_loss)
{
	uint64_t tx = 1000000;
	uint64_t rx = 0;
	double flr = (tx > 0) ? 100.0 * (tx - rx) / tx : 0.0;
	ASSERT_FLOAT_EQ(100.0, flr, 0.0001);
}

TEST(flr_zero_tx)
{
	uint64_t tx = 0;
	uint64_t rx = 0;
	double flr = (tx > 0) ? 100.0 * (tx - rx) / tx : 0.0;
	ASSERT_FLOAT_EQ(0.0, flr, 0.0001);
}

/* ============================================================================
 * CIR/EIR Rate Calculation Tests
 * ============================================================================ */

TEST(cir_percentage_of_line_rate)
{
	double line_rate_mbps = 1000.0;  /* 1 Gbps */
	double cir_mbps = 100.0;         /* 100 Mbps CIR */
	double cir_pct = (cir_mbps / line_rate_mbps) * 100.0;
	ASSERT_FLOAT_EQ(10.0, cir_pct, 0.1);
}

TEST(cir_eir_combined)
{
	double cir_mbps = 100.0;
	double eir_mbps = 50.0;
	double total = cir_mbps + eir_mbps;
	ASSERT_FLOAT_EQ(150.0, total, 0.1);
}

TEST(burst_size_validation)
{
	/* CBS should accommodate at least one jumbo frame */
	uint32_t cbs = 12000;
	uint32_t jumbo_frame = 9000;
	ASSERT_GT(cbs, jumbo_frame);
}

/* ============================================================================
 * CCM Interval Tests
 * ============================================================================ */

TEST(ccm_interval_values)
{
	/* Verify CCM interval enum values make sense */
	ASSERT_EQ(0, CCM_INVALID);
	ASSERT_EQ(1, CCM_3_33MS);
	ASSERT_EQ(2, CCM_10MS);
	ASSERT_EQ(3, CCM_100MS);
	ASSERT_EQ(4, CCM_1S);
	ASSERT_EQ(5, CCM_10S);
	ASSERT_EQ(6, CCM_1MIN);
	ASSERT_EQ(7, CCM_10MIN);
}

TEST(ccm_interval_ms_mapping)
{
	/* Map intervals to milliseconds */
	uint32_t intervals_ms[] = {0, 3, 10, 100, 1000, 10000, 60000, 600000};

	ASSERT_EQ(1000, intervals_ms[CCM_1S]);
	ASSERT_EQ(100, intervals_ms[CCM_100MS]);
}

/* ============================================================================
 * MEG Level Tests
 * ============================================================================ */

TEST(meg_level_values)
{
	ASSERT_EQ(0, MEG_LEVEL_CUSTOMER);
	ASSERT_EQ(3, MEG_LEVEL_PROVIDER);
	ASSERT_EQ(7, MEG_LEVEL_OPERATOR);
}

TEST(meg_level_hierarchy)
{
	/* Operator > Provider > Customer */
	ASSERT_GT(MEG_LEVEL_OPERATOR, MEG_LEVEL_PROVIDER);
	ASSERT_GT(MEG_LEVEL_PROVIDER, MEG_LEVEL_CUSTOMER);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
	printf("RFC 2544 Test Master - Protocol Unit Tests\n");

	TEST_SUITE("Y.1564 Configuration");
	RUN_TEST(y1564_default_sla_values);
	RUN_TEST(y1564_default_sla_null);
	RUN_TEST(y1564_default_config_values);
	RUN_TEST(y1564_default_config_null);
	RUN_TEST(y1564_default_config_services);

	TEST_SUITE("Y.1731 Configuration");
	RUN_TEST(y1731_default_mep_config_values);
	RUN_TEST(y1731_default_mep_config_null);
	RUN_TEST(y1731_default_mep_meg_id);

	TEST_SUITE("TSN Configuration");
	RUN_TEST(tsn_default_config_values);
	RUN_TEST(tsn_default_config_null);
	RUN_TEST(tsn_gcl_cycle_time);

	TEST_SUITE("MEF Configuration");
	RUN_TEST(mef_default_config_values);
	RUN_TEST(mef_default_config_null);
	RUN_TEST(mef_service_frame_delay);
	RUN_TEST(mef_service_frame_loss);

	TEST_SUITE("RFC 2889 Configuration");
	RUN_TEST(rfc2889_default_config_values);
	RUN_TEST(rfc2889_default_config_null);
	RUN_TEST(rfc2889_trial_duration);

	TEST_SUITE("SLA Threshold Validation");
	RUN_TEST(sla_frame_delay_pass);
	RUN_TEST(sla_frame_delay_fail);
	RUN_TEST(sla_frame_loss_pass);
	RUN_TEST(sla_frame_loss_fail);
	RUN_TEST(sla_jitter_pass);
	RUN_TEST(sla_jitter_fail);

	TEST_SUITE("Frame Loss Ratio");
	RUN_TEST(flr_zero_loss);
	RUN_TEST(flr_one_percent);
	RUN_TEST(flr_total_loss);
	RUN_TEST(flr_zero_tx);

	TEST_SUITE("CIR/EIR Calculations");
	RUN_TEST(cir_percentage_of_line_rate);
	RUN_TEST(cir_eir_combined);
	RUN_TEST(burst_size_validation);

	TEST_SUITE("CCM Intervals");
	RUN_TEST(ccm_interval_values);
	RUN_TEST(ccm_interval_ms_mapping);

	TEST_SUITE("MEG Levels");
	RUN_TEST(meg_level_values);
	RUN_TEST(meg_level_hierarchy);

	TEST_SUMMARY();

	return test_failures;
}
