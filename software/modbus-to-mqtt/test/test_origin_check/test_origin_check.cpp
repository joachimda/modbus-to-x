#include <Arduino.h>
#include <unity.h>

#include "network/mbx_server/OriginCheck.h"

void setUp(void) {}
void tearDown(void) {}

void test_empty_origin_allowed() {
    TEST_ASSERT_TRUE(isOriginAllowed("", "192.168.1.50"));
}

void test_null_origin_rejected() {
    TEST_ASSERT_FALSE(isOriginAllowed("null", "192.168.1.50"));
}

void test_ip_match_allowed() {
    TEST_ASSERT_TRUE(isOriginAllowed("http://192.168.1.50", "192.168.1.50"));
}

void test_default_port_normalized_origin_side() {
    TEST_ASSERT_TRUE(isOriginAllowed("http://192.168.1.50:80", "192.168.1.50"));
}

void test_default_port_normalized_host_side() {
    TEST_ASSERT_TRUE(isOriginAllowed("http://192.168.1.50", "192.168.1.50:80"));
}

void test_real_port_mismatch_rejected() {
    TEST_ASSERT_FALSE(isOriginAllowed("http://192.168.1.50:8080", "192.168.1.50"));
}

void test_hostname_match_allowed() {
    TEST_ASSERT_TRUE(isOriginAllowed("http://gateway.local", "gateway.local"));
}

void test_https_scheme_ignored() {
    TEST_ASSERT_TRUE(isOriginAllowed("https://gateway.local", "gateway.local"));
}

void test_ap_ip_allowed() {
    TEST_ASSERT_TRUE(isOriginAllowed("http://4.3.2.1", "4.3.2.1"));
}

void test_cross_origin_rejected() {
    TEST_ASSERT_FALSE(isOriginAllowed("http://evil.example", "192.168.1.50"));
}

void test_case_insensitive_hostname() {
    TEST_ASSERT_TRUE(isOriginAllowed("http://Gateway.Local", "gateway.local"));
}

void test_subdomain_rejected() {
    TEST_ASSERT_FALSE(isOriginAllowed("http://attacker.gateway.local", "gateway.local"));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_origin_allowed);
    RUN_TEST(test_null_origin_rejected);
    RUN_TEST(test_ip_match_allowed);
    RUN_TEST(test_default_port_normalized_origin_side);
    RUN_TEST(test_default_port_normalized_host_side);
    RUN_TEST(test_real_port_mismatch_rejected);
    RUN_TEST(test_hostname_match_allowed);
    RUN_TEST(test_https_scheme_ignored);
    RUN_TEST(test_ap_ip_allowed);
    RUN_TEST(test_cross_origin_rejected);
    RUN_TEST(test_case_insensitive_hostname);
    RUN_TEST(test_subdomain_rejected);
    UNITY_END();
}
