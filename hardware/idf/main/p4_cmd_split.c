// The id/command split at the head of the BLE->HID command bridge, kept in its own
// file so the host test can compile it without FreeRTOS or NimBLE.
#include "p4_cmd_bridge.h"
#include <string.h>

// Splits "<id> <command>" into its two halves. Pure, so it can be unit-tested on
// the host without FreeRTOS or NimBLE (tests/test_cmd_split.c).
//
// The id is everything up to the first space and is echoed verbatim in the reply;
// the command is the rest. An empty id, a missing space, or anything that does not
// fit is refused, because a line we cannot echo an id for is a line the App can
// never match a reply to.
bool p4_cmd_split(const char *line, size_t len, char *id, size_t id_cap,
                  const char **cmd, size_t *cmd_len) {
    if (!line || !id || !cmd || !cmd_len || id_cap == 0) return false;
    const char *sp = memchr(line, ' ', len);
    if (!sp) return false;                       // no id at all
    size_t id_len = (size_t)(sp - line);
    if (id_len == 0 || id_len >= id_cap) return false;
    memcpy(id, line, id_len);
    id[id_len] = '\0';
    *cmd = sp + 1;
    *cmd_len = len - id_len - 1;
    return true;
}
