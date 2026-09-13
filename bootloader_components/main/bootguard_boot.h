/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "bootloader_utility.h"

/*
 * Call once per boot, after the boot partition was selected and before the
 * image is loaded. Updates the crash count from this boot's reset reason and
 * returns the index to boot: `boot_index` unchanged, TEST_APP_INDEX for the
 * recovery action, or INVALID_INDEX (the caller resets). The download and halt
 * actions do not return.
 */
int bootguard_check(const bootloader_state_t *bs, int boot_index);
