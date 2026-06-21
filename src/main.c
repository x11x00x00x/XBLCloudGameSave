#include <hal/debug.h>
#include <hal/video.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <nxdk/mount.h>
#include <nxdk/net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "app_input.h"
#include "app_ui.h"
#include "base64.h"
#include "eeprom_export.h"
#include "net_auth.h"
#include "report.h"
#include "save_dates.h"
#include "scan_udata.h"
#include "session_store.h"
#include "unzip_export.h"
#include "upload.h"
#include "xbmc_profiles.h"
#include "content_hash.h"
#include "zip_export.h"

#define UDATA_PATH "E:\\UDATA"
#define OUTPUT_DIR "E:\\GameSaves"

#define EEPROM_BIN_PATH OUTPUT_DIR "\\eeprom.bin"
#define HDD_KEY_PATH    OUTPUT_DIR "\\hdd_key.txt"
#define REPORT_PATH     OUTPUT_DIR "\\saves_report.txt"
#define SESSION_PATH    OUTPUT_DIR "\\insignia_session.txt"
#define XBL_PROBE_PATH  OUTPUT_DIR "\\xbl_probe.txt"
#define SAVE_DATES_PATH OUTPUT_DIR "\\save_dates.txt"

/* The Insignia Stats website that receives the upload. Change these to match your
 * deployment (the public host of the "insignia stats" Node server). */
#define UPLOAD_HOST "xb.live"
#define UPLOAD_PORT "443"

extern struct netif *g_pnetif;

