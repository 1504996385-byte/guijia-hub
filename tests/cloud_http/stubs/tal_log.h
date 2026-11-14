#ifndef HTTP_TEST_LOG_H
#define HTTP_TEST_LOG_H
void http_fake_log(const char *format,...);
#define PR_ERR(...) http_fake_log(__VA_ARGS__)
#define PR_WARN(...) http_fake_log(__VA_ARGS__)
#define PR_INFO(...) ((void)0)
#define PR_DEBUG(...) ((void)0)
#define PR_TRACE(...) ((void)0)
#endif
