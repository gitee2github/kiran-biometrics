#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <syslog.h>

#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib-lowlevel.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>

#include "config.h"
#include "kiran-biometrics-proxy.h"
#include "kiran-pam.h"
#include "kiran-pam-msg.h"

typedef struct {
    char *result;
    pam_handle_t *pamh;
    GMainLoop *loop;
} verify_data;

static DBusGConnection *
get_dbus_connection (pam_handle_t *pamh, GMainLoop **ret_loop)
{
    DBusGConnection *connection;
    DBusConnection *conn;
    GMainLoop *loop;
    GMainContext *ctx;
    DBusError error;

    connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, NULL);

    if (connection != NULL)
            dbus_g_connection_unref (connection);

    dbus_error_init (&error);
    conn = dbus_bus_get_private (DBUS_BUS_SYSTEM, &error);
    if (conn == NULL) {
        D(pamh, "Error with getting the bus: %s", error.message);
        dbus_error_free (&error);
        return NULL;
    }

    ctx = g_main_context_new ();
    loop = g_main_loop_new (ctx, FALSE);
    dbus_connection_setup_with_g_main (conn, ctx);

    connection = dbus_connection_get_g_connection (conn);
    *ret_loop = loop;

    return connection;
}

static void 
verify_result(GObject *object, const char *result, gboolean done, gpointer user_data)
{
    verify_data *data = user_data;
    const char *msg;

    D(data->pamh, "Verify result: %s\n", result);
    if (done != FALSE) {
            data->result = g_strdup (result);
            g_main_loop_quit (data->loop);
            return;
    }
    send_info_msg (data->pamh, result);
}

static int 
do_verify(GMainLoop *loop, pam_handle_t *pamh, DBusGProxy *biometrics, const char *auth)
{
    verify_data *data;
    GError *error = NULL;
    int ret;

    data = g_new0 (verify_data, 1);
    data->pamh = pamh;
    data->loop = loop;
    data->result = NULL;

    dbus_g_proxy_add_signal(biometrics, 
		            "VerifyFprintStatus", 
			    G_TYPE_STRING, G_TYPE_BOOLEAN, NULL);
    dbus_g_proxy_connect_signal(biometrics, 
		               "VerifyFprintStatus", 
			       G_CALLBACK(verify_result),
                               data, NULL);
    ret = PAM_AUTH_ERR;
    D(data->pamh, "Verify id: %s\n", auth);
    if(!com_kylinsec_Kiran_SystemDaemon_Biometrics_verify_fprint_start (biometrics, auth, &error))
    {
        if (dbus_g_error_has_name(error, "com.kylinsec.Kiran.SystemDaemon.Biometrics.Error.NoEnrolledPrints"))
            ret = PAM_USER_UNKNOWN;
        D(pamh, "VerifyFprintStart failed: %s", error->message);
	send_info_msg (pamh,  error->message);
        g_error_free (error);

        g_free (data->result);
	g_free (data);
	return PAM_AUTH_ERR;
    }
    
    g_main_loop_run (loop);

    com_kylinsec_Kiran_SystemDaemon_Biometrics_verify_fprint_stop(biometrics, NULL);
    dbus_g_proxy_disconnect_signal(biometrics, "VerifyFprintStatus", G_CALLBACK(verify_result), data);

    if (g_str_equal (data->result, "Fingerprint not match!")) 
    {
        send_err_msg (data->pamh, data->result);
        ret = PAM_AUTH_ERR;
    } 
    else if (g_str_equal (data->result, "Fingerprint match!"))
    {
        ret = PAM_SUCCESS;
    }
    else if (g_str_equal (data->result, "verify-disconnected")) 
    {
        ret = PAM_AUTHINFO_UNAVAIL;
    } else 
    {
        send_err_msg (data->pamh, data->result);
        ret = PAM_AUTH_ERR;
    }

    g_free (data->result);
    g_free (data);

    return ret;
}

static void 
close_and_unref (DBusGConnection *connection)
{
    DBusConnection *conn;

    conn = dbus_g_connection_get_connection (connection);
    dbus_connection_close (conn);
    dbus_g_connection_unref (connection);
}

static void 
unref_loop (GMainLoop *loop)
{
    GMainContext *ctx;

    ctx = g_main_loop_get_context (loop);
    g_main_loop_unref (loop);
    g_main_context_unref (ctx);
}

static int 
do_auth(pam_handle_t *pamh, const char *username, const char *auth)
{
    DBusGConnection *connection;
    DBusGProxy *biometrics;
    DBusConnection *conn;
    GMainLoop *loop;
    char *rep;
    int ret;

    connection = get_dbus_connection (pamh, &loop);
    if (connection == NULL)
         return PAM_AUTHINFO_UNAVAIL;

    biometrics = dbus_g_proxy_new_for_name(connection,
                                        SERVICE_NAME,
                                        SERVICE_PATH,
                                        SERVICE_INTERFACE);
    if (biometrics == NULL)
    {
         D(pamh, "Error with connect the service: %s", SERVICE_NAME);
         return PAM_AUTHINFO_UNAVAIL;
    }

    //请求指纹人认证界面
    rep = request_respone(pamh,
                          PAM_PROMPT_ECHO_ON,
                          ASK_FPINT);
    if (rep && g_strcmp0(rep, REP_FPINT) == 0)
    {
	//认证界面准备完毕	
        ret = do_verify (loop, pamh, biometrics, auth);
    }
    else
    {
        ret =  PAM_AUTHINFO_UNAVAIL;
    }
   
    unref_loop (loop);
    g_object_unref (biometrics);
    close_and_unref (connection);

    return ret;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                   const char **argv)
{
    const char *rhost = NULL;
    const char *username;
    guint i;
    int r;
    const void *auth;

#if !GLIB_CHECK_VERSION (2, 36, 0)
    g_type_init();
#endif
    pam_get_item(pamh, PAM_RHOST, (const void **)(const void*) &rhost);

    if (rhost != NULL &&
        *rhost != '\0' &&
        strcmp (rhost, "localhost") != 0) {
            return PAM_AUTHINFO_UNAVAIL;
    }

    r = pam_get_user(pamh, &username, NULL);
    if (r != PAM_SUCCESS)
        return PAM_AUTHINFO_UNAVAIL;

    r = pam_get_data (pamh, FINGER_MODE, &auth);
    if (r == PAM_SUCCESS && auth != NULL)
    {
        if (g_strcmp0 (auth, NOT_NEED_DATA) == 0)
        {
	    return PAM_SUCCESS;
        }
    }

    r = do_auth (pamh, username, auth);

    return r;
}

int pam_sm_setcred(pam_handle_t *pamh, int flags,
                   int argc, const char **argv)
{
    return PAM_SUCCESS;
}

/* Account Management API's */
int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags,
                     int argc, const char **argv)
{
    return PAM_SUCCESS;
}

/* Session Management API's */
int pam_sm_open_session(pam_handle_t *pamh, int flags,
                        int argc, const char **argv)
{
    return PAM_SUCCESS;
}

int pam_sm_close_session(pam_handle_t *pamh, int flags,
                         int argc, const char **argv)
{
    return PAM_SUCCESS;
}

/* Password Management API's */
int pam_sm_chauthtok(pam_handle_t *pamh, int flags,
                     int argc, const char **argv)
{
    return PAM_SUCCESS;
}
