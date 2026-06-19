#ifndef UPLOAD_H
#define UPLOAD_H

#include <stddef.h>
#include <windows.h>

#include "xbl_account.h"

/* Largest .dukex archive the console will attempt to upload. The whole file is
 * held in RAM once (no base64 inflation any more), so this is bounded by the
 * console's ~64 MB of memory minus what the TLS stack and scan structures use. */
#define UPLOAD_MAX_RAW_BYTES (32u * 1024u * 1024u)

/* Console identity scheme sent to the server. v2 derives console_id from BOTH the
 * HDD key and the EEPROM serial so two consoles with the same cloned HDD key still
 * appear as separate entries in My Xbox. Legacy builds omit this and keep HDD-only. */
#define XBOX_CONSOLE_ID_SCHEME "v2"

/* POSTs the EEPROM dump (base64), decrypted HDD key (hex) and serial number to
 * /api/me/xbox-saves/console-data. Returns TRUE on a 2xx response. */
BOOL uploadConsoleData(const char *host, const char *port, const char *sessionKey,
                       const char *serial, const char *hddKeyHex,
                       const unsigned char *eeprom, size_t eepromLen,
                       char *consoleIdOut, size_t consoleIdOutSz);

/* Reads dukexPath and POSTs it as a raw binary body to /api/me/xbox-saves/game,
 * with metadata in the query string/headers (including the fingerprint used for
 * incremental uploads). Returns TRUE on a 2xx response. */
/* profile is the XBMC profile key ("" for the default / non-XBMC dashboard, which
 * behaves exactly as before). When non-empty the save is stored separately on the
 * server so different profiles' saves for the same game never overwrite each other.
 * profileLabel is the human-readable profile name for the website (may be NULL). */
BOOL uploadGameDukex(const char *host, const char *port, const char *sessionKey,
                     const char *consoleId, const char *serial, const char *hddKeyHex,
                     const char *profile, const char *profileLabel,
                     const char *titleId, const char *titleName,
                     int saveCount, unsigned long long totalBytes, const char *fingerprint,
                     unsigned long long saveModifiedUnix, const char *manifestJson,
                     const char *contentHash, const char *dukexPath);

/* GETs /api/me/xbox-saves/manifest into out (a text body of "TITLEID=FINGERPRINT"
 * lines). Returns TRUE on a 2xx response. */
BOOL fetchSavesManifest(const char *host, const char *port, const char *sessionKey, char *out,
                        size_t outsz);

/* Returns TRUE if manifest contains "consoleId:profile:titleId=fingerprint" (i.e.
 * the server already has this exact version for this profile and the upload can be
 * skipped). profile may be "" for the default / non-XBMC case. */
BOOL manifestTitleMatches(const char *manifest, const char *consoleId, const char *profile,
                          const char *titleId, const char *fingerprint);

/* Parses save_modified_unix from a manifest line (0 if missing/legacy). */
unsigned long long manifestCloudModUnix(const char *manifest, const char *consoleId,
                                        const char *profile, const char *titleId);

/* Newest save_modified_unix for title+profile across every console in the manifest. */
unsigned long long manifestBestCloudMod(const char *manifest, const char *profile,
                                        const char *titleId);

/* TRUE when the server already has this save and local is not newer — skip upload.
 * Matches on content_hash (any console) or when cloud's best date >= localMod.
 * profile may be "" for the default / non-XBMC case. contentHash may be NULL. */
BOOL manifestShouldSkipUpload(const char *manifest, const char *consoleId, const char *profile,
                              const char *titleId, const char *fingerprint,
                              const char *contentHash, unsigned long long localMod);

/* Downloads a title's .dukex archive from /api/me/xbox-saves/download/<titleId>
 * to destPath. sourceProfile is the XBMC profile the save belongs to on the server
 * ("" for default). Returns TRUE on a 2xx response (file written). */
BOOL downloadGameDukex(const char *host, const char *port, const char *sessionKey,
                       const char *sourceConsoleId, const char *targetConsoleId,
                       const char *sourceProfile, const char *titleId, const char *destPath);

/* ---- Optional Xbox LIVE (Insignia) account backup/transfer ---- */

/* Returns TRUE if the user enabled account backup on the website (opt-in, off by
 * default). enabledOut is set to the parsed flag. Returns FALSE on request error. */
BOOL xblAccountSyncEnabled(const char *host, const char *port, const char *sessionKey,
                           BOOL *enabledOut);

/* Uploads the present account records to /api/me/xbox-account/upload. */
BOOL uploadXblAccounts(const char *host, const char *port, const char *sessionKey,
                       const char *consoleId, int partition, const XblAccountSet *set);

/* Fetches the accounts queued to restore to this console as text lines
 * ("id:slot:xuid:blobBase64"). Returns TRUE on a 2xx response. */
BOOL fetchXblRestore(const char *host, const char *port, const char *sessionKey,
                     const char *consoleId, char *out, size_t outsz);

/* Tells the server an account row was written so it is not restored again. */
BOOL confirmXblRestored(const char *host, const char *port, const char *sessionKey, const char *id);

#endif
