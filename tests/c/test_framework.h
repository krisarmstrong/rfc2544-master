/*
 * test_framework.h - Minimal C Unit Test Framework
 *
 * Single-header test framework for RFC 2544 Test Master.
 * No external dependencies - just include and use.
 *
 * Usage:
 *   TEST(test_name) {
 *       ASSERT_EQ(expected, actual);
 *       ASSERT_TRUE(condition);
 *   }
 *
 *   int main(void) {
 *       RUN_TEST(test_name);
 *       TEST_SUMMARY();
 *       return test_failures;
 *   }
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test counters */
static int test_count = 0;
static int test_failures = 0;
static int test_assertions = 0;
static const char *current_test = NULL;

/* Colors for terminal output (prefixed to avoid collision with rfc2544.h) */
#define TF_RED "\033[0;31m"
#define TF_GREEN "\033[0;32m"
#define TF_YELLOW "\033[0;33m"
#define TF_RESET "\033[0m"

/* Test definition macro */
#define TEST(name) static void test_##name(void)

/* Run a test */
#define RUN_TEST(name)                                                                             \
	do {                                                                                           \
		current_test = #name;                                                                      \
		int prev_failures = test_failures;                                                         \
		test_count++;                                                                              \
		test_##name();                                                                             \
		if (test_failures == prev_failures) {                                                      \
			printf(TF_GREEN "  ✓ " TF_RESET "%s\n", #name);                                        \
		}                                                                                          \
	} while (0)

/* Print test summary */
#define TEST_SUMMARY()                                                                             \
	do {                                                                                           \
		printf("\n");                                                                              \
		if (test_failures == 0) {                                                                  \
			printf(TF_GREEN "All %d tests passed" TF_RESET " (%d assertions)\n", test_count,       \
			       test_assertions);                                                               \
		} else {                                                                                   \
			printf(TF_RED "%d of %d tests failed" TF_RESET " (%d assertions)\n",                   \
			       test_failures, test_count, test_assertions);                                    \
		}                                                                                          \
	} while (0)

/* Test suite header */
#define TEST_SUITE(name) printf("\n" TF_YELLOW "=== %s ===" TF_RESET "\n", name)

/* Assertion failure helper */
#define FAIL_TEST(fmt, ...)                                                                        \
	do {                                                                                           \
		printf(TF_RED "  ✗ " TF_RESET "%s\n", current_test);                                       \
		printf("    " TF_RED "FAILED" TF_RESET " at %s:%d: " fmt "\n", __FILE__, __LINE__,         \
		       ##__VA_ARGS__);                                                                     \
		test_failures++;                                                                           \
	} while (0)

/* Basic assertions */
#define ASSERT_TRUE(expr)                                                                          \
	do {                                                                                           \
		test_assertions++;                                                                         \
		if (!(expr)) {                                                                             \
			FAIL_TEST("Expected true: %s", #expr);                                                 \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_FALSE(expr)                                                                         \
	do {                                                                                           \
		test_assertions++;                                                                         \
		if (expr) {                                                                                \
			FAIL_TEST("Expected false: %s", #expr);                                                \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_NULL(ptr)                                                                           \
	do {                                                                                           \
		test_assertions++;                                                                         \
		if ((ptr) != NULL) {                                                                       \
			FAIL_TEST("Expected NULL: %s", #ptr);                                                  \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_NOT_NULL(ptr)                                                                       \
	do {                                                                                           \
		test_assertions++;                                                                         \
		if ((ptr) == NULL) {                                                                       \
			FAIL_TEST("Expected not NULL: %s", #ptr);                                              \
			return;                                                                                \
		}                                                                                          \
	} while (0)

/* Integer assertions */
#define ASSERT_EQ(expected, actual)                                                                \
	do {                                                                                           \
		test_assertions++;                                                                         \
		long long _exp = (long long)(expected);                                                    \
		long long _act = (long long)(actual);                                                      \
		if (_exp != _act) {                                                                        \
			FAIL_TEST("Expected %lld, got %lld", _exp, _act);                                      \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_NE(expected, actual)                                                                \
	do {                                                                                           \
		test_assertions++;                                                                         \
		long long _exp = (long long)(expected);                                                    \
		long long _act = (long long)(actual);                                                      \
		if (_exp == _act) {                                                                        \
			FAIL_TEST("Expected not equal to %lld", _exp);                                         \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_GT(val, threshold)                                                                  \
	do {                                                                                           \
		test_assertions++;                                                                         \
		long long _v = (long long)(val);                                                           \
		long long _t = (long long)(threshold);                                                     \
		if (_v <= _t) {                                                                            \
			FAIL_TEST("Expected %lld > %lld", _v, _t);                                             \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_GE(val, threshold)                                                                  \
	do {                                                                                           \
		test_assertions++;                                                                         \
		long long _v = (long long)(val);                                                           \
		long long _t = (long long)(threshold);                                                     \
		if (_v < _t) {                                                                             \
			FAIL_TEST("Expected %lld >= %lld", _v, _t);                                            \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_LT(val, threshold)                                                                  \
	do {                                                                                           \
		test_assertions++;                                                                         \
		long long _v = (long long)(val);                                                           \
		long long _t = (long long)(threshold);                                                     \
		if (_v >= _t) {                                                                            \
			FAIL_TEST("Expected %lld < %lld", _v, _t);                                             \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_LE(val, threshold)                                                                  \
	do {                                                                                           \
		test_assertions++;                                                                         \
		long long _v = (long long)(val);                                                           \
		long long _t = (long long)(threshold);                                                     \
		if (_v > _t) {                                                                             \
			FAIL_TEST("Expected %lld <= %lld", _v, _t);                                            \
			return;                                                                                \
		}                                                                                          \
	} while (0)

/* Floating point assertions (with epsilon tolerance) */
#define ASSERT_FLOAT_EQ(expected, actual, epsilon)                                                 \
	do {                                                                                           \
		test_assertions++;                                                                         \
		double _exp = (double)(expected);                                                          \
		double _act = (double)(actual);                                                            \
		double _eps = (double)(epsilon);                                                           \
		if (fabs(_exp - _act) > _eps) {                                                            \
			FAIL_TEST("Expected %.6f, got %.6f (epsilon=%.6f)", _exp, _act, _eps);                 \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_FLOAT_GT(val, threshold)                                                            \
	do {                                                                                           \
		test_assertions++;                                                                         \
		double _v = (double)(val);                                                                 \
		double _t = (double)(threshold);                                                           \
		if (_v <= _t) {                                                                            \
			FAIL_TEST("Expected %.6f > %.6f", _v, _t);                                             \
			return;                                                                                \
		}                                                                                          \
	} while (0)

/* String assertions */
#define ASSERT_STR_EQ(expected, actual)                                                            \
	do {                                                                                           \
		test_assertions++;                                                                         \
		if (strcmp((expected), (actual)) != 0) {                                                   \
			FAIL_TEST("Expected \"%s\", got \"%s\"", (expected), (actual));                        \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#define ASSERT_STR_NE(expected, actual)                                                            \
	do {                                                                                           \
		test_assertions++;                                                                         \
		if (strcmp((expected), (actual)) == 0) {                                                   \
			FAIL_TEST("Expected string different from \"%s\"", (expected));                        \
			return;                                                                                \
		}                                                                                          \
	} while (0)

/* Memory assertions */
#define ASSERT_MEM_EQ(expected, actual, len)                                                       \
	do {                                                                                           \
		test_assertions++;                                                                         \
		if (memcmp((expected), (actual), (len)) != 0) {                                            \
			FAIL_TEST("Memory regions differ (length=%zu)", (size_t)(len));                        \
			return;                                                                                \
		}                                                                                          \
	} while (0)

/* Range assertion */
#define ASSERT_IN_RANGE(val, min, max)                                                             \
	do {                                                                                           \
		test_assertions++;                                                                         \
		double _v = (double)(val);                                                                 \
		double _min = (double)(min);                                                               \
		double _max = (double)(max);                                                               \
		if (_v < _min || _v > _max) {                                                              \
			FAIL_TEST("Expected %.6f in range [%.6f, %.6f]", _v, _min, _max);                      \
			return;                                                                                \
		}                                                                                          \
	} while (0)

#endif /* TEST_FRAMEWORK_H */
