/**
 * Copyright (c) 2020 ~ 2021 KylinSec Co., Ltd. 
 * kiran-cc-daemon is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2. 
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2 
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, 
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, 
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.  
 * See the Mulan PSL v2 for more details.  
 * 
 * Author:     wangxiaoqing <wangxiaoqing@kylinos.com.cn>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib-lowlevel.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>
#include "json-glib/json-glib.h"
#include "kiran-accounts-gen.h"
#include "kiran-system-daemon/accounts-i.h"
#include "kiran-pam.h"
#include "kiran-user-gen.h"

static char *
parser_auth_items_json_data(pam_handle_t *pamh, char *data)
{
    JsonParser *jparse = json_parser_new();
    JsonNode *root;
    JsonReader *reader;
    GError *error = NULL;
    char *id = NULL;
    gboolean ret;

    ret = json_parser_load_from_data(jparse,
                                     data,
                                     -1,
                                     &error);
    if (!ret)
    {
        D(pamh, "Error with parse json data: %s", error->message);
        return NULL;
    }

    root = json_parser_get_root(jparse);
    if (json_node_get_node_type(root) == JSON_NODE_ARRAY)
    {
        JsonArray *array = json_node_get_array(root);
        GList *list = json_array_get_elements(array);
        GList *iter;

        reader = json_reader_new(NULL);
        for (iter = list; iter; iter = iter->next)
        {
            const gchar *data_id;

            json_reader_set_root(reader, iter->data);
            json_reader_read_member(reader, "data_id");
            data_id = json_reader_get_string_value(reader);
            if (data_id)
            {
                id = strdup(data_id);
                break;
            }
        }
        g_object_unref(reader);
    }

    g_object_unref(jparse);

    return id;
}

static void
data_cleanup(pam_handle_t *pamh, void *data, int error_status)
{
    g_free(data);
}

static char *
get_auth_item(KiranAccountsUser *user,
              pam_handle_t *pamh,
              int mod)
{
    GError *error;
    gchar *auth_items = NULL;
    gchar *auth = NULL;
    int ret;

    error = NULL;
    ret = kiran_accounts_user_call_get_auth_items_sync(user,
                                                       mod,
                                                       &auth_items,
                                                       NULL,
                                                       &error);
    if (!ret || !auth_items)
    {
        D(pamh, "Error with getting the auth item: %s", error->message);
        g_error_free(error);
        auth = g_strdup(NEED_DATA);
    }
    else
    {
        char *id;
        id = parser_auth_items_json_data(pamh, auth_items);
        if (id)
        {
            auth = id;
            D(pamh, "Getting the auth id: %s", id);
        }
        else
        {
            auth = g_strdup(NEED_DATA);
        }
    }

    return auth;
}

static void
do_authmode_set(pam_handle_t *pamh, const char *username)
{
    GDBusConnection *connection;
    KiranAccounts *accounts;
    KiranAccountsUser *user;
    GError *error;
    gchar *path;
    gchar *auth;
    gboolean ret;
    int authmode;

    error = NULL;
    connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (connection == NULL)
    {
        D(pamh, "Error with getting the bus: %s", error->message);
        g_error_free(error);
        return;
    }

    error = NULL;
    accounts = kiran_accounts_proxy_new_sync(connection,
                                             G_BUS_NAME_WATCHER_FLAGS_NONE,
                                             "com.kylinsec.Kiran.SystemDaemon.Accounts",
                                             "/com/kylinsec/Kiran/SystemDaemon/Accounts",
                                             NULL,
                                             &error);
    if (accounts == NULL)
    {
        D(pamh, "Error with getting the bus: %s", error->message);
        g_object_unref(connection);
        g_error_free(error);
        return;
    }

    path = NULL;
    error = NULL;
    ret = kiran_accounts_call_find_user_by_name_sync(accounts,
                                                     username,
                                                     &path,
                                                     NULL,
                                                     &error);
    if (!ret)
    {
        D(pamh, "Error with find the user object path: %s", error->message);
        g_error_free(error);
        g_object_unref(accounts);
        g_object_unref(connection);
        return;
    }

    error = NULL;
    user = kiran_accounts_user_proxy_new_sync(connection,
                                              G_BUS_NAME_WATCHER_FLAGS_NONE,
                                              "com.kylinsec.Kiran.SystemDaemon.Accounts",
                                              path,
                                              NULL,
                                              &error);
    g_free(path);

    if (user == NULL)
    {
        D(pamh, "Error with getting the bus: %s", error->message);
        g_error_free(error);
        g_object_unref(accounts);
        g_object_unref(connection);
        return;
    }

    authmode = kiran_accounts_user_get_auth_modes(user);

    //指纹认证
    if (authmode & ACCOUNTS_AUTH_MODE_FINGERPRINT)
        auth = get_auth_item(user, pamh, ACCOUNTS_AUTH_MODE_FINGERPRINT);
    else
        auth = g_strdup(NOT_NEED_DATA);
    pam_set_data(pamh, FINGER_MODE, auth, data_cleanup);

    //密码认证
    if (authmode & ACCOUNTS_AUTH_MODE_PASSWORD)
        auth = g_strdup(NEED_DATA);
    else
        auth = g_strdup(NOT_NEED_DATA);
    pam_set_data(pamh, PASSWD_MODE, auth, data_cleanup);

    //人脸认证
    if (authmode & ACCOUNTS_AUTH_MODE_FACE)
        auth = get_auth_item(user, pamh, ACCOUNTS_AUTH_MODE_FACE);
    else
        auth = g_strdup(NOT_NEED_DATA);
    pam_set_data(pamh, FACE_MODE, auth, data_cleanup);

    g_object_unref(user);
    g_object_unref(accounts);
    g_object_unref(connection);
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                   const char **argv)
{
    const char *rhost = NULL;
    const char *username;
    guint i;
    int r;

#if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
#endif
    pam_get_item(pamh, PAM_RHOST, (const void **)(const void *)&rhost);

    if (rhost != NULL &&
        *rhost != '\0' &&
        strcmp(rhost, "localhost") != 0)
    {
        return PAM_AUTHINFO_UNAVAIL;
    }

    r = pam_get_user(pamh, &username, NULL);
    if (r != PAM_SUCCESS)
        return PAM_AUTHINFO_UNAVAIL;

    do_authmode_set(pamh, username);

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
