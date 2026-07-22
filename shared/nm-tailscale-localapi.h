/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NM_TAILSCALE_LOCALAPI_H
#define NM_TAILSCALE_LOCALAPI_H

#include <glib.h>

G_BEGIN_DECLS

extern gboolean nm_tailscale_debug;

/* Returns the response body on HTTP 2xx, NULL otherwise (with @error set).
 * @out_http_code (nullable) receives the HTTP status, 0 on transport error. */
char *nm_tailscale_localapi_call (const char *method,
                                  const char *path,
                                  const char *body,
                                  long *out_http_code,
                                  GError **error);

G_END_DECLS

#endif
