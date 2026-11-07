#pragma once
// Called by the authenticated command worker, one command at a time.
// Returns a static protocol reply. On disconnect call p4_hid_release_all().
const char *p4_hid_command(const char *line);
void p4_hid_release_all(void);
