#ifndef EEPROM_EXPORT_H
#define EEPROM_EXPORT_H

#include <windows.h>

#define EEPROM_SIZE 256

/* Reads the full 256-byte console EEPROM over the SMBus into eeprom.
 * Returns FALSE if any word read fails. */
BOOL dumpEeprom(unsigned char *eeprom);

/* Writes len bytes to a file, replacing any existing file. */
BOOL writeFileBytes(const char *path, const void *data, DWORD len);

/* Writes a human-readable file containing the decrypted HDD key (taken from the
 * kernel, which decrypts it from the EEPROM at boot) and the console serial
 * number read from the supplied EEPROM dump. */
BOOL writeHddKeyFile(const char *path, const unsigned char *eeprom);

/* Fills out with the kernel's decrypted HDD key as an uppercase hex string
 * (32 chars + NUL). Returns FALSE if the buffer is too small. */
BOOL getHddKeyHex(char *out, size_t outsz);

/* Fills out with the ASCII console serial number read from the EEPROM dump. */
BOOL getEepromSerial(const unsigned char *eeprom, char *out, size_t outsz);

/* Fills out with the console MAC address (EEPROM offset 0x40) formatted as
 * colon-separated uppercase hex (e.g. "00:50:F2:AA:BB:CC"). out needs >= 18 bytes.
 * Returns FALSE if the buffer is too small. */
BOOL getEepromMac(const unsigned char *eeprom, char *out, size_t outsz);

#endif
