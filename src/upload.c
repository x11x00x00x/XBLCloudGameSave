#include "upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hal/debug.h>

#include "../third_party/https_client.h"
#include "app_ui.h"
#include "base64.h"

typedef struct {
    char label[64];
    size_t total;
} UploadProgressCtx;

static void upload_progress_cb(size_t sent, size_t total, void *ctx)
{
    UploadProgressCtx *u = (UploadProgressCtx *)ctx;
    if (!u || total == 0) {
        return;
    }
    float f = (float)sent / (float)total;
    ui_setUploadProgress(f, u->label);
}

#define UPLOAD_RESP_SIZE 4096

/* Returns TRUE if the HTTP response status line is 2xx. */
static BOOL responseIsOk(const char *resp)
{
    /* Expect "HTTP/1.1 2xx ...". */
    const char *sp = strchr(resp, ' ');
    if (!sp) {
        return FALSE;
    }
    return sp[1] == '2';
}

/* Appends src to dst (a malloc'd buffer of capacity *cap starting at length *len),
 * growing as needed, with JSON-string escaping. Returns FALSE on allocation
 * failure. If escape is FALSE, src is copied verbatim (used for pre-built JSON
 * fragments and base64, which need no escaping). */
static BOOL appendStr(char **dst, size_t *len, size_t *cap, const char *src, BOOL escape)
{
    for (const char *p = src; *p; p++) {
        /* Worst case a single char expands to 6 (\u00XX); ensure headroom. */
        if (*len + 8 >= *cap) {
            size_t newCap = (*cap) * 2 + 256;
            char *grown = (char *)realloc(*dst, newCap);
            if (!grown) {
                return FALSE;
            }
            *dst = grown;
            *cap = newCap;
        }
        unsigned char c = (unsigned char)*p;
        if (!escape) {
            (*dst)[(*len)++] = (char)c;
            continue;
        }
        switch (c) {
            case '"':
                (*dst)[(*len)++] = '\\';
                (*dst)[(*len)++] = '"';
                break;
            case '\\':
                (*dst)[(*len)++] = '\\';
                (*dst)[(*len)++] = '\\';
                break;
            case '\n':
                (*dst)[(*len)++] = '\\';
                (*dst)[(*len)++] = 'n';
                break;
            case '\r':
                (*dst)[(*len)++] = '\\';
                (*dst)[(*len)++] = 'r';
                break;
            case '\t':
                (*dst)[(*len)++] = '\\';
                (*dst)[(*len)++] = 't';
                break;
            default:
                if (c < 0x20) {
                    *len += snprintf(*dst + *len, *cap - *len, "\\u%04x", c);
                } else {
                    (*dst)[(*len)++] = (char)c;
                }
                break;
        }
    }
    (*dst)[*len] = '\0';
    return TRUE;
}

static int json_extract_string_simple(const char *json, const char *key, char *out, size_t outsz)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) {
        return -1;
    }
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0 ? 0 : -1;
}

