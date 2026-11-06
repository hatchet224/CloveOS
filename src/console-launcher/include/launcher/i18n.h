/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LAUNCHER_I18N_H
#define LAUNCHER_I18N_H
#include <stdint.h>
#include <stddef.h>
enum cl_language { CL_ENGLISH, CL_SPANISH };
/* Locale is process-local; only UI-owned text is passed here. Game names,
 * paths, SSIDs, protocol identifiers and core option IDs are never translated. */
void cl_language_set(unsigned int language);
enum cl_language cl_language_get(void);
void cl_copy_utf8(char *out,size_t capacity,const char *text);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format_arg(1)))
#endif
const char *cl_tr(const char *english);
#define CL_TR(text) cl_tr(text)
#endif
