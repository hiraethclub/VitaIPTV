/* Unit tests for the core logging ring buffer. Runs natively. */
#include <stdio.h>
#include <string.h>

#include "vi_log.h"

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, msg) do {                                   \
    g_checks++;                                                 \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* sink capture */
static char last_sink_line[256];
static int sink_calls;
static void test_sink(void *ctx, vi_log_level level, const char *line)
{
    (void)ctx; (void)level;
    strncpy(last_sink_line, line, sizeof(last_sink_line) - 1);
    last_sink_line[sizeof(last_sink_line) - 1] = '\0';
    sink_calls++;
}

static void test_basic_and_ring(void)
{
    char buf[256];
    vi_log_level lvl;

    printf("test_basic_and_ring\n");
    vi_log_init(VI_LOG_INFO);
    VI_LOGI("hls", "hello %d", 42);
    CHECK(vi_log_ring_count() == 1, "one line");
    CHECK(vi_log_ring_copy(0, buf, sizeof(buf), &lvl) == 1, "copy ok");
    CHECK(strcmp(buf, "I/hls: hello 42") == 0, "formatted line");
    CHECK(lvl == VI_LOG_INFO, "level captured");
    CHECK(vi_log_total() == 1, "total 1");
}

static void test_level_filter(void)
{
    printf("test_level_filter\n");
    vi_log_init(VI_LOG_WARN);
    VI_LOGI("t", "info suppressed");
    VI_LOGD("t", "debug suppressed");
    VI_LOGE("t", "error kept");
    VI_LOGW("t", "warn kept");
    CHECK(vi_log_ring_count() == 2, "only error+warn kept");
    CHECK(vi_log_total() == 2, "total counts only emitted");
}

static void test_ring_wrap(void)
{
    char buf[256];
    int i;

    printf("test_ring_wrap\n");
    vi_log_init(VI_LOG_TRACE);
    for (i = 0; i < 100; i++)
        VI_LOGI("n", "line %d", i);
    /* ring holds 64; oldest retained should be line 36 (100-64) */
    CHECK(vi_log_ring_count() == 64, "ring capped at 64");
    CHECK(vi_log_total() == 100, "total is 100");
    vi_log_ring_copy(0, buf, sizeof(buf), NULL);
    CHECK(strcmp(buf, "I/n: line 36") == 0, "oldest is line 36");
    vi_log_ring_copy(63, buf, sizeof(buf), NULL);
    CHECK(strcmp(buf, "I/n: line 99") == 0, "newest is line 99");
    CHECK(vi_log_ring_copy(64, buf, sizeof(buf), NULL) == 0, "past end -> 0");
}

static void test_sink_called(void)
{
    printf("test_sink_called\n");
    vi_log_init(VI_LOG_INFO);
    sink_calls = 0;
    last_sink_line[0] = '\0';
    vi_log_set_sink(test_sink, NULL);
    VI_LOGI("net", "connect %s", "ok");
    CHECK(sink_calls == 1, "sink called once");
    CHECK(strcmp(last_sink_line, "I/net: connect ok") == 0, "sink got line");
    VI_LOGD("net", "suppressed");   /* below INFO */
    CHECK(sink_calls == 1, "sink not called for filtered");
    vi_log_set_sink(NULL, NULL);
}

int main(void)
{
    test_basic_and_ring();
    test_level_filter();
    test_ring_wrap();
    test_sink_called();
    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
