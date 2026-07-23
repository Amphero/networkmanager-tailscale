/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NM_TAILSCALE_LOCALAPI_H
#define NM_TAILSCALE_LOCALAPI_H

#include <gio/gio.h>

G_BEGIN_DECLS

extern gboolean nm_tailscale_debug;

/* Returns the response body on HTTP 2xx, NULL otherwise (with @error set).
 * @timeout_ms caps the whole call; <= 0 means the 5 s default. Callers on
 * a UI or D-Bus main loop should pass something short — the call blocks.
 * @out_http_code (nullable) receives the HTTP status, 0 on transport error. */
char *nm_tailscale_localapi_call (const char *method,
                                  const char *path,
                                  const char *body,
                                  long timeout_ms,
                                  long *out_http_code,
                                  GError **error);

/* Same call, but run in a worker thread; @callback fires on the caller's
 * main context. @method/@path/@body are copied, @callback may be NULL for
 * fire-and-forget. Finish with nm_tailscale_localapi_call_finish(). */
void nm_tailscale_localapi_call_async (const char *method,
                                       const char *path,
                                       const char *body,
                                       long timeout_ms,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);

char *nm_tailscale_localapi_call_finish (GAsyncResult *result,
                                         long *out_http_code,
                                         GError **error);

G_END_DECLS

#endif
