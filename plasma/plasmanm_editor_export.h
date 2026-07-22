/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shim for the CMake-generated export header of plasma-nm — we only import
 * symbols from the installed libplasmanm_editor.so. */
#ifndef PLASMANM_EDITOR_EXPORT_H
#define PLASMANM_EDITOR_EXPORT_H

#define PLASMANM_EDITOR_EXPORT __attribute__((visibility("default")))
#define PLASMANM_EDITOR_NO_EXPORT
#define PLASMANM_EDITOR_DEPRECATED

#endif
