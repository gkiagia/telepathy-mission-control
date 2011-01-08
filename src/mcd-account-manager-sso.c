/*
 * A pseudo-plugin that stores/fetches accounts in/from the SSO via libaccounts
 *
 * Copyright © 2010 Nokia Corporation
 * Copyright © 2010 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "mcd-account-manager-sso.h"
#include "mcd-debug.h"

#include <telepathy-glib/util.h>

#include <string.h>
#include <ctype.h>

/* IMPORTANT IMPLEMENTATION NOTE:
 *
 * Note for implementors: save_param is for saving account parameters (in MC
 * terms) - anything that ends up stored as "param-" in the standard gkeyfile
 * save_value is for everything else.
 *
 * Whether such a value is stored in the global section of an SSO account or
 * in the IM specific section is orthogonal to the above, and in the mapping
 * is not necessarily from MC "name" to SSO "name", or from MC "param-name"
 * to SSO "parameters/name" - so be careful when making such decisions.
 *
 * The existing mappings have been arrived at empirically.
 */

#define PLUGIN_PRIORITY (MCP_ACCOUNT_STORAGE_PLUGIN_PRIO_KEYRING + 10)
#define PLUGIN_NAME "maemo-libaccounts"
#define PLUGIN_DESCRIPTION \
  "Account storage in the Maemo SSO store via libaccounts-glib API"
#define PLUGIN_PROVIDER "org.maemo.Telepathy.Account.Storage.LibAccounts"

#define MCPP "param-"
#define AGPP "parameters/"
#define LIBACCT_ID_KEY  "libacct-uid"

#define MC_ENABLED_KEY "Enabled"
#define AG_ENABLED_KEY "enabled"

#define AG_LABEL_KEY   "name"
#define MC_LABEL_KEY   "DisplayName"

#define AG_ACCOUNT_KEY "username"
#define MC_ACCOUNT_KEY "account"
#define PASSWORD_KEY   "password"
#define AG_ACCOUNT_ALT_KEY AGPP "account"

#define MC_CMANAGER_KEY "manager"
#define MC_PROTOCOL_KEY "protocol"
#define MC_IDENTITY_KEY "tmc-uid"

#define SERVICES_KEY    "sso-services"

#define MC_SERVICE_KEY  "Service"

static const gchar *exported_settings[] = { "CredentialsId", NULL };

typedef enum {
  DELAYED_CREATE,
  DELAYED_DELETE,
} DelayedSignal;

typedef struct {
  gchar *mc_name;
  gchar *ag_name;
  gboolean global;   /* global ag setting or service specific? */
  gboolean readable; /* does the _standard_ read method copy this into MC?  */
  gboolean writable; /* does the _standard_ write method copy this into AG? */
  gboolean freeable; /* should clear_setting_data deallocate the names? */
} Setting;

#define GLOBAL     TRUE
#define SERVICE    FALSE
#define READABLE   TRUE
#define UNREADABLE FALSE
#define WRITABLE   TRUE
#define UNWRITABLE FALSE

typedef enum {
  SETTING_MC,
  SETTING_AG,
} SettingType;

Setting setting_map[] = {
  { MC_ENABLED_KEY     , AG_ENABLED_KEY , GLOBAL , UNREADABLE, UNWRITABLE },
  { MCPP MC_ACCOUNT_KEY, AG_ACCOUNT_KEY , GLOBAL , READABLE  , UNWRITABLE },
  { MCPP PASSWORD_KEY  , PASSWORD_KEY   , GLOBAL , READABLE  , WRITABLE   },
  { MC_LABEL_KEY       , AG_LABEL_KEY   , GLOBAL , READABLE  , WRITABLE   },
  { LIBACCT_ID_KEY     , LIBACCT_ID_KEY , GLOBAL , UNREADABLE, UNWRITABLE },
  { MC_IDENTITY_KEY    , MC_IDENTITY_KEY, SERVICE, READABLE  , WRITABLE   },
  { MC_CMANAGER_KEY    , MC_CMANAGER_KEY, SERVICE, READABLE  , UNWRITABLE },
  { MC_PROTOCOL_KEY    , MC_PROTOCOL_KEY, SERVICE, READABLE  , UNWRITABLE },
  { MC_SERVICE_KEY     , MC_SERVICE_KEY , SERVICE, UNREADABLE, UNWRITABLE },
  { SERVICES_KEY       , SERVICES_KEY   , GLOBAL , UNREADABLE, UNWRITABLE },
  { NULL }
};

typedef struct {
  DelayedSignal signal;
  AgAccountId account_id;
} DelayedSignalData;

typedef struct {
  gchar *mc_key;
  McdAccountManagerSso *sso;
  AgAccountWatch watch;
} WatchData;

static Setting *
setting_data (const gchar *name, SettingType type)
{
  guint i = 0;
  static Setting parameter = { NULL, NULL, SERVICE, READABLE, WRITABLE, TRUE };
  const gchar *prefix;

  for (; setting_map[i].mc_name != NULL; i++)
    {
      const gchar *setting_name = NULL;

      if (type == SETTING_MC)
        setting_name = setting_map[i].mc_name;
      else
        setting_name = setting_map[i].ag_name;

      if (g_strcmp0 (name, setting_name) == 0)
        return &setting_map[i];
    }

  prefix = (type == SETTING_MC) ? MCPP : AGPP;

  if (!g_str_has_prefix (name, prefix))
    { /* a non-parameter setting */
      parameter.mc_name = g_strdup (name);
      parameter.ag_name = g_strdup (name);
    }
  else
    { /* a setting that is a parameter on both sides (AG & MC) */
      const guint plength = strlen (prefix);

      parameter.mc_name = g_strdup_printf ("%s%s", MCPP, name + plength);
      parameter.ag_name = g_strdup_printf ("%s%s", AGPP, name + plength);
    }

  return &parameter;
}

static void
clear_setting_data (Setting *setting)
{
  if (setting == NULL)
    return;

  if (!setting->freeable)
    return;

  g_free (setting->mc_name);
  g_free (setting->ag_name);
  setting->mc_name = NULL;
  setting->ag_name = NULL;
}

static gboolean _sso_account_enabled (
    McdAccountManagerSso *self,
    AgAccount *account,
    AgService *service);

static void account_storage_iface_init (McpAccountStorageIface *,
    gpointer);