static BOOL waitForNetwork(void)
{
    /* Animate the boot spinner while lwIP brings the link up. We tick the
     * spinner roughly every 80ms (about 12 frames/sec) and time out after ~90s. */
    nxNetInit(NULL);
    const int ticksPerSecond = 12;
    const int totalTicks = 90 * ticksPerSecond;
    for (int t = 0; t < totalTicks; t++) {
        if (g_pnetif && netif_ip4_addr(g_pnetif)->addr != 0) {
            return TRUE;
        }
        ui_showBootScreen("Connecting to network...");
        Sleep(1000 / ticksPerSecond);
    }
    return FALSE;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    /* XBMC4Gamers-style centered logo + loading spinner on first boot. */
    for (int i = 0; i < 8; i++) {
        ui_showBootScreen("Starting up...");
        Sleep(60);
    }

    /* 1. Mount E: and create the output folder (also holds the saved session). */
    if (!nxIsDriveMounted('E')) {
        if (!nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\")) {
            debugMoveCursor(40, 200);
            debugPrint("ERROR: could not mount E: drive\n");
            inputWaitExitToDashboard("Could not mount E: drive.", NULL);
            return 1;
        }
    }

    /* Best-effort mount of C: as well — XBMC4Gamers (and its UserData/profiles)
     * commonly lives on C: or E:. Failure is non-fatal; detection just skips it. */
    if (!nxIsDriveMounted('C')) {
        nxMountDrive('C', "\\Device\\Harddisk0\\Partition2\\");
    }

    if (!CreateDirectory(OUTPUT_DIR, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        debugMoveCursor(40, 200);
        debugPrint("ERROR: could not create %s\n", OUTPUT_DIR);
        inputWaitExitToDashboard("Could not create output folder.", NULL);
        return 1;
    }

    /* 2. Network + Insignia login. Reuse a saved session if it is still valid;
     * only show the QR code when there is no valid stored login. */
    char sessionKey[160];
    char username[256];
    BOOL loggedIn = FALSE;
    sessionKey[0] = '\0';
    username[0] = '\0';

    if (waitForNetwork()) {
        if (loadSession(SESSION_PATH, sessionKey, sizeof(sessionKey), username, sizeof(username)) &&
            verifySession(sessionKey, username, sizeof(username))) {
            loggedIn = TRUE;
            ui_showLoggedIn(username);
            Sleep(2000);
        } else {
            loggedIn = deviceLogin(sessionKey, sizeof(sessionKey), username, sizeof(username));
            if (loggedIn) {
                saveSession(SESSION_PATH, sessionKey, username);
            }
        }
    }

    ui_beginProgress();
    ui_logf("Connecting to network...");
    if (!loggedIn) {
        ui_logf("No login - local backup only.");
    }

    /* 3. EEPROM + HDD key. */
    ui_logf("Reading EEPROM and HDD key...");
    unsigned char eeprom[EEPROM_SIZE];
    BOOL eepromOk = dumpEeprom(eeprom);
    char serial[32] = "";
    char hddKeyHex[64] = "";
    char mac[24] = "";
    if (eepromOk) {
        if (!writeFileBytes(EEPROM_BIN_PATH, eeprom, EEPROM_SIZE)) {
            ui_logf("  WARNING: failed to write eeprom.bin");
        }
        if (!writeHddKeyFile(HDD_KEY_PATH, eeprom)) {
            ui_logf("  WARNING: failed to write hdd_key.txt");
        }
        getEepromSerial(eeprom, serial, sizeof(serial));
        getHddKeyHex(hddKeyHex, sizeof(hddKeyHex));
        getEepromMac(eeprom, mac, sizeof(mac));
        ui_logf("  EEPROM and HDD key saved");
        if (serial[0]) {
            ui_logf("  Serial: %s", serial);
        }
        if (mac[0]) {
            ui_logf("  MAC: %s", mac);
        }
    } else {
        ui_logf("  WARNING: EEPROM read failed");
    }

    ui_setProgress(0.08f, "Scanning saves...");
    /* 4. Detect XBMC per-profile saves, then scan each profile's UDATA. Without
     * XBMC this is a single variation with an empty profile, so the scan/upload
     * behaves exactly like before. */
    XbmcVariation vars[XBMC_MAX_VARIATIONS];
    int varCount = xbmcEnumerateVariations(UDATA_PATH, vars, XBMC_MAX_VARIATIONS);
    if (varCount <= 0) {
        varCount = 1;
        vars[0].profileKey[0] = '\0';
        vars[0].profileLabel[0] = '\0';
        strncpy(vars[0].path, UDATA_PATH, sizeof(vars[0].path) - 1);
        vars[0].path[sizeof(vars[0].path) - 1] = '\0';
    }
    if (varCount > 1 || vars[0].profileKey[0]) {
        ui_logf("XBMC profiles detected: %d variation(s)", varCount);
        for (int v = 0; v < varCount; v++) {
            ui_logf("  profile[%d]: '%s' <- %s", v,
                    vars[v].profileLabel[0] ? vars[v].profileLabel : "(default)", vars[v].path);
        }
    } else {
        ui_logf("No XBMC profiles found - single UDATA");
    }

    ScanResult scans[XBMC_MAX_VARIATIONS];
    BOOL haveScans[XBMC_MAX_VARIATIONS];
    int grandTotalTitles = 0;
    for (int v = 0; v < varCount; v++) {
        if (vars[v].profileLabel[0]) {
            ui_logf("Scanning %s [%s]...", vars[v].path, vars[v].profileLabel);
        } else {
            ui_logf("Scanning %s...", vars[v].path);
        }
        haveScans[v] = scanUdata(vars[v].path, &scans[v]);
        if (haveScans[v]) {
            ui_logf("  Found %d titles, %d saves", scans[v].titleCount, scans[v].totalSaveCount);
            grandTotalTitles += scans[v].titleCount;
        } else {
            ui_logf("  WARNING: could not open %s", vars[v].path);
        }
    }
    /* Report covers the active profile's UDATA (vars[0]). */
    if (haveScans[0]) {
        if (writeReport(REPORT_PATH, &scans[0])) {
            ui_logf("  Report written to saves_report.txt");
        } else {
            ui_logf("  WARNING: failed to write report");
        }
    }

    /* 5. Console data upload (EEPROM + HDD key) — tiny, always sent when logged in. */
    char consoleId[40];
    consoleId[0] = '\0';
    if (loggedIn) {
        loadSessionConsoleId(SESSION_PATH, consoleId, sizeof(consoleId));
    }

    if (loggedIn && eepromOk) {
        ui_logf("Uploading to %s...", UPLOAD_HOST);
        if (uploadConsoleData(UPLOAD_HOST, UPLOAD_PORT, sessionKey, serial, hddKeyHex, eeprom,
                              EEPROM_SIZE, consoleId, sizeof(consoleId))) {
            ui_logf("  Uploaded EEPROM + HDD key (%s)", consoleId);
            saveSessionConsoleId(SESSION_PATH, consoleId);
        } else {
            ui_logf("  WARNING: console-data upload failed");
        }
    }

    /* 5b. Xbox LIVE account scan (always runs — does not require login). Upload and
     * restore still need a valid session / console_id. */
    {
        XblAccountSet accts;
        BOOL readOk = xblReadAccounts(&accts);
        if (readOk) {
            ui_logf("Xbox LIVE accounts found: %d (%d verified)", accts.presentCount,
                    accts.verifiedCount);
            for (int i = 0; i < XBL_ACCOUNT_MAX; i++) {
                if (accts.slots[i].present) {
                    if (accts.slots[i].verified) {
                        ui_logf("  slot %d: %s", i, accts.slots[i].gamertag);
                    } else {
                        ui_logf("  slot %d: %s (unverified signature)", i,
                                accts.slots[i].gamertag);
                    }
                }
            }
        } else {
            ui_logf("Xbox LIVE accounts: none readable");
        }

        if (xblWriteProbeFile(XBL_PROBE_PATH, readOk ? &accts : NULL)) {
            ui_logf("  Wrote xbl_probe.txt");
        } else {
            ui_logf("  WARNING: could not write xbl_probe.txt");
        }

        if (loggedIn) {
            /* Read the user's account-sync setting ONCE. It gates BOTH upload and
             * restore. If we cannot reach the server / read the setting, acctEnabled
             * stays FALSE and we do NOT touch the account sectors (fail safe). */
            BOOL acctEnabled = FALSE;
            BOOL haveSetting =
                consoleId[0] &&
                xblAccountSyncEnabled(UPLOAD_HOST, UPLOAD_PORT, sessionKey, &acctEnabled);

            if (readOk && haveSetting && acctEnabled && accts.presentCount > 0) {
                if (uploadXblAccounts(UPLOAD_HOST, UPLOAD_PORT, sessionKey, consoleId,
                                      accts.partition, &accts)) {
                    ui_logf("  Uploaded %d account(s) to xb.live", accts.presentCount);
                } else {
                    ui_logf("  WARNING: account upload failed");
                }
            } else if (readOk && accts.presentCount > 0 && !consoleId[0]) {
                ui_logf("  WARNING: accounts not uploaded (no console_id yet)");
            }

            /* Restore queued accounts. Writes go to config sectors 12-19 on HDD, or
             * FATX superblock slots on volumes that store accounts there.
             *
             * CRITICAL: only ever write to the account sectors when the user has
             * account sync explicitly ON. The server is supposed to return nothing
             * when sync is off, but we do NOT rely on that alone — a server bug, a
             * stale queue, or a bad response must never be able to overwrite a
             * user's accounts. This is the guard that keeps "sync off" meaning
             * "this tool will not write to my accounts". */
            if (consoleId[0] && haveSetting && !acctEnabled) {
                ui_logf("  Xbox LIVE account sync is OFF - not writing accounts");
            }
            if (consoleId[0] && haveSetting && acctEnabled) {
        static char restoreText[8192];
        if (fetchXblRestore(UPLOAD_HOST, UPLOAD_PORT, sessionKey, consoleId, restoreText,
                            sizeof(restoreText)) && restoreText[0]) {
            XblAccountSet cur;
            if (!xblReadAccounts(&cur)) {
                memset(&cur, 0, sizeof(cur));
                cur.partition = XBL_CONFIG_DISK_PART;
                cur.useConfigSectors = TRUE;
            }
            int restorePart = cur.partition;
            int restored = 0;
            char *line = restoreText;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) {
                    *nl = '\0';
                }
                /* Line: "id:slot:xuid:blobBase64" */
                char *c1 = strchr(line, ':');
                char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
                char *c3 = c2 ? strchr(c2 + 1, ':') : NULL;
                if (c1 && c2 && c3) {
                    *c1 = *c2 = *c3 = '\0';
                    const char *idStr = line;
                    int wantSlot = atoi(c1 + 1);
                    const char *xuid = c2 + 1;
                    const char *blobB64 = c3 + 1;
                    unsigned char rec[XBL_ACCOUNT_LEN];
                    int n = base64Decode(blobB64, rec, sizeof(rec));
                    if (n != XBL_ACCOUNT_LEN) {
                        ui_logf("  WARNING: bad account blob (%d bytes)", n);
                    } else if (!xblVerifyAccount(rec)) {
                        /* The dashboard silently ignores records that fail the XOnline
                         * signature check, so refuse to write one (this is the usual
                         * cause of "it installed but nothing shows"). */
                        ui_logf("  WARNING: account failed verification - skipped");
                    } else {
                        /* Policy: this tool NEVER removes or replaces an account. It
                         * only ADDS a missing account into a FREE slot:
                         *   1. if this XUID is already on the console, do nothing; and
                         *   2. only write into an empty slot, never over an occupied one. */
                        int existing = -1;
                        for (int i = 0; i < XBL_ACCOUNT_MAX; i++) {
                            if (cur.slots[i].present &&
                                xblXuidEqual(cur.slots[i].xuidHex, xuid)) {
                                existing = i;
                                break;
                            }
                        }

                        int slot = -1;
                        if (existing >= 0) {
                            /* Already installed — leave it untouched, just clear it
                             * from the restore queue so we stop offering it. */
                            ui_logf("  account already present (slot %d) - skipped", existing);
                            confirmXblRestored(UPLOAD_HOST, UPLOAD_PORT, sessionKey, idStr);
                        } else if (wantSlot >= 0 && wantSlot < XBL_ACCOUNT_MAX &&
                                   !cur.slots[wantSlot].present) {
                            slot = wantSlot; /* requested slot, and it is free */
                        } else {
                            for (int i = 0; i < XBL_ACCOUNT_MAX; i++) {
                                if (!cur.slots[i].present) {
                                    slot = i;
                                    break;
                                }
                            }
                        }

                        if (existing >= 0) {
                            /* handled above */
                        } else if (slot < 0) {
                            ui_logf("  WARNING: no free account slot - nothing removed");
                        } else if (cur.slots[slot].present) {
                            /* Belt-and-suspenders: never write over an occupied slot. */
                            ui_logf("  WARNING: slot %d not empty - skipped", slot);
                        } else if (!xblWriteAccount(restorePart, slot, rec)) {
                            ui_logf("  WARNING: account write failed (slot %d)", slot);
                        } else {
                            XblAccountSet check;
                            BOOL ok = FALSE;
                            if (xblReadAccounts(&check)) {
                                ok = (memcmp(check.slots[slot].raw, rec, XBL_ACCOUNT_LEN) == 0) &&
                                     xblVerifyAccount(check.slots[slot].raw);
                            }
                            memcpy(cur.slots[slot].raw, rec, XBL_ACCOUNT_LEN);
                            cur.slots[slot].present = TRUE;
                            cur.slots[slot].verified = ok;
                            strncpy(cur.slots[slot].xuidHex, xuid,
                                    sizeof(cur.slots[slot].xuidHex) - 1);
                            if (ok) {
                                confirmXblRestored(UPLOAD_HOST, UPLOAD_PORT, sessionKey, idStr);
                                restored++;
                                ui_logf("  Installed account slot %d (verified on disk)", slot);
                            } else {
                                ui_logf("  WARNING: slot %d write did not verify on disk", slot);
                            }
                        }
                    }
                }
                if (!nl) {
                    break;
                }
                line = nl + 1;
            }
            if (restored > 0) {
                ui_logf("  Installed %d Xbox LIVE account(s)", restored);
            }
        }
            }
        }
    }

    /* 6. Per-game archives + incremental upload. Skips titles already on the server
     * (same content hash on any console, or cloud save_modified >= local). Only
     * uploads when local is genuinely newer or the content is not stored yet. */
    static char manifest[16384];
    manifest[0] = '\0';
    if (loggedIn) {
        fetchSavesManifest(UPLOAD_HOST, UPLOAD_PORT, sessionKey, manifest, sizeof(manifest));
    }

    /* Local "true modification date" tracker. Pins each save's real date to its
     * content fingerprint so that moving folders (which resets FATX timestamps
     * to "now") cannot make an old save look newer than it really is. */
    saveDatesLoad(SAVE_DATES_PATH);

    ui_logf("Backing up each game...");
    if (grandTotalTitles > 0) {
        int created = 0, uploaded = 0, skipped = 0;
        int uploadTotal = grandTotalTitles > 0 ? grandTotalTitles : 1;
        int doneCount = 0;
        if (loggedIn) {
            ui_setUploadStats(0, grandTotalTitles);
        }
        for (int v = 0; v < varCount; v++) {
            if (!haveScans[v]) {
                continue;
            }
            ScanResult *sc = &scans[v];
            const char *profKey = vars[v].profileKey;
            const char *profLabel = vars[v].profileLabel[0] ? vars[v].profileLabel : NULL;
            for (int i = 0; i < sc->titleCount; i++) {
                const TitleInfo *title = &sc->titles[i];
                char gameName[META_NAME_MAX];
                char progLabel[112];
                titleDisplayName(title, gameName, sizeof(gameName));
                if (profLabel) {
                    snprintf(progLabel, sizeof(progLabel), "Processing %s [%s]", gameName, profLabel);
                } else {
                    snprintf(progLabel, sizeof(progLabel), "Processing %s", gameName);
                }
                ui_setProgress(0.15f + 0.65f * ((float)doneCount / (float)uploadTotal), progLabel);
                doneCount++;

                char fp[24];
                titleFingerprintHex(title, fp, sizeof(fp));

                char contentHash[65];
                contentHash[0] = '\0';
                titleContentHashHex(title, contentHash, sizeof(contentHash));

                /* Prefer the date pinned to this fingerprint over the raw
                 * filesystem mtime. If the content is unchanged the tracker
                 * returns the original date; otherwise we fall back to the
                 * (current) filesystem time for genuinely new/edited saves. */
                unsigned long long fsMod = titleLatestSaveUnix(title);
                unsigned long long trackedMod = saveDatesLookup(title->titleId, profKey, fp);
                unsigned long long localMod = trackedMod > 0 ? trackedMod : fsMod;

                if (loggedIn &&
                    manifestShouldSkipUpload(manifest, consoleId, profKey, title->titleId, fp,
                                             contentHash[0] ? contentHash : NULL, localMod)) {
                    unsigned long long cloudMod = manifestBestCloudMod(manifest, profKey, title->titleId);
                    if (cloudMod == 0) {
                        cloudMod = manifestCloudModUnix(manifest, consoleId, profKey, title->titleId);
                    }
                    if (trackedMod == 0 && (cloudMod > 0 || localMod == 0)) {
                        saveDatesRecord(title->titleId, profKey, fp,
                                        cloudMod > 0 ? cloudMod : fsMod);
                    }
                    skipped++;
                    ui_logf("  %s unchanged (skip)", gameName);
                    continue;
                }

                char zipPath[MAX_PATH];
                char dukexPath[MAX_PATH];
                snprintf(zipPath, sizeof(zipPath), "%s\\%s.zip", OUTPUT_DIR, title->titleId);
                snprintf(dukexPath, sizeof(dukexPath), "%s\\%s.dukex", OUTPUT_DIR, title->titleId);

                if (!zipTitleDirectory(title, zipPath)) {
                    ui_logf("  WARNING: failed to zip %s", gameName);
                    continue;
                }
                DeleteFile(dukexPath);
                if (!MoveFile(zipPath, dukexPath)) {
                    ui_logf("  WARNING: could not rename %s", gameName);
                    continue;
                }
                created++;

                if (loggedIn) {
                    unsigned long long saveMod = localMod;
                    char manifestJson[4096];
                    manifestJson[0] = '\0';
                    titleManifestJson(title, manifestJson, sizeof(manifestJson));
                    if (uploadGameDukex(UPLOAD_HOST, UPLOAD_PORT, sessionKey, consoleId, serial,
                                        hddKeyHex, profKey, profLabel, title->titleId, title->titleName,
                                        title->saveCount, title->totalSize, fp, saveMod,
                                        manifestJson, contentHash[0] ? contentHash : NULL, dukexPath)) {
                        uploaded++;
                        /* Pin the date we just reported to this content so future
                         * folder moves keep the same date. */
                        saveDatesRecord(title->titleId, profKey, fp, saveMod);
                        ui_setUploadStats(uploaded, grandTotalTitles);
                        ui_logf("  Uploaded %s", gameName);
                    } else {
                        ui_logf("  WARNING: upload failed for %s", gameName);
                    }
                }
            }
        }
        ui_setProgress(0.85f, "Backup complete");
        ui_logf("  %d archived, %d uploaded, %d skipped", created, uploaded, skipped);
    } else {
        ui_logf("  WARNING: skipped (no scan results)");
    }

    if (loggedIn) {
        ui_logf("View saves on xb.live (Game Saves tab)");
    }

    /* 7. Pull down sync-enabled saves from other consoles/profiles when the cloud
     * copy is NEWER than what is already on this Xbox (by save_modified time).
     * Restored folders get their original last-write times from _xbl_sync.txt. */
    if (loggedIn && manifest[0]) {
        ui_setProgress(0.90f, "Checking downloads...");
        ui_logf("Checking for saves to download...");

        typedef struct {
            char srcConsole[40];
            char srcProfile[40];
            char titleId[64];
            unsigned long long cloudMod;
        } SyncPick;

        SyncPick all[96];
        int allCount = 0;

        const char *p = manifest;
        while (*p && allCount < (int)(sizeof(all) / sizeof(all[0]))) {
            char lineKey[128];
            int k = 0;
            while (*p && *p != '=' && *p != '\r' && *p != '\n' && k < (int)sizeof(lineKey) - 1) {
                lineKey[k++] = *p++;
            }
            lineKey[k] = '\0';
            /* Capture the rest of the line (value) to detect the "|nosync" marker, which
             * means the website opted this save out of being synced back to the console. */
            char lineVal[128];
            int vk = 0;
            while (*p && *p != '\r' && *p != '\n') {
                if (vk < (int)sizeof(lineVal) - 1) {
                    lineVal[vk++] = *p;
                }
                p++;
            }
            lineVal[vk] = '\0';
            while (*p && *p != '\n') {
                p++;
            }
            if (*p == '\n') {
                p++;
            }
            if (k == 0 || strstr(lineVal, "|nosync") != NULL) {
                continue;
            }

            char srcConsole[40];
            char srcProfile[40];
            char tid[64];
            srcConsole[0] = '\0';
            srcProfile[0] = '\0';
            tid[0] = '\0';
            {
                const char *c1 = strchr(lineKey, ':');
                if (c1) {
                    size_t clen = (size_t)(c1 - lineKey);
                    if (clen < sizeof(srcConsole)) {
                        memcpy(srcConsole, lineKey, clen);
                        srcConsole[clen] = '\0';
                    }
                    const char *c2 = strchr(c1 + 1, ':');
                    if (c2) {
                        size_t plen = (size_t)(c2 - (c1 + 1));
                        if (plen < sizeof(srcProfile)) {
                            memcpy(srcProfile, c1 + 1, plen);
                            srcProfile[plen] = '\0';
                        }
                        strncpy(tid, c2 + 1, sizeof(tid) - 1);
                        tid[sizeof(tid) - 1] = '\0';
                    } else {
                        strncpy(tid, c1 + 1, sizeof(tid) - 1);
                        tid[sizeof(tid) - 1] = '\0';
                    }
                } else {
                    strncpy(tid, lineKey, sizeof(tid) - 1);
                    tid[sizeof(tid) - 1] = '\0';
                }
            }
            if (!tid[0]) {
                continue;
            }
            if (srcConsole[0] && consoleId[0] && strcmp(srcConsole, consoleId) == 0) {
                continue;
            }

            unsigned long long cloudMod = 0;
            {
                const char *mp = strchr(lineVal, '|');
                if (mp) {
                    mp++;
                    while (*mp >= '0' && *mp <= '9') {
                        cloudMod = cloudMod * 10ULL + (unsigned long long)(*mp - '0');
                        mp++;
                    }
                }
            }

            SyncPick *slot = &all[allCount++];
            strncpy(slot->srcConsole, srcConsole, sizeof(slot->srcConsole) - 1);
            strncpy(slot->srcProfile, srcProfile, sizeof(slot->srcProfile) - 1);
            strncpy(slot->titleId, tid, sizeof(slot->titleId) - 1);
            slot->cloudMod = cloudMod;
        }

        SyncPick picks[64];
        int pickCount = 0;
        for (int i = 0; i < allCount; i++) {
            int found = -1;
            for (int j = 0; j < pickCount; j++) {
                if (strcmp(picks[j].titleId, all[i].titleId) == 0) {
                    found = j;
                    break;
                }
            }
            if (found >= 0) {
                if (all[i].cloudMod > picks[found].cloudMod) {
                    picks[found] = all[i];
                }
            } else if (pickCount < (int)(sizeof(picks) / sizeof(picks[0]))) {
                picks[pickCount++] = all[i];
            }
        }

        CreateDirectory(UDATA_PATH, NULL);
        int pulled = 0;
        for (int i = 0; i < pickCount; i++) {
            const SyncPick *pick = &picks[i];
            unsigned long long localMod =
                haveScans[0] ? scanTitleLatestUnix(&scans[0], pick->titleId) : 0;

            /* If the local content matches a date we've pinned, use that pinned
             * date instead of the (possibly move-inflated) filesystem mtime, so
             * a moved-but-unchanged local save can't block a real cloud update. */
            if (localMod > 0 && haveScans[0]) {
                char localFp[24];
                if (scanTitleFingerprint(&scans[0], pick->titleId, localFp, sizeof(localFp))) {
                    unsigned long long pinned =
                        saveDatesLookup(pick->titleId, vars[0].profileKey, localFp);
                    if (pinned > 0) {
                        localMod = pinned;
                    }
                }
            }

            if (pick->cloudMod > 0 && localMod > 0 && pick->cloudMod <= localMod) {
                ui_logf("  %s skipped (local is newer or same)", pick->titleId);
                continue;
            }
            if (pick->cloudMod == 0 && localMod > 0) {
                ui_logf("  %s skipped (cloud date unknown)", pick->titleId);
                continue;
            }

            char dukexPath[MAX_PATH];
            char destDir[MAX_PATH];
            snprintf(dukexPath, sizeof(dukexPath), "%s\\%s.dukex", OUTPUT_DIR, pick->titleId);
            snprintf(destDir, sizeof(destDir), "%s\\%s", UDATA_PATH, pick->titleId);

            if (!downloadGameDukex(UPLOAD_HOST, UPLOAD_PORT, sessionKey,
                                   pick->srcConsole[0] ? pick->srcConsole : "legacy",
                                   consoleId[0] ? consoleId : "", pick->srcProfile, pick->titleId,
                                   dukexPath)) {
                ui_logf("  WARNING: download failed for %s", pick->titleId);
                continue;
            }
            if (unzipToDir(dukexPath, destDir)) {
                unzipApplySyncTimes(destDir);
                pulled++;
                ui_logf("  Restored %s (cloud newer)", pick->titleId);
            } else {
                ui_logf("  WARNING: could not extract %s", pick->titleId);
            }
        }
        ui_setProgress(1.0f, "Done");
        ui_logf("  Downloaded %d save(s) from server", pulled);
    }

    /* Persist the date tracker so the pinned dates survive across runs. */
    saveDatesFlush(SAVE_DATES_PATH);

    for (int v = 0; v < varCount; v++) {
        if (haveScans[v]) {
            freeScanResult(&scans[v]);
        }
    }

    {
        char line1[128];
        char line2[128];
        snprintf(line1, sizeof(line1), "Local output: %s", OUTPUT_DIR);
        snprintf(line2, sizeof(line2), "View saves on xb.live - Game Saves tab.");
        inputWaitExitToDashboard(line1, line2);
    }
    return 0;
}
