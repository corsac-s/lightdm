/*
 * Copyright (C) 2010 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option) any
 * later version. See http://www.gnu.org/copyleft/lgpl.html the full text of the
 * license.
 */

#include <config.h>

#include <errno.h>
#include <string.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <gio/gio.h>

#include "lightdm/user-list.h"

enum {
    PROP_0,
    PROP_NUM_USERS,
    PROP_USERS,
};

enum {
    USER_ADDED,
    USER_CHANGED,
    USER_REMOVED,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

typedef struct
{
    /* Connection to AccountsService */
    GDBusProxy *accounts_service_proxy;
    GList *user_account_objects;

    /* File monitor for password file */
    GFileMonitor *passwd_monitor;
  
    /* TRUE if have scanned users */
    gboolean have_users;

    /* List of users */
    GList *users;
} LightDMUserListPrivate;

typedef struct
{
    GDBusProxy *proxy;
    LightDMUser *user;
} UserAccountObject;

G_DEFINE_TYPE (LightDMUserList, lightdm_user_list, G_TYPE_OBJECT);

#define GET_PRIVATE(obj) G_TYPE_INSTANCE_GET_PRIVATE ((obj), LIGHTDM_TYPE_USER_LIST, LightDMUserListPrivate)

#define PASSWD_FILE      "/etc/passwd"
#define USER_CONFIG_FILE "/etc/lightdm/users.conf"

/**
 * lightdm_user_list_new:
 *
 * Create a new user list.
 *
 * Return value: the new #LightDMUserList
 **/
LightDMUserList *
lightdm_user_list_new ()
{
    return g_object_new (LIGHTDM_TYPE_USER_LIST, NULL);
}

static LightDMUser *
get_user_by_name (LightDMUserList *user_list, const gchar *username)
{
    LightDMUserListPrivate *priv = GET_PRIVATE (user_list);
    GList *link;
  
    for (link = priv->users; link; link = link->next)
    {
        LightDMUser *user = link->data;
        if (strcmp (lightdm_user_get_name (user), username) == 0)
            return user;
    }

    return NULL;
}
  
static gint
compare_user (gconstpointer a, gconstpointer b)
{
    LightDMUser *user_a = (LightDMUser *) a, *user_b = (LightDMUser *) b;
    return strcmp (lightdm_user_get_display_name (user_a), lightdm_user_get_display_name (user_b));
}

static gboolean
update_passwd_user (LightDMUser *user, const gchar *real_name, const gchar *home_directory, const gchar *image, gboolean logged_in)
{
    if (g_strcmp0 (lightdm_user_get_real_name (user), real_name) == 0 &&
        g_strcmp0 (lightdm_user_get_home_directory (user), home_directory) == 0 &&
        g_strcmp0 (lightdm_user_get_image (user), image) == 0 &&
        lightdm_user_get_logged_in (user) == logged_in)
        return FALSE;

    g_object_set (user, "real-name", real_name, "home-directory", home_directory, "image", image, "logged-in", logged_in, NULL);

    return TRUE;
}

static void
user_changed_cb (LightDMUser *user, LightDMUserList *user_list)
{
    g_signal_emit (user_list, signals[USER_CHANGED], 0, user);
}

static void
load_passwd_file (LightDMUserList *user_list)
{
    LightDMUserListPrivate *priv = GET_PRIVATE (user_list);
    GKeyFile *config;
    gchar *value;
    gint minimum_uid;
    gchar **hidden_users, **hidden_shells;
    GList *users = NULL, *old_users, *new_users = NULL, *changed_users = NULL, *link;
    GError *error = NULL;

    g_debug ("Loading user config from %s", USER_CONFIG_FILE);

    config = g_key_file_new ();
    if (!g_key_file_load_from_file (config, USER_CONFIG_FILE, G_KEY_FILE_NONE, &error) &&
        !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Failed to load configuration from %s: %s", USER_CONFIG_FILE, error->message); // FIXME: Don't make warning on no file, just info
    g_clear_error (&error);

    if (g_key_file_has_key (config, "UserList", "minimum-uid", NULL))
        minimum_uid = g_key_file_get_integer (config, "UserList", "minimum-uid", NULL);
    else
        minimum_uid = 500;

    value = g_key_file_get_string (config, "UserList", "hidden-users", NULL);
    if (!value)
        value = g_strdup ("nobody nobody4 noaccess");
    hidden_users = g_strsplit (value, " ", -1);
    g_free (value);

    value = g_key_file_get_string (config, "UserList", "hidden-shells", NULL);
    if (!value)
        value = g_strdup ("/bin/false /usr/sbin/nologin");
    hidden_shells = g_strsplit (value, " ", -1);
    g_free (value);

    g_key_file_free (config);

    setpwent ();

    while (TRUE)
    {
        struct passwd *entry;
        LightDMUser *user;
        char **tokens;
        gchar *real_name, *image;
        int i;

        errno = 0;
        entry = getpwent ();
        if (!entry)
            break;

        /* Ignore system users */
        if (entry->pw_uid < minimum_uid)
            continue;

        /* Ignore users disabled by shell */
        if (entry->pw_shell)
        {
            for (i = 0; hidden_shells[i] && strcmp (entry->pw_shell, hidden_shells[i]) != 0; i++);
            if (hidden_shells[i])
                continue;
        }

        /* Ignore certain users */
        for (i = 0; hidden_users[i] && strcmp (entry->pw_name, hidden_users[i]) != 0; i++);
        if (hidden_users[i])
            continue;

        tokens = g_strsplit (entry->pw_gecos, ",", -1);
        if (tokens[0] != NULL && tokens[0][0] != '\0')
            real_name = g_strdup (tokens[0]);
        else
            real_name = NULL;
        g_strfreev (tokens);
      
        image = g_build_filename (entry->pw_dir, ".face", NULL);
        if (!g_file_test (image, G_FILE_TEST_EXISTS))
        {
            g_free (image);
            image = g_build_filename (entry->pw_dir, ".face.icon", NULL);
            if (!g_file_test (image, G_FILE_TEST_EXISTS))
            {
                g_free (image);
                image = NULL;
            }
        }

        user = g_object_new (LIGHTDM_TYPE_USER, "name", entry->pw_name, "real-name", real_name, "home-directory", entry->pw_dir, "image", image, "logged-in", FALSE, NULL);
        g_free (real_name);
        g_free (image);

        /* Update existing users if have them */
        for (link = priv->users; link; link = link->next)
        {
            LightDMUser *info = link->data;
            if (strcmp (lightdm_user_get_name (info), lightdm_user_get_name (user)) == 0)
            {
                if (update_passwd_user (info, lightdm_user_get_real_name (user), lightdm_user_get_home_directory (user), lightdm_user_get_image (user), lightdm_user_get_logged_in (user)))
                    changed_users = g_list_insert_sorted (changed_users, info, compare_user);
                g_object_unref (user);
                user = info;
                break;
            }
        }
        if (!link)
        {
            /* Only notify once we have loaded the user list */
            if (priv->have_users)
                new_users = g_list_insert_sorted (new_users, user, compare_user);
        }
        users = g_list_insert_sorted (users, user, compare_user);
    }
    g_strfreev (hidden_users);
    g_strfreev (hidden_shells);

    if (errno != 0)
        g_warning ("Failed to read password database: %s", strerror (errno));

    endpwent ();

    /* Use new user list */
    old_users = priv->users;
    priv->users = users;
  
    /* Notify of changes */
    for (link = new_users; link; link = link->next)
    {
        LightDMUser *info = link->data;
        g_debug ("User %s added", lightdm_user_get_name (info));
        g_signal_connect (info, "changed", G_CALLBACK (user_changed_cb), user_list);
        g_signal_emit (user_list, signals[USER_ADDED], 0, info);
    }
    g_list_free (new_users);
    for (link = changed_users; link; link = link->next)
    {
        LightDMUser *info = link->data;
        g_debug ("User %s changed", lightdm_user_get_name (info));
        g_signal_emit_by_name (info, "changed");
    }
    g_list_free (changed_users);
    for (link = old_users; link; link = link->next)
    {
        GList *new_link;

        /* See if this user is in the current list */
        for (new_link = priv->users; new_link; new_link = new_link->next)
        {
            if (new_link->data == link->data)
                break;
        }

        if (!new_link)
        {
            LightDMUser *info = link->data;
            g_debug ("User %s removed", lightdm_user_get_name (info));
            g_signal_emit (user_list, signals[USER_REMOVED], 0, info);
            g_object_unref (info);
        }
    }
    g_list_free (old_users);
}

static void
passwd_changed_cb (GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, LightDMUserList *user_list)
{
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    {
        g_debug ("%s changed, reloading user list", g_file_get_path (file));
        load_passwd_file (user_list);
    }
}

static gboolean
update_user (UserAccountObject *object)
{
    GVariant *result, *value;
    GVariantIter *iter;
    gchar *name;
    GError *error = NULL;

    result = g_dbus_connection_call_sync (g_dbus_proxy_get_connection (object->proxy),
                                          "org.freedesktop.Accounts",
                                          g_dbus_proxy_get_object_path (object->proxy),
                                          "org.freedesktop.DBus.Properties",
                                          "GetAll",
                                          g_variant_new ("(s)", "org.freedesktop.Accounts.User"),
                                          G_VARIANT_TYPE ("(a{sv})"),
                                          G_DBUS_CALL_FLAGS_NONE,
                                          -1,
                                          NULL,
                                          &error);
    if (!result)
        g_warning ("Error updating user %s: %s", g_dbus_proxy_get_object_path (object->proxy), error->message);
    g_clear_error (&error);
    if (!result)
        return FALSE;

    g_variant_get (result, "(a{sv})", &iter);
    while (g_variant_iter_loop (iter, "{&sv}", &name, &value))
    {
        g_debug ("%s=?", name);
        if (strcmp (name, "UserName") == 0 && g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
        {
            gchar *user_name;
            g_variant_get (value, "&s", &user_name);
            g_object_set (object->user, "name", user_name, NULL);
        }
        else if (strcmp (name, "RealName") == 0 && g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
        {
            gchar *real_name;
            g_variant_get (value, "&s", &real_name);
            g_object_set (object->user, "real-name", real_name, NULL);
        }
        else if (strcmp (name, "HomeDirectory") == 0 && g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
        {
            gchar *home_directory;
            g_variant_get (value, "&s", &home_directory);
            g_object_set (object->user, "home-directory", home_directory, NULL);
        }
        else if (strcmp (name, "IconFile") == 0 && g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
        {
            gchar *icon_file;
            g_variant_get (value, "&s", &icon_file);
            g_object_set (object->user, "image", icon_file, NULL);
        }
    }
    g_debug ("!1");
    g_variant_iter_free (iter);
    g_debug ("!2");

    g_variant_unref (result);
    g_debug ("!3");

    return TRUE;
}

static void
user_signal_cb (GDBusProxy *proxy, gchar *sender_name, gchar *signal_name, GVariant *parameters, UserAccountObject *object)
{
    if (strcmp (signal_name, "Changed") == 0)
    {
        if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("()")))
        {
            g_debug ("User %s changed", g_dbus_proxy_get_object_path (object->proxy));
            update_user (object);
            g_signal_emit_by_name (object->user, "changed");
        }
        else
            g_warning ("Got org.freedesktop.Accounts.User signal Changed with unknown parameters %s", g_variant_get_type_string (parameters));
    }
}

static UserAccountObject *
user_account_object_new (const gchar *path)
{
    GDBusProxy *proxy;
    UserAccountObject *object;
    GError *error = NULL;

    proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                           G_DBUS_PROXY_FLAGS_NONE,
                                           NULL,
                                           "org.freedesktop.Accounts",
                                           path,
                                           "org.freedesktop.Accounts.User",
                                           NULL,
                                           &error);
    if (!proxy)
        g_warning ("Error getting user %s: %s", path, error->message);
    g_clear_error (&error);
    if (!proxy)
        return NULL;

    object = g_malloc0 (sizeof (UserAccountObject));  
    object->user = g_object_new (LIGHTDM_TYPE_USER, NULL);
    object->proxy = proxy;
    g_signal_connect (proxy, "g-signal", G_CALLBACK (user_signal_cb), object);
  
    return object;
}

static void
user_account_object_free (UserAccountObject *object)
{
    if (!object)
        return;
    g_object_unref (object->user);
    g_object_unref (object->proxy);
    g_free (object);
}

static void
user_accounts_signal_cb (GDBusProxy *proxy, gchar *sender_name, gchar *signal_name, GVariant *parameters, LightDMUserList *user_list)
{
    LightDMUserListPrivate *priv = GET_PRIVATE (user_list);
  
    if (strcmp (signal_name, "UserAdded") == 0)
    {
        if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(o)")))
        {
            gchar *path;
            UserAccountObject *object;

            g_variant_get (parameters, "(&o)", &path);
            g_debug ("User %s added", path);

            object = user_account_object_new (path);
            if (object && update_user (object))
            {
                priv->user_account_objects = g_list_append (priv->user_account_objects, object);
                priv->users = g_list_insert_sorted (priv->users, g_object_ref (object->user), compare_user);
                g_signal_connect (object->user, "changed", G_CALLBACK (user_changed_cb), user_list);
                g_signal_emit (user_list, signals[USER_ADDED], 0, object->user);
            }
            else
                user_account_object_free (object);
        }
        else
            g_warning ("Got UserAccounts signal UserAdded with unknown parameters %s", g_variant_get_type_string (parameters));
    }
    else if (strcmp (signal_name, "UserDeleted") == 0)
    {
        if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(o)")))
        {
            gchar *path;
            GList *link;

            g_variant_get (parameters, "(&o)", &path);
            g_debug ("User %s deleted", path);

            for (link = priv->user_account_objects; link; link = link->next)
            {
                UserAccountObject *object = link->data;
                if (strcmp (g_dbus_proxy_get_object_path (object->proxy), path) == 0)
                {
                    priv->users = g_list_remove (priv->users, object->user);
                    g_object_unref (object->user);

                    g_signal_emit (user_list, signals[USER_REMOVED], 0, object->user);

                    priv->user_account_objects = g_list_remove (priv->user_account_objects, object);
                    user_account_object_free (object);
                    break;
                }
            }
        }
        else
            g_warning ("Got UserAccounts signal UserDeleted with unknown parameters %s", g_variant_get_type_string (parameters));
    }
}

static void
update_users (LightDMUserList *user_list)
{
    LightDMUserListPrivate *priv = GET_PRIVATE (user_list);
    GFile *passwd_file;
    GError *error = NULL;

    if (priv->have_users)
        return;
    priv->have_users = TRUE;

    priv->accounts_service_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                                  NULL,
                                                                  "org.freedesktop.Accounts",
                                                                  "/org/freedesktop/Accounts",
                                                                  "org.freedesktop.Accounts",
                                                                  NULL,
                                                                  &error);
    if (!priv->accounts_service_proxy)
        g_warning ("Error contacting AccountsService: %s", error->message);
    g_clear_error (&error);

    if (priv->accounts_service_proxy)
    {
        GVariant *result;

        g_signal_connect (priv->accounts_service_proxy, "g-signal", G_CALLBACK (user_accounts_signal_cb), user_list);

        result = g_dbus_proxy_call_sync (priv->accounts_service_proxy,
                                         "ListCachedUsers",
                                         g_variant_new ("()"),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);
        if (!result)
            g_warning ("Error getting user list from AccountsService: %s", error->message);
        g_clear_error (&error);
        if (!result)
            return;

        if (g_variant_is_of_type (result, G_VARIANT_TYPE ("(ao)")))
        {
            GVariantIter *iter;
            const gchar *path;

            g_debug ("Loading users from AccountsService");
            g_variant_get (result, "(ao)", &iter);
            while (g_variant_iter_loop (iter, "&o", &path))
            {
                UserAccountObject *object;

                g_debug ("Loading user %s", path);

                object = user_account_object_new (path);
                if (object && update_user (object))
                {
                    priv->user_account_objects = g_list_append (priv->user_account_objects, object);
                    priv->users = g_list_insert_sorted (priv->users, g_object_ref (object->user), compare_user);
                    g_signal_connect (object->user, "changed", G_CALLBACK (user_changed_cb), user_list);
                }
                else
                    user_account_object_free (object);
            }
            g_variant_iter_free (iter);
        }
        else
            g_warning ("Unexpected type from ListCachedUsers: %s", g_variant_get_type_string (result));

        g_variant_unref (result);
    }
    else
    {
        load_passwd_file (user_list);

        /* Watch for changes to user list */
        passwd_file = g_file_new_for_path (PASSWD_FILE);
        priv->passwd_monitor = g_file_monitor (passwd_file, G_FILE_MONITOR_NONE, NULL, &error);
        g_object_unref (passwd_file);
        if (!priv->passwd_monitor)
            g_warning ("Error monitoring %s: %s", PASSWD_FILE, error->message);
        else
            g_signal_connect (priv->passwd_monitor, "changed", G_CALLBACK (passwd_changed_cb), user_list);
        g_clear_error (&error);
    }
}

/**
 * lightdm_user_list_get_length:
 * @user_list: a #LightDMUserList
 *
 * Return value: The number of users able to log in
 **/
gint
lightdm_user_list_get_length (LightDMUserList *user_list)
{
    g_return_val_if_fail (LIGHTDM_IS_USER_LIST (user_list), 0);
    update_users (user_list);
    return g_list_length (GET_PRIVATE (user_list)->users);
}

/**
 * lightdm_user_list_get_users:
 * @user_list: A #LightDMUserList
 *
 * Get a list of users to present to the user.  This list may be a subset of the
 * available users and may be empty depending on the server configuration.
 *
 * Return value: (element-type LightDMUser) (transfer none): A list of #LightDMUser that should be presented to the user.
 **/
GList *
lightdm_user_list_get_users (LightDMUserList *user_list)
{
    g_return_val_if_fail (LIGHTDM_IS_USER_LIST (user_list), NULL);
    update_users (user_list);
    return GET_PRIVATE (user_list)->users;
}

/**
 * lightdm_user_list_get_user_by_name:
 * @user_list: A #LightDMUserList
 * @username: Name of user to get.
 *
 * Get infomation about a given user or #NULL if this user doesn't exist.
 *
 * Return value: (transfer none): A #LightDMUser entry for the given user.
 **/
LightDMUser *
lightdm_user_list_get_user_by_name (LightDMUserList *user_list, const gchar *username)
{
    g_return_val_if_fail (LIGHTDM_IS_USER_LIST (user_list), NULL);
    g_return_val_if_fail (username != NULL, NULL);

    update_users (user_list);

    return get_user_by_name (user_list, username);
}

static void
lightdm_user_list_init (LightDMUserList *user_list)
{
}

static void
lightdm_user_list_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
lightdm_user_list_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    LightDMUserList *self;

    self = LIGHTDM_USER_LIST (object);

    switch (prop_id) {
    case PROP_NUM_USERS:
        g_value_set_int (value, lightdm_user_list_get_length (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
lightdm_user_list_finalize (GObject *object)
{
    LightDMUserList *self = LIGHTDM_USER_LIST (object);
    LightDMUserListPrivate *priv = GET_PRIVATE (self);

    if (priv->accounts_service_proxy)
        g_object_unref (priv->accounts_service_proxy);
    g_list_free_full (priv->user_account_objects, (GDestroyNotify) user_account_object_free);
    if (priv->passwd_monitor)
        g_object_unref (priv->passwd_monitor);
    g_list_free_full (priv->users, g_object_unref);

    G_OBJECT_CLASS (lightdm_user_list_parent_class)->finalize (object);
}

static void
lightdm_user_list_class_init (LightDMUserListClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (klass, sizeof (LightDMUserListPrivate));

    object_class->set_property = lightdm_user_list_set_property;
    object_class->get_property = lightdm_user_list_get_property;
    object_class->finalize = lightdm_user_list_finalize;

    g_object_class_install_property (object_class,
                                     PROP_NUM_USERS,
                                     g_param_spec_int ("num-users",
                                                       "num-users",
                                                       "Number of login users",
                                                       0, G_MAXINT, 0,
                                                       G_PARAM_READABLE));
    /**
     * LightDMUserList::user-added:
     * @user_list: A #LightDMUserList
     * @user: The #LightDM user that has been added.
     *
     * The ::user-added signal gets emitted when a user account is created.
     **/
    signals[USER_ADDED] =
        g_signal_new ("user-added",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMUserListClass, user_added),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, LIGHTDM_TYPE_USER);

    /**
     * LightDMUserList::user-changed:
     * @user_list: A #LightDMUserList
     * @user: The #LightDM user that has been changed.
     *
     * The ::user-changed signal gets emitted when a user account is modified.
     **/
    signals[USER_CHANGED] =
        g_signal_new ("user-changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMUserListClass, user_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, LIGHTDM_TYPE_USER);

    /**
     * LightDMUserList::user-removed:
     * @user_list: A #LightDMUserList
     * @user: The #LightDM user that has been removed.
     *
     * The ::user-removed signal gets emitted when a user account is removed.
     **/
    signals[USER_REMOVED] =
        g_signal_new ("user-removed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMUserListClass, user_removed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, LIGHTDM_TYPE_USER);
}