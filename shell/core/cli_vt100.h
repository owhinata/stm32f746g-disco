/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_vt100.h
 * @brief   VT100 SGR colour escapes for the Shell output API.
 *
 * cli_error/warn/info wrap their text in these codes (error=red, warn=yellow,
 * info=green) and reset afterwards.  When CLI_USE_COLOR is 0 every macro is the
 * empty string, so a monochrome terminal or a log sink sees plain text and the
 * output path stays byte-for-byte identical minus the escapes.  Only a handful
 * of well-known SGR sequences -- this is original, not copied.
 */
#ifndef CLI_VT100_H
#define CLI_VT100_H

#include "cli_config.h"   /* CLI_USE_COLOR */

#if CLI_USE_COLOR
#define CLI_VT100_RED     "\x1b[31m"
#define CLI_VT100_YELLOW  "\x1b[33m"
#define CLI_VT100_GREEN   "\x1b[32m"
#define CLI_VT100_RESET   "\x1b[0m"
#else
#define CLI_VT100_RED     ""
#define CLI_VT100_YELLOW  ""
#define CLI_VT100_GREEN   ""
#define CLI_VT100_RESET   ""
#endif

#endif /* CLI_VT100_H */
