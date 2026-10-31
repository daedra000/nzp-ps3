/*
ps3_https.h: HTTP(S) GET over mbedTLS (ps3_https.c).
*/
#ifndef PS3_HTTPS_H
#define PS3_HTTPS_H

// takes each piece of the body; return 0 to stop
typedef int (*http_sink_t)(void *ctx, const void *data, int len);

// follows redirects; fills err (and returns 0) on failure
int PS3_HttpGet(const char *url, http_sink_t sink, void *ctx, volatile unsigned long long *got,
	volatile unsigned long long *total, volatile int *cancel, char *err, int errsize);

void PS3_Http_Init(void);	// at start-up: the lock between the downloader and the score sender

// GET or POST (body != NULL); with status_out, a non-200's body is read too
int PS3_HttpRequest(const char *method, const char *url, const char *body, int body_len,
	const char *ctype, http_sink_t sink, void *ctx, volatile unsigned long long *got,
	volatile unsigned long long *total, volatile int *cancel, char *err, int errsize, int *status_out);

#endif