static gchar *
_ag_accountid_to_mc_key (McdAccountManagerSso *sso,
    AgAccountId id,
    gboolean create);

static void _ag_account_stored_cb (AgAccount *acct,
    const GError *err,
    gpointer ignore);

static gboolean save_setting (
    McdAccountManagerSso *self,
    AgAccount *account,
    const Setting *setting,
    const gchar *val);

G_DEFINE_TYPE_WITH_CODE (McdAccountManagerSso, mcd_account_manager_sso,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (MCP_TYPE_ACCOUNT_STORAGE,
        account_storage_iface_init));

static gchar *
_gvalue_to_string (const GValue *val)
{
  switch (G_VALUE_TYPE (val))
    {
      case G_TYPE_STRING:
        return g_value_dup_string (val);
      case G_TYPE_BOOLEAN:
        return g_strdup (g_value_get_boolean (val) ? "true" : "false");
      case G_TYPE_CHAR:
        return g_strdup_printf ("%c", g_value_get_uchar (val));
      case G_TYPE_UCHAR:
        return g_strdup_printf ("%c", g_value_get_char (val));
      case G_TYPE_INT:
        return g_strdup_printf ("%i", g_value_get_int (val));
      case G_TYPE_UINT:
        return g_strdup_printf ("%u", g_value_get_uint (val));
      case G_TYPE_LONG:
        return g_strdup_printf ("%ld", g_value_get_long (val));
      case G_TYPE_ULONG:
        return g_strdup_printf ("%lu", g_value_get_ulong (val));
      case G_TYPE_INT64:
        return g_strdup_printf ("%" G_GINT64_FORMAT, g_value_get_int64 (val));
      case G_TYPE_UINT64:
        return g_strdup_printf ("%" G_GUINT64_FORMAT, g_value_get_uint64 (val));
      case G_TYPE_ENUM:
        return g_strdup_printf ("%d" , g_value_get_enum (val));
      case G_TYPE_FLAGS:
        return g_strdup_printf ("%u", g_value_get_flags (val));
      case G_TYPE_FLOAT:
        return g_strdup_printf ("%f", g_value_get_float (val));
      case G_TYPE_DOUBLE:
        return g_strdup_printf ("%g", g_value_get_double (val));
      default:
        DEBUG ("Unsupported type %s", G_VALUE_TYPE_NAME (val));
        return NULL;
    }
}

static const gchar *
account_manager_sso_get_service_type (McdAccountManagerSso *self)
{
  McdAccountManagerSsoClass *klass = MCD_ACCOUNT_MANAGER_SSO_GET_CLASS (self);

  g_assert (klass->service_type != NULL);

  return klass->service_type;
}

static gboolean
_ag_account_select_default_im_service (
    McdAccountManagerSso *self,
    AgAccount *account)
{
  const gchar *service_type = account_manager_sso_get_service_type (self);
  gboolean have_service = FALSE;
  GList *first = ag_account_list_services_by_type (account, service_type);

  if (first != NULL && first->data != NULL)
    {
      have_service = TRUE;
      DEBUG ("default %s service %s", service_type,
          ag_service_get_name (first->data));
      ag_account_select_service (account, first->data);
    }

  ag_service_list_free (first);

  return have_service;
}

static AgSettingSource
_ag_account_global_value (AgAccount *account,
    const gchar *key,
    GValue *value)
{
  AgSettingSource src = AG_SETTING_SOURCE_NONE;
  AgService *service = ag_account_get_selected_service (account);

  if (service != NULL)
    {
      ag_account_select_service (account, NULL);
      src = ag_account_get_value (account, key, value);
      ag_account_select_service (account, service);
    }
  else
    {
      src = ag_account_get_value (account, key, value);
    }

  return src;
}

static AgSettingSource
_ag_account_local_value (
    McdAccountManagerSso *self,
    AgAccount *account,
    const gchar *key,
    GValue *value)
{
  AgSettingSource src = AG_SETTING_SOURCE_NONE;
  AgService *service = ag_account_get_selected_service (account);

  if (service != NULL)
    {
      src = ag_account_get_value (account, key, value);
    }
  else
    {
      _ag_account_select_default_im_service (self, account);
      src = ag_account_get_value (account, key, value);
      ag_account_select_service (account, NULL);
    }

  return src;
}

/* AG_ACCOUNT_ALT_KEY from service overrides global AG_ACCOUNT_KEY if set */
static void
_maybe_set_account_param_from_service (
    McdAccountManagerSso *self,
    const McpAccountManager *am,
    AgAccount *ag_account,
    const gchar *mc_account)
{
  Setting *setting = setting_data (AG_ACCOUNT_KEY, SETTING_AG);
  AgSettingSource source = AG_SETTING_SOURCE_NONE;
  GValue ag_value = { 0 };

  g_return_if_fail (setting != NULL);

  g_value_init (&ag_value, G_TYPE_STRING);

  source = _ag_account_local_value (self, ag_account, AG_ACCOUNT_ALT_KEY,
      &ag_value);

  if (source != AG_SETTING_SOURCE_NONE)
    {
      gchar *value = _gvalue_to_string (&ag_value);

      DEBUG ("overriding global %s param with %s: %s",
          AG_ACCOUNT_KEY, AG_ACCOUNT_ALT_KEY, value);
      mcp_account_manager_set_value (am, mc_account, setting->mc_name, value);
      g_free (value);
    }

  g_value_unset (&ag_value);
  clear_setting_data (setting);
}

static WatchData *
make_watch_data (McdAccountManagerSso *sso,
    const gchar *mc_key)
{
  WatchData *data = g_slice_new0 (WatchData);

  data->sso = g_object_ref (sso);
  data->mc_key = g_strdup (mc_key);

  return data;
}

static void
free_watch_data (gpointer data)
{
  WatchData *wd = data;

  g_object_unref (wd->sso);
  g_free (wd->mc_key);
  g_slice_free (WatchData, wd);
}

