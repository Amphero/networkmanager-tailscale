/* SPDX-License-Identifier: GPL-2.0-or-later */
/* HTTP client for tailscaled's LocalAPI, shared between the VPN service
 * daemon and the GTK4 editor (exit node list). */

#include <curl/curl.h>
#include <NetworkManager.h>

#include "nm-tailscale-localapi.h"

#define TAILSCALED_SOCKET "/var/run/tailscale/tailscaled.sock"

gboolean nm_tailscale_debug;

static const char *
tailscaled_socket (void)
{
	const char *p = g_getenv ("NM_TAILSCALE_SOCKET");

	return p && p[0] ? p : TAILSCALED_SOCKET;
}

static size_t
write_cb (char *ptr, size_t size, size_t nmemb, void *data)
{
	g_string_append_len (data, ptr, size * nmemb);
	return size * nmemb;
}

char *
nm_tailscale_localapi_call (const char *method, const char *path, const char *body,
                            long timeout_ms, long *out_http_code, GError **error)
{
	static gsize curl_inited = 0;
	CURL *curl;
	CURLcode rc;
	long code = 0;
	GString *resp = g_string_new (NULL);
	g_autofree char *url = g_strconcat ("http://local-tailscaled.sock", path, NULL);

	if (g_once_init_enter (&curl_inited)) {
		curl_global_init (CURL_GLOBAL_DEFAULT);
		g_once_init_leave (&curl_inited, 1);
	}

	if (nm_tailscale_debug)
		g_message ("localapi: %s %s %s", method, path, body ?: "");

	curl = curl_easy_init ();
	curl_easy_setopt (curl, CURLOPT_UNIX_SOCKET_PATH, tailscaled_socket ());
	curl_easy_setopt (curl, CURLOPT_URL, url);
	curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, method);
	if (body)
		curl_easy_setopt (curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 5000L);
	curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);

	rc = curl_easy_perform (curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup (curl);
	if (out_http_code)
		*out_http_code = rc == CURLE_OK ? code : 0;

	if (rc != CURLE_OK) {
		if (!g_file_test (tailscaled_socket (), G_FILE_TEST_EXISTS)) {
			g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
			             "tailscaled socket %s does not exist: "
			             "is tailscale installed and tailscaled.service running?",
			             tailscaled_socket ());
		} else {
			g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
			             "cannot reach tailscaled at %s: %s (is tailscaled.service running?)",
			             tailscaled_socket (), curl_easy_strerror (rc));
		}
		g_string_free (resp, TRUE);
		return NULL;
	}
	if (code < 200 || code > 299) {
		g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED,
		             "LocalAPI %s %s failed: HTTP %ld: %s", method, path, code, resp->str);
		g_string_free (resp, TRUE);
		return NULL;
	}
	return g_string_free (resp, FALSE);
}

/*****************************************************************************/

typedef struct {
	char *method;
	char *path;
	char *body;
	long timeout_ms;
	long http_code;
} CallData;

static void
call_data_free (CallData *data)
{
	g_free (data->method);
	g_free (data->path);
	g_free (data->body);
	g_free (data);
}

static void
call_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
	CallData *data = task_data;
	GError *error = NULL;
	char *resp;

	resp = nm_tailscale_localapi_call (data->method, data->path, data->body,
	                                   data->timeout_ms, &data->http_code, &error);
	if (resp)
		g_task_return_pointer (task, resp, g_free);
	else
		g_task_return_error (task, error);
}

void
nm_tailscale_localapi_call_async (const char *method, const char *path, const char *body,
                                  long timeout_ms, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task = g_task_new (NULL, cancellable, callback, user_data);
	CallData *data = g_new0 (CallData, 1);

	data->method = g_strdup (method);
	data->path = g_strdup (path);
	data->body = g_strdup (body);
	data->timeout_ms = timeout_ms;
	g_task_set_task_data (task, data, (GDestroyNotify) call_data_free);
	g_task_run_in_thread (task, call_thread);
	g_object_unref (task);
}

char *
nm_tailscale_localapi_call_finish (GAsyncResult *result, long *out_http_code, GError **error)
{
	if (out_http_code)
		*out_http_code = ((CallData *) g_task_get_task_data (G_TASK (result)))->http_code;
	return g_task_propagate_pointer (G_TASK (result), error);
}
