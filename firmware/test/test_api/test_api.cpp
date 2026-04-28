#include <unity.h>
#include "api.h"

void setUp(void) {}
void tearDown(void) {}

void test_valid_payload() {
  const char* body = R"({
    "ts": 1777735800, "plan": "Max 5x",
    "block_5h": {"used_tokens": 1017862, "started_at": 1777734000, "resets_at": 1777752000,
                 "cost_usd": 1.81, "messages": 11, "burn_rate_tpm": 82684},
    "weekly":   {"used_tokens": 5400000, "started_at": 1777718313, "resets_at": 1778323113,
                 "cost_usd": 23.69, "messages": 47},
    "today":    {"tokens": 2100000, "messages": 14, "cost_usd": 5.62}
  })";
  UsageData u;
  TEST_ASSERT_TRUE(parseUsageJson(body, u));
  TEST_ASSERT_EQUAL_UINT32(1777735800, u.ts);
  TEST_ASSERT_EQUAL_STRING("Max 5x", u.plan);
  TEST_ASSERT_EQUAL_UINT64(1017862, u.tok_5h);
  TEST_ASSERT_EQUAL_UINT32(1777734000, u.started_5h);
  TEST_ASSERT_EQUAL_UINT32(1777752000, u.reset_5h);
  TEST_ASSERT_EQUAL_UINT32(82684, u.burn_tpm);
  TEST_ASSERT_TRUE(u.burn_present);
  TEST_ASSERT_EQUAL_UINT64(5400000, u.tok_weekly);
  TEST_ASSERT_EQUAL_UINT32(1777718313, u.started_weekly);
  TEST_ASSERT_EQUAL_UINT32(1778323113, u.reset_weekly);
  TEST_ASSERT_TRUE(u.today_present);
  TEST_ASSERT_EQUAL_UINT64(2100000, u.tok_today);
}

void test_missing_today_is_ok() {
  const char* body = R"({
    "ts": 1, "plan": "x",
    "block_5h":{"used_tokens":0,"started_at":0,"resets_at":0},
    "weekly":  {"used_tokens":0,"started_at":0,"resets_at":0}
  })";
  UsageData u;
  TEST_ASSERT_TRUE(parseUsageJson(body, u));
  TEST_ASSERT_FALSE(u.today_present);
  TEST_ASSERT_FALSE(u.burn_present);
  TEST_ASSERT_FALSE(u.cost_5h_present);
}

void test_missing_burn_is_ok() {
  const char* body = R"({
    "ts":1,"plan":"x",
    "block_5h":{"used_tokens":42000,"started_at":1,"resets_at":2,"cost_usd":0.08,"messages":1},
    "weekly":  {"used_tokens":42000,"started_at":1,"resets_at":2}
  })";
  UsageData u;
  TEST_ASSERT_TRUE(parseUsageJson(body, u));
  TEST_ASSERT_FALSE(u.burn_present);
  TEST_ASSERT_TRUE(u.cost_5h_present);
  TEST_ASSERT_TRUE(u.msgs_5h_present);
}

void test_malformed_rejects() {
  UsageData u;
  TEST_ASSERT_FALSE(parseUsageJson("not json", u));
  TEST_ASSERT_FALSE(parseUsageJson("{}", u));
  TEST_ASSERT_FALSE(parseUsageJson(
    R"({"ts":1,"plan":"x","block_5h":{"used_tokens":0,"resets_at":0},"weekly":{"used_tokens":0,"started_at":0,"resets_at":0}})",
    u));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_payload);
  RUN_TEST(test_missing_today_is_ok);
  RUN_TEST(test_missing_burn_is_ok);
  RUN_TEST(test_malformed_rejects);
  return UNITY_END();
}