static void unwatch_account_keys (McdAccountManagerSso *sso,
    AgAccountId id)
{
  gpointer watch_key = GUINT_TO_POINTER (id);
  GHashTable *account_watches = g_hash_table_lookup (sso->watches, watch_key);
  AgAccount *account = ag_manager_get_account (sso->ag_manager, id);
  GHashTableIter iter;
  gpointer key, value;

  if (account_watches == NULL || account == NULL)
    return;

  g_hash_table_iter_init (&iter, account_watches);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      WatchData *watch = value;

      ag_account_remove_watch (account, watch->watch);
      watch->watch = NULL;
      free_watch_data (watch);
    }

  g_hash_table_remove (sso->watches, watch_key);
}

static void _sso_updated (AgAccount * account,
    const gchar *key,
    gpointer data)
{
  WatchData *wd = data;
  McdAccountManagerSso *sso = wd->sso;
  McpAccountManager *am = sso->manager_interface;
  McpAccountStorage *mcpa = MCP_ACCOUNT_STORAGE (sso);
  gpointer id = GUINT_TO_POINTER (account->id);
  const gchar *name = g_hash_table_lookup (sso->id_name_map, id);
  AgSettingSource src = AG_SETTING_SOURCE_NONE;
  GValue ag_value = { 0 };
  gchar *mc_string = NULL;
  gchar *ag_string = NULL;
  Setting *setting = NULL;

  DEBUG ("update for account %s, key %s [%s]", name, key, wd->mc_key);

  /* an account we know nothing about. pretend this didn't happen */
  if (name == NULL)
    return;

  g_value_init (&ag_value, G_TYPE_STRING);

  setting = setting_data (key, SETTING_AG);

  if (setting == NULL)
    {
      DEBUG ("setting %s is unknown/unmapped, aborting update", key);
      return;
    }

  if (setting->global)
    src = _ag_account_global_value (account, key, &ag_value);
  else
    src = _ag_account_local_value (sso, account, key, &ag_value);

  if (src != AG_SETTING_SOURCE_NONE)
    ag_string = _gvalue_to_string (&ag_value);

  mc_string = mcp_account_manager_get_value (am, name, wd->mc_key);

  DEBUG ("cmp values: %s:%s vs %s:%s", key, ag_string, wd->mc_key, mc_string);

  if (g_strcmp0 (mc_string, ag_string) == 0)
    goto done;

  mcp_account_manager_set_value (am, name, wd->mc_key, ag_string);

  /* if we haven't completed startup, there's nothing else to do here */
  if (!sso->ready)
    goto done;

  g_signal_emit_by_name (mcpa, "altered-one", name, wd->mc_key);

 done:
  g_free (ag_string);
  g_free (mc_string);
  g_value_unset (&ag_value);
  clear_setting_data (setting);
}

static void watch_for_updates (McdAccountManagerSso *sso,
    AgAccount *account,
    Setting *setting)
{
  WatchData *data;
  gpointer id = GUINT_TO_POINTER (account->id);
  GHashTable *account_watches = g_hash_table_lookup (sso->watches, id);

  if (!setting->readable)
    return;

  if (account_watches == NULL)
    {
      account_watches =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_hash_table_insert (sso->watches, id, account_watches);
    }
  else if (g_hash_table_lookup (account_watches, setting->mc_name) != NULL)
    {
      return; /* already being watched */
    }

  DEBUG ("watching %u.%s [%s] for updates",
      account->id,
      setting->mc_name,
      setting->ag_name);

  data = make_watch_data (sso, setting->mc_name);
  data->watch = ag_account_watch_key (account, setting->ag_name, _sso_updated,
      data);
  g_hash_table_insert (account_watches, g_strdup (setting->mc_name), data);
}

static void _sso_toggled (GObject *object,
    const gchar *service_name,
    gboolean enabled,
    gpointer data)
{
  AgAccount *account = AG_ACCOUNT (object);
  AgAccountId id = account->id;
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (data);
  McpAccountStorage *mcpa = MCP_ACCOUNT_STORAGE (sso);
  gboolean on = FALSE;
  const gchar *name = NULL;
  AgService *service = NULL;
  AgManager *manager = NULL;

  /* If the account manager isn't ready, account state changes are of no   *
   * interest to us: it will pick up the then-current state of the account *
   * when it does become ready, and anything that happens between now and  *
   * then is not important:                                                */
  if (!sso->ready)
    return;

  manager = ag_account_get_manager (account);
  service = ag_manager_get_service (manager, service_name);

  /* Services of types other than IM (or whatever a subclass has told us to
   * care about instead) don't interest us.
   */
  if (service != NULL)
    {
      const gchar *service_type = ag_service_get_service_type (service);
      const gchar *our_service_type =
          account_manager_sso_get_service_type (sso);

      if (tp_strdiff (service_type, our_service_type))
        return;
    }

  on = _sso_account_enabled (sso, account, service);
  name = g_hash_table_lookup (sso->id_name_map, GUINT_TO_POINTER (id));

  if (name != NULL)
    {
      const gchar *value = on ? "true" : "false";
      McpAccountManager *am = sso->manager_interface;

      mcp_account_manager_set_value (am, name, "Enabled", value);
      g_signal_emit_by_name (mcpa, "toggled", name, on);
    }
  else
    {
      DEBUG ("received enabled=%u signal for unknown SSO account %u", on, id);
    }
}

static void _sso_deleted (GObject *object,
    AgAccountId id,
    gpointer user_data)
{
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (user_data);

  if (sso->ready)
    {
      const gchar *name =
        g_hash_table_lookup (sso->id_name_map, GUINT_TO_POINTER (id));

      /* if the account was in our cache, then this was a 3rd party delete *
       * op that someone did behind our back: fire the signal and clean up */
      if (name != NULL)
        {
          McpAccountStorage *mcpa = MCP_ACCOUNT_STORAGE (sso);
          gchar *signalled_name = g_strdup (name);

          /* forget id->name map first, so the signal can't start a loop */
          g_hash_table_remove (sso->id_name_map, GUINT_TO_POINTER (id));
          g_hash_table_remove (sso->accounts, signalled_name);

          /* stop watching for updates */
          unwatch_account_keys (sso, id);

          g_signal_emit_by_name (mcpa, "deleted", signalled_name);

          g_free (signalled_name);
        }
    }
  else
    {
      DelayedSignalData *sig_data = g_slice_new0 (DelayedSignalData);

      sig_data->signal = DELAYED_DELETE;
      sig_data->account_id = id;
      g_queue_push_tail (sso->pending_signals, sig_data);
    }
}

