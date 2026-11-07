// Host test for p4_cmd_split(), the id/command split at the head of the BLE->HID
// command bridge. Pure logic, so no FreeRTOS or NimBLE needed: compile the real
// function straight out of the component source.
//
// A line whose id we cannot echo is a line the App can never match a reply to, so
// the refusals matter as much as the happy path.
#include "p4_cmd_bridge.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static void check(const char *what, int ok) {
    printf("  %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    char id[16];
    const char *cmd;
    size_t cmd_len;

    // Happy path: the App's own format, "<id> <command>".
    check("splits a plain command",
          p4_cmd_split("123456 click", strlen("123456 click"), id, sizeof id, &cmd, &cmd_len)
          && !strcmp(id, "123456") && cmd_len == strlen("click")
          && !memcmp(cmd, "click", strlen("click")));

    // A multi-word command keeps everything after the first space.
    check("keeps the rest of the line as the command",
          p4_cmd_split("7 move 120 -40", strlen("7 move 120 -40"), id, sizeof id, &cmd, &cmd_len)
          && !strcmp(id, "7") && cmd_len == strlen("move 120 -40")
          && !memcmp(cmd, "move 120 -40", strlen("move 120 -40")));

    // A trailing space is part of the command, not a separator we drop.
    check("keeps a trailing space in the command",
          p4_cmd_split("9 type hi ", strlen("9 type hi "), id, sizeof id, &cmd, &cmd_len)
          && !strcmp(id, "9") && cmd_len == strlen("type hi ")
          && !memcmp(cmd, "type hi ", strlen("type hi ")));

    // No space: no id to echo.
    check("refuses a line with no id", !p4_cmd_split("click", strlen("click"), id, sizeof id, &cmd, &cmd_len));

    // Empty id.
    check("refuses an empty id", !p4_cmd_split(" click", strlen(" click"), id, sizeof id, &cmd, &cmd_len));

    // An id too long for the buffer must be refused, not truncated.
    check("refuses an over-long id",
          !p4_cmd_split("12345678901234567890 click", strlen("12345678901234567890 click"), id, sizeof id, &cmd, &cmd_len));

    // An id that exactly fills the buffer minus the NUL is fine.
    check("accepts an id that just fits",
          p4_cmd_split("123456789012345 click", strlen("123456789012345 click"), id, sizeof id, &cmd, &cmd_len)
          && !strcmp(id, "123456789012345"));

    // Empty command after the id.
    check("splits an empty command",
          p4_cmd_split("42 ", strlen("42 "), id, sizeof id, &cmd, &cmd_len)
          && !strcmp(id, "42") && cmd_len == 0);

    // Bad arguments must not crash.
    check("refuses a NULL line", !p4_cmd_split(NULL, 0, id, sizeof id, &cmd, &cmd_len));
    check("refuses a zero id buffer", !p4_cmd_split("1 click", strlen("1 click"), id, 0, &cmd, &cmd_len));

    printf("%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
