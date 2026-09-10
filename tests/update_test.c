// Exercise the real release checker with controlled HTTP responses. No network,
// registry, NVIDIA process or saved update preferences are accessed.
#include "defines.h"
#include <curl/curl.h>
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

GlobalCb glbl = { .loglock = PTHREAD_MUTEX_INITIALIZER };
char *GetDateTimeStaticStr(void) { return "test"; }

static struct
{
    const char *body;
    size_t length;
    long httpCode;
    CURLcode result;
    BOOL optionFails, infoFails, writeRejected;
    BOOL url, userAgent, headers, nativeCa, noCaFile, noCaPath, https, httpsRedirect;
    BOOL fallback, headOnly;
    long fallbackHttpCode;
    CURLcode fallbackResult;
    const char *redirectUrl;
    unsigned requests;
    size_t (*receive)(char *, size_t, size_t, void *);
    void *context;
} env;

static CURLcode FakeSetopt(CURL *handle, CURLoption option, ...)
{
    assert(handle);
    if (env.optionFails) return CURLE_UNKNOWN_OPTION;
    va_list args;
    va_start(args, option);
    switch (option) {
    case CURLOPT_URL: {
        const char *url = va_arg(args, const char *);
        env.fallback = strcmp(url, "https://github.com/" GITHUB_NAME_WITH_OWNER "/releases/latest") == 0;
        env.url = env.fallback || strcmp(url,
            "https://api.github.com/repos/" GITHUB_NAME_WITH_OWNER "/releases/latest") == 0;
        break;
    }
    case CURLOPT_USERAGENT:
        env.userAgent = strcmp(va_arg(args, const char *), "AlwaysShadow/" ALWAYSSHADOW_VERSION) == 0;
        break;
    case CURLOPT_HTTPHEADER: {
        const struct curl_slist *header = va_arg(args, struct curl_slist *);
        if (env.fallback) {
            assert(!header);
            env.headers = FALSE;
            break;
        }
        assert(header && strcmp(header->data, "Accept: application/vnd.github+json") == 0);
        assert(header->next && strcmp(header->next->data, "X-GitHub-Api-Version: 2022-11-28") == 0);
        assert(!header->next->next); // No authentication or machine-specific data.
        env.headers = TRUE;
        break;
    }
    case CURLOPT_CONNECTTIMEOUT:
    case CURLOPT_TIMEOUT: {
        const long timeout = va_arg(args, long);
        assert(timeout > 0 && timeout <= 10);
        break;
    }
    case CURLOPT_FOLLOWLOCATION:
        assert(va_arg(args, long) == 1);
        break;
    case CURLOPT_NOBODY:
        env.headOnly = va_arg(args, long) == 1;
        break;
    case CURLOPT_MAXREDIRS: {
        const long redirects = va_arg(args, long);
        assert(redirects > 0 && redirects <= 5);
        break;
    }
    case CURLOPT_PROTOCOLS_STR:
        env.https = strcmp(va_arg(args, const char *), "https") == 0;
        break;
    case CURLOPT_REDIR_PROTOCOLS_STR:
        env.httpsRedirect = strcmp(va_arg(args, const char *), "https") == 0;
        break;
    case CURLOPT_SSL_OPTIONS:
        env.nativeCa = va_arg(args, long) == CURLSSLOPT_NATIVE_CA;
        break;
    case CURLOPT_CAINFO:
        env.noCaFile = va_arg(args, const char *) == NULL;
        break;
    case CURLOPT_CAPATH:
        env.noCaPath = va_arg(args, const char *) == NULL;
        break;
    case CURLOPT_WRITEFUNCTION:
        env.receive = va_arg(args, size_t (*)(char *, size_t, size_t, void *));
        break;
    case CURLOPT_WRITEDATA:
        env.context = va_arg(args, void *);
        break;
    default:
        assert(FALSE);
    }
    va_end(args);
    return CURLE_OK;
}