BOOL uploadConsoleData(const char *host, const char *port, const char *sessionKey,
                       const char *serial, const char *hddKeyHex,
                       const unsigned char *eeprom, size_t eepromLen,
                       char *consoleIdOut, size_t consoleIdOutSz)
{
    char *eepromB64 = base64Encode(eeprom, eepromLen);
    if (!eepromB64) {
        return FALSE;
    }

    size_t cap = 1024;
    size_t len = 0;
    char *body = (char *)malloc(cap);
    if (!body) {
        free(eepromB64);
        return FALSE;
    }
    body[0] = '\0';

    BOOL ok = appendStr(&body, &len, &cap, "{\"sessionKey\":\"", FALSE) &&
              appendStr(&body, &len, &cap, sessionKey, TRUE) &&
              appendStr(&body, &len, &cap, "\",\"serial\":\"", FALSE) &&
              appendStr(&body, &len, &cap, serial ? serial : "", TRUE) &&
              appendStr(&body, &len, &cap, "\",\"hdd_key_hex\":\"", FALSE) &&
              appendStr(&body, &len, &cap, hddKeyHex ? hddKeyHex : "", TRUE) &&
              appendStr(&body, &len, &cap, "\",\"eeprom_base64\":\"", FALSE) &&
              appendStr(&body, &len, &cap, eepromB64, FALSE) &&
              appendStr(&body, &len, &cap, "\",\"console_id_scheme\":\"", FALSE) &&
              appendStr(&body, &len, &cap, XBOX_CONSOLE_ID_SCHEME, FALSE) &&
              appendStr(&body, &len, &cap, "\"}", FALSE);
    free(eepromB64);
    if (!ok) {
        free(body);
        return FALSE;
    }

    char resp[UPLOAD_RESP_SIZE];
    UploadProgressCtx consoleProg;
    consoleProg.total = len;
    snprintf(consoleProg.label, sizeof(consoleProg.label), "Uploading console data...");
    int r = https_request(host, port, "POST", "/api/me/xbox-saves/console-data", "application/json",
                          NULL, 0, body, len, resp, sizeof(resp), upload_progress_cb, &consoleProg);
    free(body);
    if (r != 0) {
        return FALSE;
    }
    if (!responseIsOk(resp)) {
        return FALSE;
    }
    if (consoleIdOut && consoleIdOutSz > 0) {
        consoleIdOut[0] = '\0';
        const char *body = strstr(resp, "\r\n\r\n");
        if (body) {
            json_extract_string_simple(body + 4, "console_id", consoleIdOut, consoleIdOutSz);
        }
    }
    return TRUE;
}

