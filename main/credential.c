#include "credential.h"

#include <stdio.h>
#include <string.h>

size_t credential_hex(const credential_t *c, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0U) {
        return 0U;
    }
    buf[0] = '\0';
    if (c == NULL) {
        return 0U;
    }

    size_t pos = 0U;
    for (uint8_t i = 0U; i < c->len; i++) {
        /* Two characters and a terminator, or stop where we are. Truncating
         * on a byte boundary keeps a shortened string readable as hex
         * instead of ending mid-nibble. */
        if (pos + 3U > cap) {
            break;
        }
        pos += (size_t)snprintf(&buf[pos], cap - pos, "%02X", c->bytes[i]);
    }
    return pos;
}

const char *credential_kind_name(const credential_t *c)
{
    if (c != NULL && c->kind == CREDENTIAL_PKOC) {
        return "PKOC";
    }
    return "UID";
}

bool credential_equal(const credential_t *a, const credential_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    /* The kind is part of the identity. A card read once as a UID and then,
     * after the applet answered, as PKOC is two different things to report,
     * and collapsing them would silently drop the second. */
    return a->kind == b->kind
           && a->len == b->len
           && a->bit_count == b->bit_count
           && memcmp(a->bytes, b->bytes, a->len) == 0;
}