static CURLcode FakePerform(CURL *handle)
{
    assert(handle && env.receive && env.context);
    assert(env.url && env.userAgent && env.nativeCa);
    assert(env.noCaFile && env.noCaPath && env.https && env.httpsRedirect);
    ++env.requests;
    if (env.fallback) {
        assert(env.headOnly && !env.headers);
        return env.fallbackResult;
    }
    assert(env.headers && !env.headOnly);
    if (env.result != CURLE_OK) return env.result;
    for (size_t offset = 0; offset < env.length;) {
        const size_t count = env.length - offset > 37 ? 37 : env.length - offset;
        if (env.receive((char *)env.body + offset, 1, count, env.context) != count) {
            env.writeRejected = TRUE;
            return CURLE_WRITE_ERROR;
        }
        offset += count;
    }
    return CURLE_OK;
}

static CURLcode FakeGetinfo(CURL *handle, CURLINFO info, ...)
{
    assert(handle);
    if (env.infoFails) return CURLE_BAD_FUNCTION_ARGUMENT;
    va_list args;
    va_start(args, info);
    if (info == CURLINFO_RESPONSE_CODE)
        *va_arg(args, long *) = env.fallback ? env.fallbackHttpCode : env.httpCode;
    else {
        assert(info == CURLINFO_EFFECTIVE_URL && env.fallback);
        *va_arg(args, const char **) = env.redirectUrl;
    }
    va_end(args);
    return CURLE_OK;
}

#undef curl_easy_setopt
#undef curl_easy_getinfo
#define curl_easy_setopt FakeSetopt
#define curl_easy_perform FakePerform
#define curl_easy_getinfo FakeGetinfo
#include "../src/update.c"

static void ResetResponse(const char *body)
{
    memset(&env, 0, sizeof(env));
    env.body = body;
    env.length = strlen(body);
    env.httpCode = 200;
    env.fallbackHttpCode = 200;
    env.redirectUrl = "https://github.com/" GITHUB_NAME_WITH_OWNER "/releases/tag/v2.4";
}

static void ExpectVersion(const char *current, const char *latest, BOOL expected)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"tag_name\":\"%s\",\"draft\":false,\"prerelease\":false,\"body\":\"Release notes\"}", latest);
    ResetResponse(body);
    BOOL available = TRUE;
    assert(CheckForReleaseUpdate(current, &available));
    assert(available == expected && env.requests == 1);
}

static void ExpectFailure(void)
{
    BOOL available = TRUE;
    assert(!CheckForReleaseUpdate("2.3", &available));
    assert(!available);
}