/* return TRUE if we actually changed any state, FALSE otherwise */
static gboolean _sso_account_enable (
    McdAccountManagerSso *self,
    AgAccount *account,
    AgService *service,
    gboolean on)
{
  AgService *original = ag_account_get_selected_service (account);

  /* the setting account is already in one of the global+service
     configurations that corresponds to our current state: don't touch it */
  if (_sso_account_enabled (self, account, service) == on)
    return FALSE;

  /* turn the local enabled flag on/off as required */
  if (service != NULL)
    ag_account_select_service (account, service);
  else
    _ag_account_select_default_im_service (self, account);

  ag_account_set_enabled (account, on);

  /* if we are turning the account on, the global flag must also be set *
   * NOTE: this isn't needed when turning the account off               */
  if (on)
    {
      ag_account_select_service (account, NULL);
      ag_account_set_enabled (account, on);
    }

  ag_account_select_service (account, original);

  return TRUE;
}

static gboolean _sso_account_enabled (
    McdAccountManagerSso *self,
    AgAccount *account,
    AgService *service)
{
  gboolean local  = FALSE;
  gboolean global = FALSE;
  AgService *original = ag_account_get_selected_service (account);

  if (service == NULL)
    {
      _ag_account_select_default_im_service (self, account);
      local = ag_account_get_enabled (account);
    }
  else
    {
      if (original != service)
        ag_account_select_service (account, service);

      local = ag_account_get_enabled (account);
    }

  ag_account_select_service (account, NULL);
  global = ag_account_get_enabled (account);

  ag_account_select_service (account, original);

  DEBUG ("_sso_account_enabled: global:%d && local:%d", global, local);

  return local && global;
}

static void _sso_created (GObject *object,
    AgAccountId id,
    gpointer user_data)
{
  AgManager *ag_manager = AG_MANAGER (object);
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (user_data);
  gchar *name =
    g_hash_table_lookup (sso->id_name_map, GUINT_TO_POINTER (id));

  if (sso->ready)
    {
      /* if we already know the account's name, we shouldn't fire the new *
       * account signal as it is one we (and our superiors) already have  */
      if (name == NULL)
        {
          McpAccountStorage *mcpa = MCP_ACCOUNT_STORAGE (sso);
          AgAccount *account = ag_manager_get_account (ag_manager, id);

          if (account != NULL)
            {
              /* this will be owned by the ag account hash, do not free it */
              name = _ag_accountid_to_mc_key (sso, id, TRUE);

              if (name != NULL)
                {
                  Setting *setting = setting_data (MC_IDENTITY_KEY, SETTING_MC);

                  g_hash_table_insert (sso->accounts, name, account);
                  g_hash_table_insert (sso->id_name_map, GUINT_TO_POINTER (id),
                      g_strdup (name));

                  save_setting (sso, account, setting, name);

                  ag_account_store (account, _ag_account_stored_cb, sso);

                  g_signal_emit_by_name (mcpa, "created", name);

                  g_signal_connect (account, "enabled",
                      G_CALLBACK (_sso_toggled), user_data);

                  clear_setting_data (setting);
                }
              else
                {
                  DEBUG ("SSO account #%u is unnameable, ignoring it", id);
                }
            }
        }
    }
  else
    {
      DelayedSignalData *sig_data = g_slice_new0 (DelayedSignalData);

      sig_data->signal = DELAYED_CREATE;
      sig_data->account_id = id;
      g_queue_push_tail (sso->pending_signals, sig_data);
    }
}

static void
mcd_account_manager_sso_init (McdAccountManagerSso *self)
{
  self->accounts =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->id_name_map =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  self->watches =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
        (GDestroyNotify) g_hash_table_unref);
  self->pending_signals = g_queue_new ();

}

static void
mcd_account_manager_sso_constructed (GObject *object)
{
  McdAccountManagerSso *self = MCD_ACCOUNT_MANAGER_SSO (object);
  GObjectClass *parent_class =
      G_OBJECT_CLASS (mcd_account_manager_sso_parent_class);
  const gchar *service_type = account_manager_sso_get_service_type (self);

  if (parent_class->constructed != NULL)
    parent_class->constructed (object);

  DEBUG ("Watching for services of type '%s'", service_type);
  self->ag_manager = ag_manager_new_for_service_type (service_type);

  g_signal_connect(self->ag_manager, "account-deleted",
      G_CALLBACK (_sso_deleted), self);
  g_signal_connect(self->ag_manager, "account-created",
      G_CALLBACK (_sso_created), self);
}

static void
mcd_account_manager_sso_class_init (McdAccountManagerSsoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = mcd_account_manager_sso_constructed;

  klass->service_type = "IM";
}

static void
_ag_account_stored_cb (
    AgAccount *account,
    const GError *err,
    gpointer user_data)
{
  McdAccountManagerSso *self = MCD_ACCOUNT_MANAGER_SSO (user_data);
  GValue uid = { 0 };
  const gchar *name = NULL;
  AgSettingSource src = AG_SETTING_SOURCE_NONE;

  g_value_init (&uid, G_TYPE_STRING);

  src = _ag_account_local_value (self, account, MC_IDENTITY_KEY, &uid);

  if (src != AG_SETTING_SOURCE_NONE && G_VALUE_HOLDS_STRING (&uid))
    {
      name = g_value_get_string (&uid);
      DEBUG ("%p:%s stored: %s", account, name, err ? err->message : "-");
      g_value_unset (&uid);
    }
  else
    {
      DEBUG ("%p:%s not stored? %s", acct,
          ag_account_get_display_name (account), err ? err->message : "-");
    }
}

