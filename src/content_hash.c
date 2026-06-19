#include "content_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#define HASH_LEN_UNSET 0x7fffffff

typedef struct {
    const char *titleId;
    const char *dataFile;
    int dataOffset;
    int dataLen; /* HASH_LEN_UNSET = use (fileSize - dataStart) */
} HashProfile;

/* Mirrors lib/xbox-save-profiles.json NoRoam slices (sigKey omitted). */
static const HashProfile HASH_PROFILES[] = {
    { "4c41000d", "options.opt", 24, HASH_LEN_UNSET },
    { "4541005b", "*.*", 0, -20 },
    { "54430006", "ups.dat", 20, HASH_LEN_UNSET },
    { "454d0009", "*.sav", 0, -20 },
    { "454d0020", "*.sav", 0, -20 },
    { "4d53006e", "UserProfile.xml", 0, -20 },
    { "4d53006e", "Garage.bin", 0, -20 },
    { "4d53006e", "Garage.dat", 0, -20 },
    { "4d53006e", "*.crt", 0, -20 },
    { "4d53006e", "Leaderboard*", 0, -20 },
    { "54540009", "*.*", 0, -20 },
    { "54430003", "*.dat", 20, HASH_LEN_UNSET },
    { "54430003", "system.dat", 20, HASH_LEN_UNSET },
    { "5443000d", "*.dat", 20, HASH_LEN_UNSET },
    { "5443000d", "system.dat", 20, HASH_LEN_UNSET },
    { "5553005e", "*.sav", 0, -20 },
    { "4c410013", "*.*", 0, -24 },
    { "4c410019", "*.*", 0, -24 },
    { "5655000d", "Options.dat", 20, -20 },
    { "56560003", "Options.dat", 20, -20 },
};

typedef struct {
    char path[MAX_PATH];
} HashEntry;

static BOOL titleIdEqual(const char *a, const char *b)
{
    if (!a || !b) {
        return FALSE;
    }
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return FALSE;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static const char *pathBaseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash) {
        return slash + 1;
    }
    slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

static BOOL globMatchBase(const char *path, const char *pattern)
{
    const char *base = pathBaseName(path);
    if (!pattern || !pattern[0]) {
        return FALSE;
    }
    if (strcmp(pattern, "*.*") == 0 || strcmp(pattern, "*") == 0) {
        return TRUE;
    }
    size_t plen = strlen(pattern);
    if (pattern[plen - 1] == '*') {
        if (plen == 1) {
            return TRUE;
        }
        return _strnicmp(base, pattern, plen - 1) == 0;
    }
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *dot = strrchr(base, '.');
        return dot && _stricmp(dot, pattern + 1) == 0;
    }
    return _stricmp(base, pattern) == 0;
}

static size_t resolveOffset(int off, size_t fileSize)
{
    if (off < 0) {
        if ((size_t)(-off) > fileSize) {
            return 0;
        }
        return fileSize + (size_t)off;
    }
    if ((size_t)off > fileSize) {
        return fileSize;
    }
    return (size_t)off;
}

static size_t resolveLen(int len, size_t fileSize)
{
    if (len == HASH_LEN_UNSET) {
        return fileSize;
    }
    if (len == 0) {
        return fileSize;
    }
    if (len < 0) {
        if ((size_t)(-len) > fileSize) {
            return 0;
        }
        return fileSize + (size_t)len;
    }
    if ((size_t)len > fileSize) {
        return fileSize;
    }
    return (size_t)len;
}

static const HashProfile *matchProfile(const char *titleId, const char *archivePath)
{
    for (size_t i = 0; i < sizeof(HASH_PROFILES) / sizeof(HASH_PROFILES[0]); i++) {
        const HashProfile *p = &HASH_PROFILES[i];
        if (!titleIdEqual(titleId, p->titleId)) {
            continue;
        }
        if (globMatchBase(archivePath, p->dataFile)) {
            return p;
        }
    }
    return NULL;
}

static int entryCompare(const void *a, const void *b)
{
    return strcmp(((const HashEntry *)a)->path, ((const HashEntry *)b)->path);
}