int main(void)
{
    glbl.logfile = tmpfile();
    assert(glbl.logfile);
    assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);

    ExpectVersion("2.3", "v2.3", FALSE);
    ExpectVersion("2.3", "v2.3.0", FALSE);
    ExpectVersion("2.3", "v2.2", FALSE);
    ExpectVersion("2.3", "v2.3.1", TRUE);
    ExpectVersion("2.9", "v2.10", TRUE);
    ExpectVersion("2.10", "v2.9", FALSE);
    ExpectVersion("2.99", "v3.0", TRUE);
    ExpectVersion("3.0", "v2.999", FALSE);
    ExpectVersion("2", "2.0.0", FALSE);
    ExpectVersion("v2.3", "V2.4", TRUE);

    const char *invalid[] = {
        "", "not JSON", "[]", "{}", "{\"tag_name\":\"v2.4\"}",
        "{\"tag_name\":null,\"draft\":false,\"prerelease\":false}",
        "{\"tag_name\":24,\"draft\":false,\"prerelease\":false}",
        "{\"tag_name\":\"v2.4\",\"draft\":true,\"prerelease\":false}",
        "{\"tag_name\":\"v2.4\",\"draft\":false,\"prerelease\":true}",
        "{\"tag_name\":\"v2.4\",\"draft\":\"false\",\"prerelease\":false}",
        "{\"tag_name\":\"v2.4\",\"draft\":false,\"prerelease\":false}junk",
        "{\"tag_name\":\"v2.4\",\"draft\":false,\"prerelease\":false"
    };
    for (size_t i = 0; i < _countof(invalid); ++i) {
        ResetResponse(invalid[i]);
        ExpectFailure();
    }
    const char *invalidTags[] = {"", "v", "latest", "v2.4-rc1", "2..4", "2.4.", "2.4.1.2", "-2.4", "2.4 ", "4294967296.0"};
    for (size_t i = 0; i < _countof(invalidTags); ++i) {
        char body[256];
        snprintf(body, sizeof(body), "{\"tag_name\":\"%s\",\"draft\":false,\"prerelease\":false}", invalidTags[i]);
        ResetResponse(body);
        ExpectFailure();
    }

    const char *valid = "{\"tag_name\":\"v2.4\",\"draft\":false,\"prerelease\":false}";
    const long httpErrors[] = {301, 404, 500};
    for (size_t i = 0; i < _countof(httpErrors); ++i) {
        ResetResponse(valid);
        env.httpCode = httpErrors[i];
        ExpectFailure();
    }
    const long quotaErrors[] = {403, 429};
    for (size_t i = 0; i < _countof(quotaErrors); ++i) {
        ResetResponse("{\"message\":\"API rate limit exceeded\"}");
        env.httpCode = quotaErrors[i];
        BOOL available = FALSE;
        assert(CheckForReleaseUpdate("2.3", &available) && available);
        assert(env.requests == 2 && env.headOnly);
        ResetResponse(valid);
        env.httpCode = quotaErrors[i];
        assert(CheckForReleaseUpdate("2.4", &available) && !available);
        ResetResponse(valid);
        env.httpCode = quotaErrors[i];
        assert(CheckForReleaseUpdate("2.5", &available) && !available);
    }
    const char *badRedirects[] = {
        NULL, "https://github.com/login", "https://example.com/releases/tag/v2.4",
        "https://github.com/other/project/releases/tag/v2.4",
        "https://github.com/" GITHUB_NAME_WITH_OWNER "/releases/latest",
        "https://github.com/" GITHUB_NAME_WITH_OWNER "/releases/tag/v2.4-rc1"
    };
    for (size_t i = 0; i < _countof(badRedirects); ++i) {
        ResetResponse(valid);
        env.httpCode = 403;
        env.redirectUrl = badRedirects[i];
        ExpectFailure();
    }
    ResetResponse(valid);
    env.httpCode = 429;
    env.fallbackHttpCode = 404;
    ExpectFailure();
    ResetResponse(valid);
    env.httpCode = 403;
    env.fallbackResult = CURLE_OPERATION_TIMEDOUT;
    ExpectFailure();
    const CURLcode errors[] = {CURLE_OPERATION_TIMEDOUT, CURLE_COULDNT_RESOLVE_HOST, CURLE_PEER_FAILED_VERIFICATION};
    for (size_t i = 0; i < _countof(errors); ++i) {
        ResetResponse(valid);
        env.result = errors[i];
        ExpectFailure();
    }
    ResetResponse(valid);
    env.optionFails = TRUE;
    ExpectFailure();
    assert(env.requests == 0);
    ResetResponse(valid);
    env.infoFails = TRUE;
    ExpectFailure();

    char *oversized = malloc(RELEASE_RESPONSE_LIMIT + 2);
    assert(oversized);
    memset(oversized, 'x', RELEASE_RESPONSE_LIMIT + 1);
    oversized[RELEASE_RESPONSE_LIMIT + 1] = '\0';
    ResetResponse(oversized);
    ExpectFailure();
    assert(env.writeRejected);
    free(oversized);

    char buffer[16] = {0};
    ReleaseResponse response = {buffer, 0};
    assert(ReceiveRelease("abcdef", 2, 3, &response) == 6);
    assert(strcmp(buffer, "abcdef") == 0 && response.used == 6);
    assert(ReceiveRelease("x", SIZE_MAX, 2, &response) == 0);
    assert(response.used == 6);

    ResetResponse(valid);
    BOOL available = TRUE;
    assert(!CheckForReleaseUpdate("dev", &available) && !available);
    assert(env.requests == 0);
    assert(!CheckForReleaseUpdate(NULL, &available));
    assert(!CheckForReleaseUpdate("2.3", NULL));

    curl_global_cleanup();
    fclose(glbl.logfile);
    puts("Release update tests passed: numeric versions, stable JSON, API rate-limit fallback, HTTP/TLS failures, bounded responses and portable HTTPS settings.");
    return 0;
}