static gchar *
_ag_accountid_to_mc_key (McdAccountManagerSso *sso,
    AgAccountId id,
    gboolean create)
{
  AgAccount *account = ag_manager_get_account (sso->ag_manager, id);
  AgSettingSource src = AG_SETTING_SOURCE_NONE;
  GValue value = { 0 };

  DEBUG ("AG Account ID: %u", id);

  g_value_init (&value, G_TYPE_STRING);

  /* first look for the stored TMC uid */
  src = _ag_account_local_value (sso, account, MC_IDENTITY_KEY, &value);

  /* if we found something, our work here is done: */
  if (src != AG_SETTING_SOURCE_NONE)
    {
      gchar *uid = g_value_dup_string (&value);
      g_value_unset (&value);
      return uid;
    }

  if (!create)
    return NULL;

  DEBUG ("no " MC_IDENTITY_KEY " found, synthesising one:\n");

  src = _ag_account_global_value (account, AG_ACCOUNT_KEY, &value);

  DEBUG (AG_ACCOUNT_KEY ": %s; type: %s",
      src ? "exists" : "missing",
      src ? (G_VALUE_TYPE_NAME (&value)) : "n/a" );

  if (src != AG_SETTING_SOURCE_NONE && G_VALUE_HOLDS_STRING (&value))
    {
      AgAccountSettingIter iter;
      const gchar *k;
      const GValue *v;
      GValue cmanager = { 0 };
      GValue protocol = { 0 };
      const gchar *cman, *proto;
      McpAccountManager *am = sso->manager_interface;
      AgService *service = ag_account_get_selected_service (account);
      GHashTable *params = g_hash_table_new_full (g_str_hash, g_str_equal,
          g_free, NULL);
      gchar *name = NULL;

      g_value_init (&cmanager, G_TYPE_STRING);
      g_value_init (&protocol, G_TYPE_STRING);

      /* if we weren't on a service when got here, pick the most likely one: */
      if (service == NULL)
        _ag_account_select_default_im_service (sso, account);

      ag_account_get_value (account, MC_CMANAGER_KEY, &cmanager);
      cman = g_value_get_string (&cmanager);

      if (cman == NULL)
        goto cleanup;

      ag_account_get_value (account, MC_PROTOCOL_KEY, &protocol);
      proto = g_value_get_string (&protocol);

      if (proto == NULL)
        goto cleanup;

      /* prepare the hash of MC param keys -> GValue */
      /* NOTE: some AG bare settings map to MC parameters,   *
       * so we must iterate over all AG settings, parameters *
       * and bare settings included                          */

      /* first any matching global values: */
      ag_account_select_service (account, NULL);
      ag_account_settings_iter_init (account, &iter, NULL);

      while (ag_account_settings_iter_next (&iter, &k, &v))
        {
          Setting *setting = setting_data (k, SETTING_AG);

          if (setting != NULL && g_str_has_prefix (setting->mc_name, MCPP))
            {
              gchar *param_key = g_strdup (setting->mc_name + strlen (MCPP));

              g_hash_table_insert (params, param_key, (gpointer) v);
            }

          clear_setting_data (setting);
        }

      /* then any service specific settings */
      if (service != NULL)
        ag_account_select_service (account, service);
      else
        _ag_account_select_default_im_service (sso, account);

      ag_account_settings_iter_init (account, &iter, NULL);
      while (ag_account_settings_iter_next (&iter, &k, &v))
        {
          Setting *setting = setting_data (k, SETTING_AG);

          if (setting != NULL && g_str_has_prefix (setting->mc_name, MCPP))
            {
              gchar *param_key = g_strdup (setting->mc_name + strlen (MCPP));

              g_hash_table_insert (params, param_key, (gpointer) v);
            }

          clear_setting_data (setting);
        }

      /* we want this to override any other settings for uid generation */
      g_hash_table_insert (params, g_strdup (MC_ACCOUNT_KEY), &value);

      name = mcp_account_manager_get_unique_name (am, cman, proto, params);

    cleanup:
      ag_account_select_service (account, service);
      g_hash_table_unref (params);
      g_value_unset (&value);
      g_value_unset (&cmanager);
      g_value_unset (&protocol);

      DEBUG (MC_IDENTITY_KEY " value %p:%s synthesised", name, name);
      return name;
    }

  DEBUG (MC_IDENTITY_KEY "not synthesised, returning NULL");
  return NULL;
}

static AgAccount *
get_ag_account (const McdAccountManagerSso *sso,
    const McpAccountManager *am,
    const gchar *name,
    AgAccountId *id)
{
  AgAccount *account;

  g_return_val_if_fail (id != NULL, NULL);

  /* we have a cached account, just return that */
  account = g_hash_table_lookup (sso->accounts, name);
  if (account != NULL)
    {
      *id = account->id;
      return account;
    }

  *id = 0;

  return NULL;
}

/* returns true if it actually changed an account's state */
static gboolean
save_setting (
    McdAccountManagerSso *self,
    AgAccount *account,
    const Setting *setting,
    const gchar *val)
{
  gboolean changed = FALSE;
  AgService *service = ag_account_get_selected_service (account);

  if (!setting->writable)
    return FALSE;

  if (setting->global)
    ag_account_select_service (account, NULL);
  else if (service == NULL)
    _ag_account_select_default_im_service (self, account);

  if (setting->readable)
    {
      GValue old = { 0 };
      AgSettingSource src = AG_SETTING_SOURCE_NONE;

      g_value_init (&old, G_TYPE_STRING);

      if (setting->global)
        src = _ag_account_global_value (account, setting->ag_name, &old);
      else
        src = _ag_account_local_value (self, account, setting->ag_name, &old);

      /* unsetting an already unset value, bail out */
      if (val == NULL && src == AG_SETTING_SOURCE_NONE)
        goto done;

      /* assigning a value to one which _is_ set: check it actually changed */
      if (val != NULL && src != AG_SETTING_SOURCE_NONE)
        {
          gchar *str = _gvalue_to_string (&old);
          gboolean noop = g_strcmp0 (str, val) == 0;

          g_value_unset (&old);
          g_free (str);

          if (noop)
            goto done;
        }
    }

  /* if we got this far, we're changing the stored state: */
  changed = TRUE;

  if (val != NULL)
    {
      GValue value = { 0 };

      g_value_init (&value, G_TYPE_STRING);
      g_value_set_string (&value, val);
      ag_account_set_value (account, setting->ag_name, &value);
      g_value_unset (&value);
    }
  else
    {
      ag_account_set_value (account, setting->ag_name, NULL);
    }

  /* leave the selected service as we found it: */
 done:
  ag_account_select_service (account, service);

  return changed;
}