static BOOL collectFiles(const char *fullPath, const char *prefix, HashEntry **entries, int *count,
                         int *cap)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", fullPath);

    WIN32_FIND_DATA fd;
    HANDLE find = FindFirstFile(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return TRUE;
    }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }

        char childPath[MAX_PATH];
        snprintf(childPath, sizeof(childPath), "%s\\%s", fullPath, fd.cFileName);

        char childArchive[MAX_PATH];
        snprintf(childArchive, sizeof(childArchive), "%s%s", prefix, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char nextPrefix[MAX_PATH];
            snprintf(nextPrefix, sizeof(nextPrefix), "%s/", childArchive);
            if (!collectFiles(childPath, nextPrefix, entries, count, cap)) {
                FindClose(find);
                return FALSE;
            }
        } else {
            if (*count >= *cap) {
                int newCap = *cap ? *cap * 2 : 32;
                HashEntry *grown = (HashEntry *)realloc(*entries, (size_t)newCap * sizeof(HashEntry));
                if (!grown) {
                    FindClose(find);
                    return FALSE;
                }
                *entries = grown;
                *cap = newCap;
            }
            HashEntry *e = &(*entries)[*count];
            strncpy(e->path, childArchive, sizeof(e->path) - 1);
            e->path[sizeof(e->path) - 1] = '\0';
            (*count)++;
        }
    } while (FindNextFile(find, &fd));

    FindClose(find);
    return TRUE;
}

static void hexEncode(const unsigned char *in, size_t inLen, char *out, size_t outsz)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = inLen * 2;
    if (outsz == 0) {
        return;
    }
    if (n >= outsz) {
        n = outsz - 1;
    }
    for (size_t i = 0; i < n / 2; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[n] = '\0';
}

#define CONTENT_HASH_MAX_FILE (16 * 1024 * 1024)

static BOOL hashFileRegion(mbedtls_sha256_context *ctx, const char *titleId,
                           const char *archivePath, const char *fullPath)
{
    HANDLE h = CreateFile(fullPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > CONTENT_HASH_MAX_FILE) {
        CloseHandle(h);
        return FALSE;
    }

    unsigned char *data = (unsigned char *)malloc(size ? size : 1);
    if (!data) {
        CloseHandle(h);
        return FALSE;
    }

    DWORD total = 0;
    while (total < size) {
        DWORD got = 0;
        if (!ReadFile(h, data + total, size - total, &got, NULL) || got == 0) {
            break;
        }
        total += got;
    }
    CloseHandle(h);
    if (total != size) {
        free(data);
        return FALSE;
    }

    const HashProfile *profile = matchProfile(titleId, archivePath);
    size_t dataStart = 0;
    size_t dataEnd = size;
    if (profile) {
        dataStart = resolveOffset(profile->dataOffset, size);
        size_t lenArg = profile->dataLen;
        if (lenArg == HASH_LEN_UNSET) {
            lenArg = (int)(size > dataStart ? size - dataStart : 0);
        }
        size_t regionLen = resolveLen(lenArg, size);
        dataEnd = dataStart + regionLen;
        if (dataEnd > size) {
            dataEnd = size;
        }
        if (dataStart > dataEnd) {
            dataStart = dataEnd;
        }
    }

    mbedtls_sha256_update(ctx, (const unsigned char *)archivePath, strlen(archivePath));
    mbedtls_sha256_update(ctx, (const unsigned char *)"\n", 1);
    if (dataEnd > dataStart) {
        mbedtls_sha256_update(ctx, data + dataStart, dataEnd - dataStart);
    }
    free(data);
    return TRUE;
}

BOOL titleContentHashHex(const TitleInfo *title, char *out, size_t outsz)
{
    if (out && outsz > 0) {
        out[0] = '\0';
    }
    if (!title || !title->path[0] || !title->titleId[0] || !out || outsz < 65) {
        return FALSE;
    }

    HashEntry *entries = NULL;
    int count = 0;
    int cap = 0;
    if (!collectFiles(title->path, "", &entries, &count, &cap)) {
        free(entries);
        return FALSE;
    }
    if (count == 0) {
        free(entries);
        return FALSE;
    }

    qsort(entries, (size_t)count, sizeof(HashEntry), entryCompare);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        free(entries);
        mbedtls_sha256_free(&ctx);
        return FALSE;
    }

    BOOL ok = TRUE;
    for (int i = 0; ok && i < count; i++) {
        char fullPath[MAX_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", title->path, entries[i].path);
        for (char *p = fullPath + strlen(title->path) + 1; *p; p++) {
            if (*p == '/') {
                *p = '\\';
            }
        }
        ok = hashFileRegion(&ctx, title->titleId, entries[i].path, fullPath);
    }

    unsigned char digest[32];
    if (ok && mbedtls_sha256_finish(&ctx, digest) == 0) {
        hexEncode(digest, 32, out, outsz);
    } else {
        ok = FALSE;
    }

    mbedtls_sha256_free(&ctx);
    free(entries);
    return ok;
}
