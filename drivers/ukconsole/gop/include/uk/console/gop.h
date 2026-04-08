/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#ifndef __UK_CONSOLE_GOP_H__
#define __UK_CONSOLE_GOP_H__

#include <uk/efi.h>

uk_efi_status_t gop_init(struct uk_efi_boot_services *uk_efi_bs);
void gop_console_activate(void);

#endif /* __UK_CONSOLE_GOP_H__ */
