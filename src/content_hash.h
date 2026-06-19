#ifndef CONTENT_HASH_H
#define CONTENT_HASH_H

#include <stddef.h>
#include <windows.h>

#include "scan_udata.h"

/* SHA-256 hex digest (64 chars + NUL) of a title's save tree. Signed (NoRoam)
 * files contribute only their signed data region so the hash matches across
 * consoles; other files are hashed whole. Used as X-Content-Hash on upload. */
BOOL titleContentHashHex(const TitleInfo *title, char *out, size_t outsz);

#endif
