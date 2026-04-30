#include <unity.h>
#include <string.h>
#include "attention.h"

void setUp(void) {}
void tearDown(void) {}

// ---- parseAttentionJson --------------------------------------------------

void test_parse_valid_working() {
  const char* body = R"({
    "ts": 1745673120, "state": "WORKING",
    "cwd": "/Users/rock/code/foo", "session_id": "abc-123"
  })";
  AttentionState a;
  TEST_ASSERT_TRUE(parseAttentionJson(body, a));
  TEST_ASSERT_EQUAL(ATTN_WORKING, a.kind);
  TEST_ASSERT_EQUAL_STRING("/Users/rock/code/foo", a.cwd);
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL_UINT32(0, a.since_ms);  // parser must NOT stamp this; handler does
}

void test_parse_done_waiting_idle() {
  AttentionState a;
  TEST_ASSERT_TRUE(parseAttentionJson(R"({"ts":1,"state":"DONE"})", a));
  TEST_ASSERT_EQUAL(ATTN_DONE, a.kind);
  TEST_ASSERT_TRUE(parseAttentionJson(R"({"ts":1,"state":"WAITING"})", a));
  TEST_ASSERT_EQUAL(ATTN_WAITING, a.kind);
  TEST_ASSERT_TRUE(parseAttentionJson(R"({"ts":1,"state":"IDLE"})", a));
  TEST_ASSERT_EQUAL(ATTN_IDLE, a.kind);
}

void test_parse_missing_ts_rejected() {
  AttentionState a;
  TEST_ASSERT_FALSE(parseAttentionJson(R"({"state":"WORKING"})", a));
}

void test_parse_missing_state_rejected() {
  AttentionState a;
  TEST_ASSERT_FALSE(parseAttentionJson(R"({"ts":1})", a));
}

void test_parse_unknown_state_rejected() {
  AttentionState a;
  TEST_ASSERT_FALSE(parseAttentionJson(R"({"ts":1,"state":"BOGUS"})", a));
}

void test_parse_lowercase_state_rejected() {
  AttentionState a;
  TEST_ASSERT_FALSE(parseAttentionJson(R"({"ts":1,"state":"working"})", a));
}

void test_parse_missing_cwd_ok() {
  AttentionState a;
  TEST_ASSERT_TRUE(parseAttentionJson(R"({"ts":1,"state":"WORKING"})", a));
  TEST_ASSERT_EQUAL(ATTN_WORKING, a.kind);
  TEST_ASSERT_EQUAL_STRING("", a.cwd);
}

void test_parse_long_cwd_truncated() {
  // 80 'a' characters in cwd; should truncate to 63 + null.
  const char* body = R"({"ts":1,"state":"WORKING","cwd":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
  AttentionState a;
  TEST_ASSERT_TRUE(parseAttentionJson(body, a));
  TEST_ASSERT_EQUAL_INT(63, (int)strlen(a.cwd));
  for (int i = 0; i < 63; ++i) TEST_ASSERT_EQUAL_CHAR('a', a.cwd[i]);
}

// ---- attentionTick --------------------------------------------------------

void test_tick_within_timeout_no_change() {
  AttentionState s;
  s.kind = ATTN_WORKING;
  s.since_ms = 1000;
  TEST_ASSERT_FALSE(attentionTick(s, 1000 + 14UL*60*1000));
  TEST_ASSERT_EQUAL(ATTN_WORKING, s.kind);
}

void test_tick_past_timeout_goes_idle() {
  AttentionState s;
  s.kind = ATTN_DONE;
  s.since_ms = 1000;
  s.cwd[0] = 'x'; s.cwd[1] = '\0';   // seed a non-empty cwd
  uint32_t now = 1000 + 16UL*60*1000;
  TEST_ASSERT_TRUE(attentionTick(s, now));
  TEST_ASSERT_EQUAL(ATTN_IDLE, s.kind);
  TEST_ASSERT_EQUAL_UINT32(now, s.since_ms);  // since_ms must be updated to now_ms
  TEST_ASSERT_EQUAL_STRING("", s.cwd);         // cwd must be cleared
}

void test_tick_idle_never_transitions() {
  AttentionState s;  // default ATTN_IDLE
  s.since_ms = 1000;
  TEST_ASSERT_FALSE(attentionTick(s, 1000 + 999UL*60*1000));
  TEST_ASSERT_EQUAL(ATTN_IDLE, s.kind);
}

void test_tick_handles_millis_rollover() {
  // since_ms close to uint32 max; now_ms wrapped past zero by ~2 minutes.
  // (uint32_t)(now - since) = (uint32_t)(0x00010000 - 0xFFFF0000) = 0x00020000
  // = 131072 ms ≈ 2 min. Still under the 15 min threshold → no change.
  AttentionState s;
  s.kind = ATTN_WAITING;
  s.since_ms = 0xFFFF0000UL;
  TEST_ASSERT_FALSE(attentionTick(s, 0x00010000UL));
  TEST_ASSERT_EQUAL(ATTN_WAITING, s.kind);
}

int runUnityTests(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_valid_working);
  RUN_TEST(test_parse_done_waiting_idle);
  RUN_TEST(test_parse_missing_ts_rejected);
  RUN_TEST(test_parse_missing_state_rejected);
  RUN_TEST(test_parse_unknown_state_rejected);
  RUN_TEST(test_parse_lowercase_state_rejected);
  RUN_TEST(test_parse_missing_cwd_ok);
  RUN_TEST(test_parse_long_cwd_truncated);
  RUN_TEST(test_tick_within_timeout_no_change);
  RUN_TEST(test_tick_past_timeout_goes_idle);
  RUN_TEST(test_tick_idle_never_transitions);
  RUN_TEST(test_tick_handles_millis_rollover);
  return UNITY_END();
}

int main(int, char**) { return runUnityTests(); }
