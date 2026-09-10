#include "defines.h"
#include "update.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RELEASE_RESPONSE_LIMIT (256u * 1024u)

typedef struct
{
    char *data;
    size_t used;
} ReleaseResponse;

static size_t ReceiveRelease(char *data, size_t size, size_t count, void *context)
{
    ReleaseResponse *response = context;
    if (size && count > SIZE_MAX / size) return 0;
    const size_t bytes = size * count;
    if (bytes > RELEASE_RESPONSE_LIMIT - response->used) return 0;
    memcpy(response->data + response->used, data, bytes);
    response->used += bytes;
    response->data[response->used] = '\0';
    return bytes;
}

// Compare numeric release tags, treating omitted minor/patch parts as zero.
static BOOL ParseVersion(const char *text, uint32_t parts[3])
{
    memset(parts, 0, 3 * sizeof(*parts));
    if (!text) return FALSE;
    if (*text == 'v' || *text == 'V') ++text;
    for (unsigned part = 0; part < 3; ++part) {
        if (*text < '0' || *text > '9') return FALSE;
        do {
            const unsigned digit = (unsigned)(*text++ - '0');
            if (parts[part] > (UINT32_MAX - digit) / 10) return FALSE;
            parts[part] = parts[part] * 10 + digit;
        } while (*text >= '0' && *text <= '9');
        if (!*text) return TRUE;
        if (*text++ != '.') return FALSE;
    }
    return FALSE;
}

static BOOL CompareReleaseTag(const char *tag, const uint32_t current[3], BOOL *available)
{
    uint32_t latest[3];
    if (!ParseVersion(tag, latest)) return FALSE;
    for (unsigned part = 0; part < 3; ++part) {
        if (latest[part] == current[part]) continue;
        *available = latest[part] > current[part];
        break;
    }
    LOG("Latest GitHub release: %s; update available: %d", tag, *available);
    return TRUE;
}

static BOOL ReadRelease(const ReleaseResponse *response, const uint32_t current[3], BOOL *available)
{
    // Reject truncated/concatenated JSON and embedded NUL bytes in the body.
    if (!response->used || memchr(response->data, '\0', response->used)) return FALSE;
    cJSON *release = cJSON_ParseWithLengthOpts(response->data, response->used + 1, NULL, TRUE);
    if (!release) return FALSE;
    BOOL success = FALSE;
    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(release, "tag_name");
    if (!cJSON_IsObject(release) || !cJSON_IsString(tag) ||
        !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(release, "draft")) ||
        !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(release, "prerelease"))) goto cleanup;
    success = CompareReleaseTag(tag->valuestring, current, available);
cleanup:
    cJSON_Delete(release);
    return success;
}

static BOOL ReadReleaseRedirect(CURL *handle, const uint32_t current[3], BOOL *available)
{
    // GitHub's public latest-Release page selects the same stable release and
    // works without an API token when an anonymous/shared IP exhausts its quota.
    HANDLE_CURL_ERROR(failed, curl_easy_setopt(handle, CURLOPT_URL,
        "https://github.com/" GITHUB_NAME_WITH_OWNER "/releases/latest"), "set latest Release page");
    HANDLE_CURL_ERROR(failed, curl_easy_setopt(handle, CURLOPT_HTTPHEADER, NULL), "clear API headers");
    HANDLE_CURL_ERROR(failed, curl_easy_setopt(handle, CURLOPT_NOBODY, 1L), "request Release headers only");
    HANDLE_CURL_ERROR(failed, curl_easy_perform(handle), "resolve latest Release page");
    long httpCode = 0;
    char *url = NULL;
    HANDLE_CURL_ERROR(failed, curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &httpCode), "read Release page status");
    HANDLE_CURL_ERROR(failed, curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &url), "read Release destination");
    const char prefix[] = "https://github.com/" GITHUB_NAME_WITH_OWNER "/releases/tag/";
    if (httpCode != 200 || !url || strncmp(url, prefix, sizeof(prefix) - 1) != 0) {
        LOG_WARN("Latest Release page did not resolve to this repository's release (HTTP %ld).", httpCode);
        return FALSE;
    }
    return CompareReleaseTag(url + sizeof(prefix) - 1, current, available);
failed:
    return FALSE;
}

BOOL CheckForReleaseUpdate(const char *currentVersion, BOOL *available)
{
    if (!available) return FALSE;
    *available = FALSE;
    uint32_t current[3];
    if (!ParseVersion(currentVersion, current)) {
        LOG_WARN("Cannot check updates: invalid application version.");
        return FALSE;
    }

    BOOL success = FALSE;
    CURL *handle = curl_easy_init();
    struct curl_slist *headers = NULL;
    ReleaseResponse response = {0};
    if (!handle) goto cleanup;
    response.data = malloc(RELEASE_RESPONSE_LIMIT + 1);
    if (!response.data) goto cleanup;
    response.data[0] = '\0';
    headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    if (!headers) goto cleanup;
    struct curl_slist *updated = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    if (!updated) goto cleanup;
    headers = updated;

    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_URL,
        "https://api.github.com/repos/" GITHUB_NAME_WITH_OWNER "/releases/latest"), "set release URL");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_USERAGENT,
        "AlwaysShadow/" ALWAYSSHADOW_VERSION), "set GitHub user agent");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers), "set GitHub headers");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 3L), "set connection timeout");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_TIMEOUT, 5L), "set request timeout");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L), "allow repository redirects");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 3L), "limit redirects");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https"), "require HTTPS");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https"), "require HTTPS redirects");
    // Use Windows' trusted roots so the standalone EXE needs no MSYS2 CA bundle.
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA), "use Windows certificates");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_CAINFO, NULL), "clear build-time certificate path");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_CAPATH, NULL), "clear build-time certificate directory");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, ReceiveRelease), "set release receiver");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response), "set release buffer");
    HANDLE_CURL_ERROR(cleanup, curl_easy_perform(handle), "fetch latest GitHub release");
    long httpCode = 0;
    HANDLE_CURL_ERROR(cleanup, curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &httpCode), "read release HTTP status");
    if (httpCode == 403 || httpCode == 429) {
        LOG_WARN("GitHub API returned HTTP %ld; checking the public latest Release page.", httpCode);
        success = ReadReleaseRedirect(handle, current, available);
        goto cleanup;
    }
    if (httpCode != 200) {
        LOG_WARN("Latest GitHub release request returned HTTP %ld.", httpCode);
        goto cleanup;
    }
    success = ReadRelease(&response, current, available);
    if (!success) LOG_WARN("GitHub returned an invalid stable release or unsupported version tag.");
cleanup:
    curl_easy_cleanup(handle);
    curl_slist_free_all(headers);
    free(response.data);
    return success;
}
