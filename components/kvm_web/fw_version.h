/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Comparing release tags, "v.0.81.0" or "v0.81", the same way the console does
 * (compareVersions in web/src/state/device.ts). Plain C, so it runs on a host.
 */
#pragma once

#include <stdbool.h>

/**
 * Compare two release tags.
 * @param out  negative when @p a is older, 0 when they match, positive when newer
 * @return false when either side is not a release, such as an untagged build
 *         ("v.0.81.0-3-gabc1234-dirty"): no order between those means anything.
 */
bool fw_version_cmp(const char *a, const char *b, int *out);
