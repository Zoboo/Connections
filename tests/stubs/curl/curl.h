#pragma once
typedef struct { int unused; } CURL;
typedef int CURLcode;
typedef long long curl_off_t;
#define CURLE_OK 0
#define CURL_GLOBAL_DEFAULT 0
#define CURLPROTO_HTTPS 2
enum {
    CURLOPT_URL, CURLOPT_USERAGENT, CURLOPT_PROTOCOLS, CURLOPT_REDIR_PROTOCOLS,
    CURLOPT_FOLLOWLOCATION, CURLOPT_MAXREDIRS, CURLOPT_FAILONERROR,
    CURLOPT_SSL_VERIFYPEER, CURLOPT_SSL_VERIFYHOST, CURLOPT_CONNECTTIMEOUT,
    CURLOPT_TIMEOUT, CURLOPT_LOW_SPEED_LIMIT, CURLOPT_LOW_SPEED_TIME,
    CURLOPT_NOSIGNAL, CURLOPT_MAXFILESIZE_LARGE, CURLOPT_WRITEFUNCTION,
    CURLOPT_WRITEDATA, CURLOPT_HEADERFUNCTION, CURLOPT_HEADERDATA, CURLOPT_NOPROGRESS,
    CURLOPT_XFERINFOFUNCTION, CURLOPT_ACCEPT_ENCODING, CURLOPT_RANGE, CURLINFO_RESPONSE_CODE
};
static int curl_global_init(long flags) { (void)flags; return 0; }
static void curl_global_cleanup(void) {}
static CURL *curl_easy_init(void) { static CURL curl; return &curl; }
static void curl_easy_cleanup(CURL *curl) { (void)curl; }
static void curl_easy_reset(CURL *curl) { (void)curl; }
static int curl_easy_setopt(CURL *curl, int key, ...) { (void)curl; (void)key; return 0; }
static int curl_easy_perform(CURL *curl) { (void)curl; return 1; }
static void curl_easy_getinfo(CURL *curl, int key, long *out) { (void)curl; (void)key; *out = 503; }
