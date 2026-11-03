#ifndef TEST_LIVEAREA_LOGGER_H
#define TEST_LIVEAREA_LOGGER_H
void test_log(const char *format, ...);
#define l_info(...) test_log(__VA_ARGS__)
#define l_warn(...) test_log(__VA_ARGS__)
#endif
