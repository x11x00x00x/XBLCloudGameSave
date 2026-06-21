#include "eeprom_export.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <xboxkrnl/xboxkrnl.h>

/* SMBus slave address used to read the EEPROM (matches the kernel's own access
 * and the well-known dump tools). */
#define EEPROM_SMBUS_READ_ADDRESS 0xA9

/* Offsets within the EEPROM (see https://xboxdevwiki.net/EEPROM). */
#define EEPROM_SERIAL_OFFSET 0x34
#define EEPROM_SERIAL_LENGTH 0x0C
#define EEPROM_MAC_OFFSET 0x40
#define EEPROM_MAC_LENGTH 0x06

BOOL dumpEeprom(unsigned char *eeprom)
{
    for (int offset = 0; offset < EEPROM_SIZE; offset += 2) {
        ULONG value = 0;
        NTSTATUS status = HalReadSMBusValue(EEPROM_SMBUS_READ_ADDRESS,
                                            (UCHAR)offset, TRUE, &value);
        if (!NT_SUCCESS(status)) {
            return FALSE;
        }
        eeprom[offset]     = (unsigned char)(value & 0xFF);
        eeprom[offset + 1] = (unsigned char)((value >> 8) & 0xFF);
    }
    return TRUE;
}

BOOL writeFileBytes(const char *path, const void *data, DWORD len)
{
    HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(h, data, len, &written, NULL) && written == len;
    CloseHandle(h);
    return ok;
}

BOOL writeHddKeyFile(const char *path, const unsigned char *eeprom)
{
    char text[512];
    int pos = 0;

    pos += snprintf(text + pos, sizeof(text) - pos,
                    "Original Xbox HDD Key\r\n=====================\r\n\r\n");

    /* The kernel decrypts the HDD key from the EEPROM at boot and exports it.
     * Using it directly handles every motherboard revision correctly. */
    pos += snprintf(text + pos, sizeof(text) - pos, "HDD Key (hex): ");
    for (int i = 0; i < XBOX_KEY_LENGTH; i++) {
        pos += snprintf(text + pos, sizeof(text) - pos, "%02X", XboxHDKey[i]);
    }
    pos += snprintf(text + pos, sizeof(text) - pos, "\r\n");

    char serial[EEPROM_SERIAL_LENGTH + 1];
    memcpy(serial, eeprom + EEPROM_SERIAL_OFFSET, EEPROM_SERIAL_LENGTH);
    serial[EEPROM_SERIAL_LENGTH] = '\0';
    for (int i = 0; i < EEPROM_SERIAL_LENGTH; i++) {
        if (serial[i] < 0x20 || serial[i] > 0x7E) {
            serial[i] = '\0';
            break;
        }
    }
    pos += snprintf(text + pos, sizeof(text) - pos, "Serial Number: %s\r\n", serial);

    char mac[24];
    if (getEepromMac(eeprom, mac, sizeof(mac))) {
        pos += snprintf(text + pos, sizeof(text) - pos, "MAC Address: %s\r\n", mac);
    }

    pos += snprintf(text + pos, sizeof(text) - pos,
                    "\r\nWARNING: This key unlocks your hard drive. Treat it like a "
                    "password.\r\n");

    return writeFileBytes(path, text, (DWORD)pos);
}

BOOL getHddKeyHex(char *out, size_t outsz)
{
    if (outsz < (size_t)(XBOX_KEY_LENGTH * 2 + 1)) {
        return FALSE;
    }
    int pos = 0;
    for (int i = 0; i < XBOX_KEY_LENGTH; i++) {
        pos += snprintf(out + pos, outsz - pos, "%02X", XboxHDKey[i]);
    }
    out[pos] = '\0';
    return TRUE;
}

BOOL getEepromMac(const unsigned char *eeprom, char *out, size_t outsz)
{
    /* "XX:XX:XX:XX:XX:XX" + NUL = 18 bytes. */
    if (outsz < (size_t)(EEPROM_MAC_LENGTH * 3)) {
        return FALSE;
    }
    int pos = 0;
    for (int i = 0; i < EEPROM_MAC_LENGTH; i++) {
        if (i) {
            out[pos++] = ':';
        }
        pos += snprintf(out + pos, outsz - pos, "%02X", eeprom[EEPROM_MAC_OFFSET + i]);
    }
    out[pos] = '\0';
    return TRUE;
}

BOOL getEepromSerial(const unsigned char *eeprom, char *out, size_t outsz)
{
    if (outsz < (size_t)(EEPROM_SERIAL_LENGTH + 1)) {
        return FALSE;
    }
    memcpy(out, eeprom + EEPROM_SERIAL_OFFSET, EEPROM_SERIAL_LENGTH);
    out[EEPROM_SERIAL_LENGTH] = '\0';
    for (int i = 0; i < EEPROM_SERIAL_LENGTH; i++) {
        if (out[i] < 0x20 || out[i] > 0x7E) {
            out[i] = '\0';
            break;
        }
    }
    return TRUE;
}