static gboolean
_set (const McpAccountStorage *self,
    const McpAccountManager *am,
    const gchar *account_suffix,
    const gchar *key,
    const gchar *val)
{
  AgAccountId id;
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (self);
  AgAccount *account = get_ag_account (sso, am, account_suffix, &id);
  Setting *setting = NULL;
  gboolean updated = FALSE;

  /* can't store a setting with no name */
  g_return_val_if_fail (key != NULL, FALSE);

  /* we no longer create accounts in libaccount: either an account exists *
   * in libaccount as a result of some 3rd party intervention, or it is   *
   * not an account that this plugin should ever concern itself with      */

  if (account != NULL)
    setting = setting_data (key, SETTING_MC);
  else
    return FALSE;

  if (setting != NULL)
    {
      /* Enabled is both a global and a local value, for extra fun: */
      if (g_str_equal (setting->mc_name, MC_ENABLED_KEY))
        {
          gboolean on = g_str_equal (val, "true");

          DEBUG ("setting enabled flag: '%d'", on);
          updated = _sso_account_enable (sso, account, NULL, on);
        }
      else
        {
          updated = save_setting (sso, account, setting, val);
        }

      if (updated)
        sso->save = TRUE;

      clear_setting_data (setting);
    }

  /* whether or not we stored this value, if we got this far it's our *
   * setting and no-one else is allowed to claim it: so return TRUE   */
  return TRUE;
}

/* Implements the half of the _get method where key is not NULL. */
static void
account_manager_sso_get_one (
    McdAccountManagerSso *sso,
    const McpAccountManager *am,
    const gchar *account_suffix,
    const gchar *key,
    AgAccount *account,
    AgService *service)
{
  if (g_str_equal (key, MC_ENABLED_KEY))
    {
      const gchar *v = NULL;

      v = _sso_account_enabled (sso, account, service) ? "true" : "false";
      mcp_account_manager_set_value (am, account_suffix, key, v);
    }
  else if (g_str_equal (key, SERVICES_KEY))
    {
      GString *result = g_string_new ("");
      AgManager * agm = ag_account_get_manager (account);
      GList *services = ag_manager_list_services (agm);
      GList *item = NULL;

      for (item = services; item != NULL; item = g_list_next (item))
        {
          const gchar *name = ag_service_get_name (item->data);

          g_string_append_printf (result, "%s;", name);
        }

      mcp_account_manager_set_value (am, account_suffix, key, result->str);

      ag_service_list_free (services);
      g_string_free (result, TRUE);
    }
  else if (g_str_equal (key, MC_SERVICE_KEY))
    {
      const gchar *service_name = NULL;
      AgService *im_service = NULL;

      _ag_account_select_default_im_service (sso, account);
      im_service = ag_account_get_selected_service (account);
      service_name = ag_service_get_name (im_service);
      mcp_account_manager_set_value (am, account_suffix, key, service_name);
    }
  else
    {
      GValue v = { 0 };
      AgSettingSource src = AG_SETTING_SOURCE_NONE;
      Setting *setting = setting_data (key, SETTING_MC);

      if (setting == NULL)
        return;

      g_value_init (&v, G_TYPE_STRING);

      if (setting->global)
        src = _ag_account_global_value (account, setting->ag_name, &v);
      else
        src = _ag_account_local_value (sso, account, setting->ag_name, &v);

      if (src != AG_SETTING_SOURCE_NONE)
        {
          gchar *val = _gvalue_to_string (&v);

          mcp_account_manager_set_value (am, account_suffix, key, val);

          g_free (val);
        }

      if (g_str_equal (key, MCPP MC_ACCOUNT_KEY))
        _maybe_set_account_param_from_service (sso, am, account,
            account_suffix);

      g_value_unset (&v);
      clear_setting_data (setting);
    }
}

/* Implements the half of the _get method where key == NULL, which is an
 * instruction from MC that we should look up all of this account's properties
 * and stash them with mcp_account_manager_set_value().
 */
static void
account_manager_sso_get_all (
    McdAccountManagerSso *sso,
    const McpAccountManager *am,
    const gchar *account_suffix,
    AgAccount *account,
    AgService *service)
{
  AgAccountSettingIter ag_setting;
  const gchar *k;
  const GValue *v;
  const gchar *on = NULL;
  AgService *im_service = NULL;

  /* pick the IM service if we haven't got one set */
  if (service == NULL)
    _ag_account_select_default_im_service (sso, account);

  /* special case, not stored as a normal setting */
  im_service = ag_account_get_selected_service (account);
  mcp_account_manager_set_value (am, account_suffix, MC_SERVICE_KEY,
      ag_service_get_name (im_service));

  ag_account_settings_iter_init (account, &ag_setting, NULL);
  while (ag_account_settings_iter_next (&ag_setting, &k, &v))
    {
      Setting *setting = setting_data (k, SETTING_AG);

      if (setting != NULL && setting->readable && !setting->global)
        {
          gchar *value = _gvalue_to_string (v);

          mcp_account_manager_set_value (am, account_suffix,
              setting->mc_name, value);

          g_free (value);
        }

      clear_setting_data (setting);
    }

  /* deselect any service we may have to get global settings */
  ag_account_select_service (account, NULL);
  ag_account_settings_iter_init (account, &ag_setting, NULL);

  while (ag_account_settings_iter_next (&ag_setting, &k, &v))
    {
      Setting *setting = setting_data (k, SETTING_AG);

      if (setting != NULL && setting->readable && setting->global)
        {
          gchar *value  = _gvalue_to_string (v);

          mcp_account_manager_set_value (am, account_suffix,
              setting->mc_name, value);

          g_free (value);
        }

      clear_setting_data (setting);
    }

  /* special case, actually two separate but related flags in SSO */
  on = _sso_account_enabled (sso, account, NULL) ? "true" : "false";
  mcp_account_manager_set_value (am, account_suffix, MC_ENABLED_KEY, on);

  _maybe_set_account_param_from_service (sso, am, account, account_suffix);
}

gboolean
_mcd_account_manager_sso_get (
    const McpAccountStorage *self,
    const McpAccountManager *am,
    const gchar *account_suffix,
    const gchar *key)
{
  AgAccountId id;
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (self);
  AgAccount *account = get_ag_account (sso, am, account_suffix, &id);
  AgService *service = ag_account_get_selected_service (account);

  if (account == NULL)
    return FALSE;

  /* Delegate to one of the two relatively-orthogonal meanings of this
   * method... */
  if (key != NULL)
    account_manager_sso_get_one (sso, am, account_suffix, key, account,
        service);
  else
    account_manager_sso_get_all (sso, am, account_suffix, account, service);

  /* leave the selected service as we found it */
  ag_account_select_service (account, service);
  return TRUE;
}