BOOL uploadGameDukex(const char *host, const char *port, const char *sessionKey,
                     const char *consoleId, const char *serial, const char *hddKeyHex,
                     const char *profile, const char *profileLabel,
                     const char *titleId, const char *titleName, int saveCount,
                     unsigned long long totalBytes, const char *fingerprint,
                     unsigned long long saveModifiedUnix, const char *manifestJson,
                     const char *contentHash, const char *dukexPath)
{
    HANDLE h = CreateFile(dukexPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    DWORD sizeHigh = 0;
    DWORD sizeLow = GetFileSize(h, &sizeHigh);
    const char *disp = titleName;
    if (!disp || !disp[0] || strcmp(disp, "<unknown title>") == 0) {
        disp = titleId ? titleId : "game";
    }
    if (sizeHigh != 0 || sizeLow == INVALID_FILE_SIZE || sizeLow > UPLOAD_MAX_RAW_BYTES) {
        ui_logf("  skip %s (too large: %lu MB)", disp, (unsigned long)(sizeLow / (1024u * 1024u)));
        CloseHandle(h);
        return FALSE;
    }

    unsigned char *raw = (unsigned char *)malloc(sizeLow ? sizeLow : 1);
    if (!raw) {
        CloseHandle(h);
        return FALSE;
    }
    DWORD readTotal = 0;
    while (readTotal < sizeLow) {
        DWORD got = 0;
        if (!ReadFile(h, raw + readTotal, sizeLow - readTotal, &got, NULL) || got == 0) {
            break;
        }
        readTotal += got;
    }
    CloseHandle(h);
    if (readTotal != sizeLow) {
        free(raw);
        return FALSE;
    }

    /* Metadata travels in the query string + headers; the body is the raw file.
     * Always send HDD key + serial + console_id_scheme so the server can derive a
     * stable console_id (v2 = HDD+serial; legacy uploads used HDD key only). */
    char path[920];
    snprintf(path, sizeof(path),
             "/api/me/xbox-saves/game?title_id=%s&console_id=%s&hdd_key_hex=%s&serial=%s&"
             "console_id_scheme=%s&profile=%s&save_count=%d&total_bytes=%llu&fingerprint=%s&"
             "save_modified=%llu",
             titleId, consoleId && consoleId[0] ? consoleId : "unknown",
             hddKeyHex && hddKeyHex[0] ? hddKeyHex : "", serial ? serial : "",
             XBOX_CONSOLE_ID_SCHEME, profile ? profile : "", saveCount, totalBytes,
             fingerprint ? fingerprint : "", saveModifiedUnix);

    char skHeader[256];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", sessionKey);

    char *nameB64 = base64Encode((const unsigned char *)(titleName ? titleName : ""),
                                 titleName ? strlen(titleName) : 0);
    char nameHeader[512];
    snprintf(nameHeader, sizeof(nameHeader), "X-Title-Name-B64: %s", nameB64 ? nameB64 : "");
    free(nameB64);

    /* Human-readable profile name for the website (base64 so spaces survive headers). */
    char profileHeader[256];
    profileHeader[0] = '\0';
    if (profileLabel && profileLabel[0]) {
        char *plB64 = base64Encode((const unsigned char *)profileLabel, strlen(profileLabel));
        if (plB64) {
            snprintf(profileHeader, sizeof(profileHeader), "X-Profile-Label-B64: %s", plB64);
            free(plB64);
        }
    }

    char *manifestB64 = NULL;
    /* Keep the big header + response buffers OFF the stack. This is the deepest
     * frame before the TLS path needs its own room; a ~11 KB stack frame here can
     * overflow the (modest) stack and fault the console. Uploads run one at a
     * time, so file-scope/static buffers are safe. */
    static char manifestHeader[7168];
    manifestHeader[0] = '\0';
    if (manifestJson && manifestJson[0]) {
        manifestB64 = base64Encode((const unsigned char *)manifestJson, strlen(manifestJson));
        if (manifestB64) {
            snprintf(manifestHeader, sizeof(manifestHeader), "X-Manifest-B64: %s", manifestB64);
        }
    }

    char contentHashHeader[128];
    contentHashHeader[0] = '\0';
    if (contentHash && contentHash[0]) {
        snprintf(contentHashHeader, sizeof(contentHashHeader), "X-Content-Hash: %s", contentHash);
    }

    const char *headers[6];
    int nHeaders = 0;
    headers[nHeaders++] = skHeader;
    headers[nHeaders++] = nameHeader;
    if (profileHeader[0]) {
        headers[nHeaders++] = profileHeader;
    }
    if (manifestHeader[0]) {
        headers[nHeaders++] = manifestHeader;
    }
    if (contentHashHeader[0]) {
        headers[nHeaders++] = contentHashHeader;
    }

    UploadProgressCtx prog;
    prog.total = sizeLow;
    snprintf(prog.label, sizeof(prog.label), "Uploading %s", disp);

    static char resp[UPLOAD_RESP_SIZE];
    int r = https_request(host, port, "POST", path, "application/octet-stream", headers, nHeaders,
                          (const char *)raw, sizeLow, resp, sizeof(resp), upload_progress_cb, &prog);
    free(manifestB64);
    free(raw);
    if (r != 0) {
        return FALSE;
    }
    return responseIsOk(resp);
}

BOOL fetchSavesManifest(const char *host, const char *port, const char *sessionKey, char *out,
                        size_t outsz)
{
    char skHeader[256];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", sessionKey);
    const char *headers[] = { skHeader };

    if (outsz == 0) {
        return FALSE;
    }
    out[0] = '\0';

    static char resp[16384];
    int r = https_request(host, port, "GET", "/api/me/xbox-saves/manifest", "application/json",
                          headers, 1, NULL, 0, resp, sizeof(resp), NULL, NULL);
    if (r != 0 || !responseIsOk(resp)) {
        return FALSE;
    }
    const char *p = strstr(resp, "\r\n\r\n");
    const char *bodyStart = p ? p + 4 : resp;
    strncpy(out, bodyStart, outsz - 1);
    out[outsz - 1] = '\0';
    return TRUE;
}

BOOL downloadGameDukex(const char *host, const char *port, const char *sessionKey,
                       const char *sourceConsoleId, const char *targetConsoleId,
                       const char *sourceProfile, const char *titleId, const char *destPath)
{
    char path[512];
    snprintf(path, sizeof(path),
             "/api/me/xbox-saves/download/%s?console_id=%s&target_console_id=%s&profile=%s", titleId,
             sourceConsoleId && sourceConsoleId[0] ? sourceConsoleId : "",
             targetConsoleId && targetConsoleId[0] ? targetConsoleId : "",
             sourceProfile ? sourceProfile : "");

    char skHeader[256];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", sessionKey);
    const char *headers[] = { skHeader };

    int status = 0;
    int r = https_get_to_file(host, port, path, headers, 1, destPath, &status);
    return (r == 0) ? TRUE : FALSE;
}

BOOL xblAccountSyncEnabled(const char *host, const char *port, const char *sessionKey,
                           BOOL *enabledOut)
{
    if (enabledOut) {
        *enabledOut = FALSE;
    }
    char skHeader[256];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", sessionKey);
    const char *headers[] = { skHeader };
    char path[256];
    snprintf(path, sizeof(path), "/api/me/xbox-account/settings?sessionKey=%s", sessionKey);

    char resp[UPLOAD_RESP_SIZE];
    int r = https_request(host, port, "GET", path, "application/json", headers, 1, NULL, 0, resp,
                          sizeof(resp), NULL, NULL);
    if (r != 0 || !responseIsOk(resp)) {
        return FALSE;
    }
    const char *body = strstr(resp, "\r\n\r\n");
    if (body && strstr(body, "\"enabled\":true")) {
        if (enabledOut) {
            *enabledOut = TRUE;
        }
    }
    return TRUE;
}

BOOL uploadXblAccounts(const char *host, const char *port, const char *sessionKey,
                       const char *consoleId, int partition, const XblAccountSet *set)
{
    if (!set || set->presentCount <= 0) {
        return TRUE; /* nothing to upload is not an error */
    }

    size_t cap = 2048;
    size_t len = 0;
    char *body = (char *)malloc(cap);
    if (!body) {
        return FALSE;
    }
    body[0] = '\0';

    char partStr[16];
    snprintf(partStr, sizeof(partStr), "%d", partition);

    BOOL ok = appendStr(&body, &len, &cap, "{\"sessionKey\":\"", FALSE) &&
              appendStr(&body, &len, &cap, sessionKey, TRUE) &&
              appendStr(&body, &len, &cap, "\",\"console_id\":\"", FALSE) &&
              appendStr(&body, &len, &cap, consoleId ? consoleId : "", TRUE) &&
              appendStr(&body, &len, &cap, "\",\"source_partition\":", FALSE) &&
              appendStr(&body, &len, &cap, partStr, FALSE) &&
              appendStr(&body, &len, &cap, ",\"accounts\":[", FALSE);

    BOOL first = TRUE;
    for (int i = 0; ok && i < XBL_ACCOUNT_MAX; i++) {
        if (!set->slots[i].present) {
            continue;
        }
        char *blobB64 = base64Encode(set->slots[i].raw, XBL_ACCOUNT_LEN);
        if (!blobB64) {
            ok = FALSE;
            break;
        }
        ok = appendStr(&body, &len, &cap, first ? "{" : ",{", FALSE) &&
             appendStr(&body, &len, &cap, "\"xuid\":\"", FALSE) &&
             appendStr(&body, &len, &cap, set->slots[i].xuidHex, TRUE) &&
             appendStr(&body, &len, &cap, "\",\"gamertag\":\"", FALSE) &&
             appendStr(&body, &len, &cap, set->slots[i].gamertag, TRUE) &&
             appendStr(&body, &len, &cap, "\",\"blob_base64\":\"", FALSE) &&
             appendStr(&body, &len, &cap, blobB64, FALSE) &&
             appendStr(&body, &len, &cap, "\"}", FALSE);
        free(blobB64);
        first = FALSE;
    }
    if (ok) {
        ok = appendStr(&body, &len, &cap, "]}", FALSE);
    }
    if (!ok) {
        free(body);
        return FALSE;
    }

    char resp[UPLOAD_RESP_SIZE];
    int r = https_request(host, port, "POST", "/api/me/xbox-account/upload", "application/json",
                          NULL, 0, body, len, resp, sizeof(resp), NULL, NULL);
    free(body);
    return (r == 0) && responseIsOk(resp);
}

BOOL fetchXblRestore(const char *host, const char *port, const char *sessionKey,
                     const char *consoleId, char *out, size_t outsz)
{
    if (outsz == 0) {
        return FALSE;
    }
    out[0] = '\0';
    char skHeader[256];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", sessionKey);
    const char *headers[] = { skHeader };
    char path[320];
    snprintf(path, sizeof(path), "/api/me/xbox-account/restore?console_id=%s&sessionKey=%s",
             consoleId ? consoleId : "", sessionKey);

    static char resp[8192];
    int r = https_request(host, port, "GET", path, "application/json", headers, 1, NULL, 0, resp,
                          sizeof(resp), NULL, NULL);
    if (r != 0 || !responseIsOk(resp)) {
        return FALSE;
    }
    const char *body = strstr(resp, "\r\n\r\n");
    const char *start = body ? body + 4 : resp;
    strncpy(out, start, outsz - 1);
    out[outsz - 1] = '\0';
    return TRUE;
}

BOOL confirmXblRestored(const char *host, const char *port, const char *sessionKey, const char *id)
{
    char path[256];
    snprintf(path, sizeof(path), "/api/me/xbox-account/%s/restored", id ? id : "");
    char skHeader[256];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", sessionKey);
    const char *headers[] = { skHeader };

    char body[256];
    int blen = snprintf(body, sizeof(body), "{\"sessionKey\":\"%s\"}", sessionKey);
    char resp[UPLOAD_RESP_SIZE];
    int r = https_request(host, port, "POST", path, "application/json", headers, 1, body, blen, resp,
                          sizeof(resp), NULL, NULL);
    return (r == 0) && responseIsOk(resp);
}

static BOOL titleIdEqualI(const char *a, const char *b)
{
    if (!a || !b) {
        return FALSE;
    }
    return _stricmp(a, b) == 0;
}

static BOOL profileEqual(const char *a, const char *b)
{
    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    return strcmp(a, b) == 0;
}

static BOOL isHexHashField(const char *s)
{
    if (!s || !s[0]) {
        return FALSE;
    }
    size_t n = strlen(s);
    if (n < 16 || n > 128) {
        return FALSE;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Parses "console:profile:title=fingerprint|mod|hash|nosync" from one manifest line. */
static BOOL manifestParseLine(const char *lineStart, const char *lineEnd, char *consoleId,
                              size_t consoleIdSz, char *profile, size_t profileSz, char *titleId,
                              size_t titleIdSz, char *fingerprint, size_t fingerprintSz,
                              unsigned long long *modOut, char *contentHash, size_t contentHashSz)
{
    if (consoleId && consoleIdSz > 0) {
        consoleId[0] = '\0';
    }
    if (profile && profileSz > 0) {
        profile[0] = '\0';
    }
    if (titleId && titleIdSz > 0) {
        titleId[0] = '\0';
    }
    if (fingerprint && fingerprintSz > 0) {
        fingerprint[0] = '\0';
    }
    if (contentHash && contentHashSz > 0) {
        contentHash[0] = '\0';
    }
    if (modOut) {
        *modOut = 0;
    }
    if (!lineStart || lineStart >= lineEnd) {
        return FALSE;
    }

    const char *eq = strchr(lineStart, '=');
    if (!eq || eq >= lineEnd) {
        return FALSE;
    }

    const char *key = lineStart;
    size_t keyLen = (size_t)(eq - key);
    char keyBuf[160];
    if (keyLen >= sizeof(keyBuf)) {
        return FALSE;
    }
    memcpy(keyBuf, key, keyLen);
    keyBuf[keyLen] = '\0';

    const char *c1 = strchr(keyBuf, ':');
    if (c1) {
        size_t clen = (size_t)(c1 - keyBuf);
        if (consoleId && clen < consoleIdSz) {
            memcpy(consoleId, keyBuf, clen);
            consoleId[clen] = '\0';
        }
        const char *c2 = strchr(c1 + 1, ':');
        if (c2) {
            size_t plen = (size_t)(c2 - (c1 + 1));
            if (profile && plen < profileSz) {
                memcpy(profile, c1 + 1, plen);
                profile[plen] = '\0';
            }
            if (titleId) {
                strncpy(titleId, c2 + 1, titleIdSz - 1);
                titleId[titleIdSz - 1] = '\0';
            }
        } else if (titleId) {
            strncpy(titleId, c1 + 1, titleIdSz - 1);
            titleId[titleIdSz - 1] = '\0';
        }
    } else if (titleId) {
        strncpy(titleId, keyBuf, titleIdSz - 1);
        titleId[titleIdSz - 1] = '\0';
    }

    char valBuf[256];
    size_t valLen = (size_t)(lineEnd - (eq + 1));
    if (valLen >= sizeof(valBuf)) {
        return FALSE;
    }
    memcpy(valBuf, eq + 1, valLen);
    valBuf[valLen] = '\0';

    size_t vlen = strlen(valBuf);
    if (vlen >= 7 && strcmp(valBuf + vlen - 7, "|nosync") == 0) {
        valBuf[vlen - 7] = '\0';
    }

    const char *fp = valBuf;
    const char *mp = strchr(fp, '|');
    if (!mp) {
        if (fingerprint) {
            strncpy(fingerprint, fp, fingerprintSz - 1);
            fingerprint[fingerprintSz - 1] = '\0';
        }
        return titleId && titleId[0];
    }

    if (fingerprint) {
        size_t flen = (size_t)(mp - fp);
        if (flen >= fingerprintSz) {
            flen = fingerprintSz - 1;
        }
        memcpy(fingerprint, fp, flen);
        fingerprint[flen] = '\0';
    }

    mp++;
    if (modOut) {
        while (*mp >= '0' && *mp <= '9') {
            *modOut = *modOut * 10ULL + (unsigned long long)(*mp - '0');
            mp++;
        }
    }

    if (*mp == '|') {
        mp++;
        if (isHexHashField(mp) && contentHash && contentHashSz > 0) {
            strncpy(contentHash, mp, contentHashSz - 1);
            contentHash[contentHashSz - 1] = '\0';
        }
    }

    return titleId && titleId[0];
}

static void manifestEachLine(const char *manifest,
                             void (*fn)(const char *consoleId, const char *profile,
                                        const char *titleId, const char *fingerprint,
                                        unsigned long long mod, const char *contentHash, void *ctx),
                             void *ctx)
{
    if (!manifest || !fn) {
        return;
    }
    const char *p = manifest;
    while (*p) {
        const char *lineStart = p;
        while (*p && *p != '\r' && *p != '\n') {
            p++;
        }
        const char *lineEnd = p;
        while (*p == '\r' || *p == '\n') {
            p++;
        }
        if (lineEnd > lineStart) {
            char consoleId[40];
            char prof[40];
            char tid[64];
            char fp[24];
            char hash[129];
            unsigned long long mod = 0;
            if (manifestParseLine(lineStart, lineEnd, consoleId, sizeof(consoleId), prof,
                                  sizeof(prof), tid, sizeof(tid), fp, sizeof(fp), &mod, hash,
                                  sizeof(hash))) {
                fn(consoleId, prof, tid, fp, mod, hash[0] ? hash : NULL, ctx);
            }
        }
    }
}

typedef struct {
    const char *profile;
    const char *titleId;
    unsigned long long bestMod;
} BestModCtx;

static void manifestBestModCb(const char *consoleId, const char *profile, const char *titleId,
                              const char *fingerprint, unsigned long long mod,
                              const char *contentHash, void *ctx)
{
    (void)consoleId;
    (void)fingerprint;
    (void)contentHash;
    BestModCtx *b = (BestModCtx *)ctx;
    if (!profileEqual(profile, b->profile) || !titleIdEqualI(titleId, b->titleId)) {
        return;
    }
    if (mod > b->bestMod) {
        b->bestMod = mod;
    }
}

unsigned long long manifestBestCloudMod(const char *manifest, const char *profile,
                                        const char *titleId)
{
    BestModCtx ctx;
    ctx.profile = profile ? profile : "";
    ctx.titleId = titleId;
    ctx.bestMod = 0;
    manifestEachLine(manifest, manifestBestModCb, &ctx);
    return ctx.bestMod;
}

typedef struct {
    const char *profile;
    const char *titleId;
    const char *contentHash;
    BOOL found;
} HashMatchCtx;

static void manifestHashMatchCb(const char *consoleId, const char *profile, const char *titleId,
                                const char *fingerprint, unsigned long long mod,
                                const char *contentHash, void *ctx)
{
    (void)consoleId;
    (void)fingerprint;
    (void)mod;
    HashMatchCtx *h = (HashMatchCtx *)ctx;
    if (!h->contentHash || !h->contentHash[0] || !contentHash || !contentHash[0]) {
        return;
    }
    if (!profileEqual(profile, h->profile) || !titleIdEqualI(titleId, h->titleId)) {
        return;
    }
    if (_stricmp(contentHash, h->contentHash) == 0) {
        h->found = TRUE;
    }
}

BOOL manifestShouldSkipUpload(const char *manifest, const char *consoleId, const char *profile,
                              const char *titleId, const char *fingerprint,
                              const char *contentHash, unsigned long long localMod)
{
    if (!manifest || !titleId || !titleId[0]) {
        return FALSE;
    }

    if (contentHash && contentHash[0]) {
        HashMatchCtx hctx;
        hctx.profile = profile ? profile : "";
        hctx.titleId = titleId;
        hctx.contentHash = contentHash;
        hctx.found = FALSE;
        manifestEachLine(manifest, manifestHashMatchCb, &hctx);
        if (hctx.found) {
            return TRUE;
        }
    }

    unsigned long long bestCloud = manifestBestCloudMod(manifest, profile, titleId);
    if (bestCloud > 0 && localMod > 0 && localMod <= bestCloud) {
        return TRUE;
    }

    if (fingerprint && fingerprint[0] && consoleId && consoleId[0] &&
        manifestTitleMatches(manifest, consoleId, profile, titleId, fingerprint)) {
        unsigned long long ownCloud = manifestCloudModUnix(manifest, consoleId, profile, titleId);
        if (ownCloud > 0 || localMod == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

BOOL manifestTitleMatches(const char *manifest, const char *consoleId, const char *profile,
                          const char *titleId, const char *fingerprint)
{
    if (!manifest || !titleId || !fingerprint || !fingerprint[0]) {
        return FALSE;
    }
    char needle[128];
    if (consoleId && consoleId[0]) {
        /* Server emits "console_id:profile:title_id=" (profile may be empty). */
        snprintf(needle, sizeof(needle), "%s:%s:%s=", consoleId, profile ? profile : "", titleId);
    } else {
        snprintf(needle, sizeof(needle), "%s=", titleId);
    }
    const char *p = manifest;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        if (p == manifest || p[-1] == '\n') {
            const char *val = p + nlen;
            size_t flen = strlen(fingerprint);
            /* '|' terminates the fingerprint too (e.g. "...=<fp>|nosync"). */
            if (strncmp(val, fingerprint, flen) == 0 &&
                (val[flen] == '\0' || val[flen] == '\r' || val[flen] == '\n' ||
                 val[flen] == '|')) {
                return TRUE;
            }
            return FALSE;
        }
        p += nlen;
    }
    return FALSE;
}

unsigned long long manifestCloudModUnix(const char *manifest, const char *consoleId,
                                        const char *profile, const char *titleId)
{
    if (!manifest || !titleId || !titleId[0]) {
        return 0;
    }
    char needle[128];
    if (consoleId && consoleId[0]) {
        snprintf(needle, sizeof(needle), "%s:%s:%s=", consoleId, profile ? profile : "", titleId);
    } else {
        snprintf(needle, sizeof(needle), "%s=", titleId);
    }
    const char *p = manifest;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        if (p != manifest && p[-1] != '\n') {
            p++;
            continue;
        }
        const char *val = p + nlen;
        const char *mp = strchr(val, '|');
        if (!mp) {
            return 0;
        }
        mp++;
        unsigned long long cloudMod = 0;
        while (*mp >= '0' && *mp <= '9') {
            cloudMod = cloudMod * 10ULL + (unsigned long long)(*mp - '0');
            mp++;
        }
        return cloudMod;
    }
    return 0;
}