static gboolean
_delete (const McpAccountStorage *self,
      const McpAccountManager *am,
      const gchar *account_suffix,
      const gchar *key)
{
  AgAccountId id;
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (self);
  AgAccount *account = get_ag_account (sso, am, account_suffix, &id);
  gboolean updated = FALSE;

  /* have no values for this account, nothing to do here: */
  if (account == NULL)
    return TRUE;

  if (key == NULL)
    {
      ag_account_delete (account);
      g_hash_table_remove (sso->accounts, account_suffix);
      g_hash_table_remove (sso->id_name_map, GUINT_TO_POINTER (id));

      /* stop watching for updates */
      unwatch_account_keys (sso, id);
      updated = TRUE;
    }
  else
    {
      Setting *setting = setting_data (key, SETTING_MC);

      if (setting != NULL)
        updated = save_setting (sso, account, setting, NULL);

      clear_setting_data (setting);
    }

  if (updated)
    sso->save = TRUE;

  return TRUE;
}

static gboolean
_commit (const McpAccountStorage *self,
    const McpAccountManager *am)
{
  GHashTableIter iter;
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (self);
  gchar *key;
  AgAccount *account;

  if (!sso->save)
    return TRUE;

  /* FIXME: implement commit_one(), and use account_name if it's non-NULL */

  g_hash_table_iter_init (&iter, sso->accounts);

  while (g_hash_table_iter_next (&iter, (gpointer) &key, (gpointer) &account))
    {
      Setting *setting = setting_data (MC_IDENTITY_KEY, SETTING_MC);
      /* this value ties MC accounts to SSO accounts */
      save_setting (sso, account, setting, key);
      ag_account_store (account, _ag_account_stored_cb, sso);
    }

  /* any pending changes should now have been pushed, clear the save-me flag */
  sso->save = FALSE;

  return TRUE;
}

static void
_load_from_libaccounts (McdAccountManagerSso *sso,
    const McpAccountManager *am)
{
  GList *ag_ids = ag_manager_list_by_service_type (sso->ag_manager,
      account_manager_sso_get_service_type (sso));
  GList *ag_id;

  for (ag_id = ag_ids; ag_id != NULL; ag_id = g_list_next (ag_id))
    {
      const gchar *key;
      const GValue *val;
      AgAccountSettingIter iter;
      AgAccountId id = GPOINTER_TO_UINT (ag_id->data);
      AgAccount *account = ag_manager_get_account (sso->ag_manager, id);

      if (account != NULL)
        {
          AgService *service = ag_account_get_selected_service (account);
          gchar *name = _ag_accountid_to_mc_key (sso, id, FALSE);

          if (name != NULL)
            {
              AgService *im_service = NULL;
              gchar *ident = g_strdup_printf ("%u", id);
              GStrv mc_id = g_strsplit (name, "/", 3);
              gboolean enabled;

              /* cache the account object, and the ID->name maping: the  *
               * latter is required because we might receive an async    *
               * delete signal with the ID after libaccounts-glib has    *
               * purged all its account data, so we couldn't rely on the *
               * MC_IDENTITY_KEY setting.                                */
              g_hash_table_insert (sso->accounts, name, account);
              g_hash_table_insert (sso->id_name_map, GUINT_TO_POINTER (id),
                  g_strdup (name));

              if (service == NULL)
                _ag_account_select_default_im_service (sso, account);

              /* special case, not stored as a normal setting */
              im_service = ag_account_get_selected_service (account);
              mcp_account_manager_set_value (am, name, MC_SERVICE_KEY,
                  ag_service_get_name (im_service));

              ag_account_settings_iter_init (account, &iter, NULL);

              while (ag_account_settings_iter_next (&iter, &key, &val))
                {
                  Setting *setting = setting_data (key, SETTING_AG);

                  if (setting != NULL && !setting->global && setting->readable)
                    {
                      gchar *value = _gvalue_to_string (val);

                      mcp_account_manager_set_value (am, name, setting->mc_name,
                          value);
                      watch_for_updates (sso, account, setting);

                      g_free (value);
                    }

                  clear_setting_data (setting);
                }

              ag_account_select_service (account, NULL);
              ag_account_settings_iter_init (account, &iter, NULL);

              while (ag_account_settings_iter_next (&iter, &key, &val))
                {
                  Setting *setting = setting_data (key, SETTING_AG);

                  if (setting != NULL && setting->global && setting->readable)
                    {
                      gchar *value = _gvalue_to_string (val);

                      mcp_account_manager_set_value (am, name, setting->mc_name,
                          value);
                      watch_for_updates (sso, account, setting);

                      g_free (value);
                    }

                  clear_setting_data (setting);
                }

              /* special case, actually two separate but related flags in SSO */
              enabled = _sso_account_enabled (sso, account, NULL);

              mcp_account_manager_set_value (am, name, MC_ENABLED_KEY,
                  enabled ? "true" : "false");
              mcp_account_manager_set_value (am, name, LIBACCT_ID_KEY, ident);
              mcp_account_manager_set_value (am, name, MC_CMANAGER_KEY, mc_id[0]);
              mcp_account_manager_set_value (am, name, MC_PROTOCOL_KEY, mc_id[1]);
              mcp_account_manager_set_value (am, name, MC_IDENTITY_KEY, name);
              _maybe_set_account_param_from_service (sso, am, account, name);

              /* force the services value to be synthesised + cached */
              _mcd_account_manager_sso_get (MCP_ACCOUNT_STORAGE (sso), am,
                  name, SERVICES_KEY);

              ag_account_select_service (account, service);

              g_signal_connect (account, "enabled",
                  G_CALLBACK (_sso_toggled), sso);

              g_strfreev (mc_id);
              g_free (ident);
            }
        }
      else
        {
          DelayedSignalData *data = g_slice_new0 (DelayedSignalData);

          data->account_id = id;
          g_queue_push_tail (sso->pending_signals, data);
        }
    }

  sso->loaded = TRUE;
  ag_manager_list_free (ag_ids);
}

static GList *
_list (const McpAccountStorage *self,
    const McpAccountManager *am)
{
  GList *rval = NULL;
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (self);
  GList *ag_ids = NULL;
  GList *ag_id;

  if (!sso->loaded)
    _load_from_libaccounts (sso, am);

  ag_ids = ag_manager_list_by_service_type (sso->ag_manager,
      account_manager_sso_get_service_type (sso));

  for (ag_id = ag_ids; ag_id != NULL; ag_id = g_list_next (ag_id))
    {
      AgAccountId id = GPOINTER_TO_UINT (ag_id->data);
      gchar *name = NULL;

      name = _ag_accountid_to_mc_key (sso, id, FALSE);

      if (name != NULL)
        {
          DEBUG ("\naccount %s listed", name);
          rval = g_list_prepend (rval, name);
        }
      else
        {
          DelayedSignalData *data = g_slice_new0 (DelayedSignalData);

          DEBUG ("\naccount %u delayed", id);
          data->signal = DELAYED_CREATE;
          data->account_id = id;
          g_queue_push_tail (sso->pending_signals, data);
        }
    }

  ag_manager_list_free (ag_ids);

  return rval;
}

static void
_ready (const McpAccountStorage *self,
    const McpAccountManager *am)
{
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (self);

  if (sso->ready)
    return;

  g_assert (sso->manager_interface == NULL);
  sso->manager_interface = g_object_ref (G_OBJECT (am));
  sso->ready = TRUE;

  while (g_queue_get_length (sso->pending_signals) > 0)
    {
      DelayedSignalData *data = g_queue_pop_head (sso->pending_signals);
      GObject *signal_source = G_OBJECT (sso->ag_manager);

      switch (data->signal)
        {
          case DELAYED_CREATE:
            _sso_created (signal_source, data->account_id, sso);
            break;
          case DELAYED_DELETE:
            _sso_deleted (signal_source, data->account_id, sso);
            break;
          default:
            g_assert_not_reached ();
        }

      g_slice_free (DelayedSignalData, data);
    }

  g_queue_free (sso->pending_signals);
  sso->pending_signals = NULL;
}

static gboolean
_find_account (McdAccountManagerSso *sso,
    const gchar *account_name,
    AgAccountId *account_id)
{
  GList *ag_ids = NULL;
  GList *ag_id;
  gboolean found = FALSE;

  g_return_val_if_fail (account_id != NULL, found);

  ag_ids = ag_manager_list_by_service_type (sso->ag_manager,
      account_manager_sso_get_service_type (sso));

  for (ag_id = ag_ids; ag_id != NULL; ag_id = g_list_next (ag_id))
    {
      AgAccountId id = GPOINTER_TO_UINT (ag_id->data);
      gchar *name = NULL;

      name = _ag_accountid_to_mc_key (sso, id, FALSE);

      if (g_strcmp0 (name, account_name) == 0)
        {
          found = TRUE;
          *account_id = id;
        }

      g_free (name);

      if (found)
        break;
    }

  ag_manager_list_free (ag_ids);

  return found;
}

static void
_get_identifier (const McpAccountStorage *self,
    const gchar *account,
    GValue *identifier)
{
  AgAccountId account_id = 0;

  if (!_find_account (MCD_ACCOUNT_MANAGER_SSO (self), account, &account_id))
    g_warning ("Didn't find account %s in %s", account, PLUGIN_NAME);

  g_value_init (identifier, G_TYPE_UINT);

  g_value_set_uint (identifier, account_id);
}

static GHashTable *
_get_additional_info (const McpAccountStorage *self,
    const gchar *account_suffix)
{
  AgAccountId account_id = 0;
  McdAccountManagerSso *sso = MCD_ACCOUNT_MANAGER_SSO (self);
  GHashTable *additional_info = NULL;
  AgAccount *account;
  AgService *service;
  AgAccountSettingIter iter;
  const GValue *val;
  const gchar *key;

  if (!_find_account (sso, account_suffix, &account_id))
    {
      g_warning ("Didn't find account %s in %s", account_suffix, PLUGIN_NAME);
      return NULL;
    }

  account = ag_manager_get_account (sso->ag_manager, account_id);

  g_return_val_if_fail (account != NULL, NULL);

  service = ag_account_get_selected_service (account);

  additional_info = g_hash_table_new_full (g_str_hash, g_str_equal,
      (GDestroyNotify) g_free, (GDestroyNotify) tp_g_value_slice_free);

  if (service == NULL)
    _ag_account_select_default_im_service (sso, account);

  ag_account_settings_iter_init (account, &iter, NULL);

  while (ag_account_settings_iter_next (&iter, &key, &val))
    {
      if (tp_strv_contains (exported_settings, key))
          g_hash_table_insert (additional_info, g_strdup (key),
              tp_g_value_slice_dup (val));
    }

  ag_account_select_service (account, NULL);
  ag_account_settings_iter_init (account, &iter, NULL);

  while (ag_account_settings_iter_next (&iter, &key, &val))
    {
      if (tp_strv_contains (exported_settings, key))
          g_hash_table_insert (additional_info, g_strdup (key),
              tp_g_value_slice_dup (val));
    }

  ag_account_select_service (account, service);

  g_object_unref (account);

  return additional_info;
}

static void
account_storage_iface_init (McpAccountStorageIface *iface,
    gpointer unused G_GNUC_UNUSED)
{
  mcp_account_storage_iface_set_name (iface, PLUGIN_NAME);
  mcp_account_storage_iface_set_desc (iface, PLUGIN_DESCRIPTION);
  mcp_account_storage_iface_set_priority (iface, PLUGIN_PRIORITY);
  mcp_account_storage_iface_set_provider (iface, PLUGIN_PROVIDER);

  mcp_account_storage_iface_implement_get (iface,
      _mcd_account_manager_sso_get);
  mcp_account_storage_iface_implement_set (iface, _set);
  mcp_account_storage_iface_implement_delete (iface, _delete);
  mcp_account_storage_iface_implement_commit (iface, _commit);
  mcp_account_storage_iface_implement_list (iface, _list);
  mcp_account_storage_iface_implement_ready (iface, _ready);
  mcp_account_storage_iface_implement_get_identifier (iface, _get_identifier);
  mcp_account_storage_iface_implement_get_additional_info (iface,
      _get_additional_info);
}

McdAccountManagerSso *
mcd_account_manager_sso_new (void)
{
  return g_object_new (MCD_TYPE_ACCOUNT_MANAGER_SSO, NULL);
}
