/* vi: set et sw=4 ts=8 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 8 -*- */
/*
 * This file is part of mission-control
 *
 * Copyright (C) 2007 Nokia Corporation. 
 *
 * Contact: Naba Kumar  <naba.kumar@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/**
 * SECTION:mcd-dispatcher
 * @title: McdDispatcher
 * @short_description: Dispatcher class to dispatch channels to handlers
 * @see_also: 
 * @stability: Unstable
 * @include: mcd-dispatcher.h
 * 
 * FIXME
 */

#include <dlfcn.h>
#include <errno.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gi18n.h>

#include <dbus/dbus-glib-lowlevel.h>

#include "mcd-signals-marshal.h"
#include "mcd-account-priv.h"
#include "mcd-account-requests.h"
#include "mcd-connection.h"
#include "mcd-channel.h"
#include "mcd-master.h"
#include "mcd-channel-priv.h"
#include "mcd-dispatcher-context.h"
#include "mcd-dispatcher-priv.h"
#include "mcd-dispatch-operation.h"
#include "mcd-dispatch-operation-priv.h"
#include "mcd-misc.h"
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/proxy-subclass.h>
#include <telepathy-glib/svc-generic.h>
#include <telepathy-glib/util.h>
#include "_gen/interfaces.h"
#include "_gen/gtypes.h"
#include "_gen/cli-client.h"
#include "_gen/cli-client-body.h"
#include "_gen/svc-dispatcher.h"

#include <libmcclient/mc-errors.h>

#include <string.h>
#include "sp_timestamp.h"

#define MCD_DISPATCHER_PRIV(dispatcher) (MCD_DISPATCHER (dispatcher)->priv)

static void dispatcher_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE (McdDispatcher, mcd_dispatcher, MCD_TYPE_MISSION,
    G_IMPLEMENT_INTERFACE (MC_TYPE_SVC_CHANNEL_DISPATCHER,
                           dispatcher_iface_init);
    G_IMPLEMENT_INTERFACE (
        MC_TYPE_SVC_CHANNEL_DISPATCHER_INTERFACE_OPERATION_LIST,
        NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
                           tp_dbus_properties_mixin_iface_init))

struct _McdDispatcherContext
{
    gint ref_count;

    guint finished : 1;

    /* If this flag is TRUE, dispatching must be cancelled ASAP */
    guint cancelled : 1;

    /* This is set to TRUE if the incoming channel being dispatched has being
     * requested before the approvers could be run; in that case, the approval
     * phase should be skipped */
    guint skip_approval : 1;

    McdDispatcher *dispatcher;

    GList *channels;
    McdChannel *main_channel;
    McdAccount *account;
    McdDispatchOperation *operation;
    /* bus names (including the common prefix) in preference order */
    GStrv possible_handlers;

    /* This variable is the count of locks that must be removed before handlers
     * can be invoked. Each call to an observer increments this count (and
     * decrements it on return), and for unrequested channels we have an
     * approver lock, too.
     * When the variable gets back to 0, handlers are run. */
    gint client_locks;

    /* Number of approvers that we invoked */
    gint approvers_invoked;

    gchar *protocol;

    /* State-machine internal data fields: */
    GList *chain;

    /* Next function in chain */
    guint next_func_index;
};

typedef struct
{
    McdDispatcher *dispatcher;
    McdChannel *channel;
    gint handler_locks;
    gboolean handled;
} McdChannelRecover;

typedef enum
{
    MCD_CLIENT_APPROVER = 0x1,
    MCD_CLIENT_HANDLER  = 0x2,
    MCD_CLIENT_OBSERVER = 0x4,
    MCD_CLIENT_INTERFACE_REQUESTS  = 0x8,
} McdClientInterface;

typedef struct _McdClient
{
    TpProxy *proxy;
    gchar *name;
    McdClientInterface interfaces;
    gchar **handled_channels;
    guint bypass_approver : 1;

    /* If a client was in the ListActivatableNames list, it must not be
     * removed when it disappear from the bus.
     */
    guint activatable : 1;
    guint active : 1;
    guint got_handled_channels : 1;
    guint getting_handled_channels : 1;

    /* Channel filters
     * A channel filter is a GHashTable of
     * - key: gchar *property_name
     * - value: GValue of one of the allowed types on the ObserverChannelFilter
     *          spec. The following matching is observed:
     *           * G_TYPE_STRING: 's'
     *           * G_TYPE_BOOLEAN: 'b'
     *           * DBUS_TYPE_G_OBJECT_PATH: 'o'
     *           * G_TYPE_UINT64: 'y' (8b), 'q' (16b), 'u' (32b), 't' (64b)
     *           * G_TYPE_INT64:            'n' (16b), 'i' (32b), 'x' (64b)
     *
     * The list can be NULL if there is no filter, or the filters are not yet
     * retrieven from the D-Bus *ChannelFitler properties. In the last case,
     * the dispatcher just don't dispatch to this client.
     */
    GList *approver_filters;
    GList *handler_filters;
    GList *observer_filters;
} McdClient;

/* Analogous to TP_CM_*_BASE */
#define MC_CLIENT_BUS_NAME_BASE MC_IFACE_CLIENT "."
#define MC_CLIENT_OBJECT_PATH_BASE "/org/freedesktop/Telepathy/Client/"

struct _McdDispatcherPrivate
{
    /* Dispatching contexts */
    GList *contexts;

    TpDBusDaemon *dbus_daemon;

    /* Array of channel handler's capabilities, stored as a GPtrArray for
     * performance reasons */
    GPtrArray *channel_handler_caps;

    /* list of McdFilter elements */
    GList *filters;

    /* hash table containing clients
     * char *bus_name -> McdClient */
    GHashTable *clients;

    McdMaster *master;

    /* Initially FALSE, meaning we suppress OperationList.DispatchOperations
     * change notification signals because nobody has retrieved that property
     * yet. Set to TRUE the first time someone reads the DispatchOperations
     * property. */
    gboolean operation_list_active;

    gboolean is_disposed;
    
};

struct cancel_call_data
{
    DBusGProxy *handler_proxy;
    DBusGProxyCall *call;
    McdDispatcher *dispatcher;
};

typedef struct
{
    McdDispatcherContext *context;
    GList *channels;
} McdHandlerCallData;

typedef struct
{
    TpProxy *handler;
    gchar *request_path;
} McdRemoveRequestData;

enum
{
    PROP_0,
    PROP_DBUS_DAEMON,
    PROP_MCD_MASTER,
    PROP_INTERFACES,
    PROP_DISPATCH_OPERATIONS,
};

enum _McdDispatcherSignalType
{
    CHANNEL_ADDED,
    CHANNEL_REMOVED,
    DISPATCHED,
    DISPATCH_FAILED,
    DISPATCH_COMPLETED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };
static GQuark client_ready_quark = 0;

static void mcd_dispatcher_context_unref (McdDispatcherContext * ctx);
static void on_operation_finished (McdDispatchOperation *operation,
                                   McdDispatcherContext *context);


static inline void
mcd_dispatcher_context_ref (McdDispatcherContext *context)
{
    g_return_if_fail (context != NULL);
    DEBUG ("called on %p (ref = %d)", context, context->ref_count);
    context->ref_count++;
}

static void
mcd_handler_call_data_free (McdHandlerCallData *call_data)
{
    g_list_free (call_data->channels);
    g_slice_free (McdHandlerCallData, call_data);
}

/*
 * mcd_dispatcher_context_handler_done:
 * @context: the #McdDispatcherContext.
 * 
 * Called to informs the @context that handling of a channel is completed,
 * either because a channel handler has returned from the HandleChannel(s)
 * call, or because there was an error in calling the handler. 
 * This function checks the status of all the channels in @context, and when
 * there is nothing left to do (either because all channels are dispatched, or
 * because it's impossible to dispatch them) it emits the "dispatch-completed"
 * signal and destroys the @context.
 */
static void
mcd_dispatcher_context_handler_done (McdDispatcherContext *context)
{
    GList *list;
    gint channels_left = 0;

    if (context->finished)
    {
        DEBUG ("context %p is already finished", context);
        return;
    }

    for (list = context->channels; list != NULL; list = list->next)
    {
        McdChannel *channel = MCD_CHANNEL (list->data);
        McdChannelStatus status;

        status = mcd_channel_get_status (channel);
        if (status == MCD_CHANNEL_STATUS_DISPATCHING ||
            status == MCD_CHANNEL_STATUS_HANDLER_INVOKED)
            channels_left++;
        /* TODO: recognize those channels whose dispatch failed, and
         * re-dispatch them to another handler */
    }

    DEBUG ("%d channels still dispatching", channels_left);
    if (channels_left == 0)
    {
        context->finished = TRUE;
        g_signal_emit (context->dispatcher,
                       signals[DISPATCH_COMPLETED], 0, context);
        mcd_dispatcher_context_unref (context);
    }
}

static void
mcd_client_free (McdClient *client)
{
    if (client->proxy)
        g_object_unref (client->proxy);

    g_free (client->name);

    g_list_foreach (client->approver_filters,
                    (GFunc)g_hash_table_destroy, NULL);
    g_list_foreach (client->handler_filters,
                    (GFunc)g_hash_table_destroy, NULL);
    g_list_foreach (client->observer_filters,
                    (GFunc)g_hash_table_destroy, NULL);
}

static GList *
chain_add_filter (GList *chain,
		  McdFilterFunc filter,
		  guint priority,
                  gpointer user_data)
{
    GList *elem;
    McdFilter *filter_data;

    filter_data = g_slice_new (McdFilter);
    filter_data->func = filter;
    filter_data->priority = priority;
    filter_data->user_data = user_data;
    for (elem = chain; elem; elem = elem->next)
	if (((McdFilter *)elem->data)->priority >= priority) break;

    return g_list_insert_before (chain, elem, filter_data);
}

/* Returns # of times particular channel type  has been used */
gint
mcd_dispatcher_get_channel_type_usage (McdDispatcher * dispatcher,
				       GQuark chan_type_quark)
{
    const GList *managers, *connections, *channels;
    McdDispatcherPrivate *priv = dispatcher->priv;
    gint usage_counter = 0;

    managers = mcd_operation_get_missions (MCD_OPERATION (priv->master));
    while (managers)
    {
        connections =
            mcd_operation_get_missions (MCD_OPERATION (managers->data));
        while (connections)
        {
            channels =
                mcd_operation_get_missions (MCD_OPERATION (connections->data));
            while (channels)
            {
                McdChannel *channel = MCD_CHANNEL (channels->data);
                McdChannelStatus status;

                status = mcd_channel_get_status (channel);
                if ((status == MCD_CHANNEL_STATUS_DISPATCHING ||
                     status == MCD_CHANNEL_STATUS_HANDLER_INVOKED ||
                     status == MCD_CHANNEL_STATUS_DISPATCHED) &&
                    mcd_channel_get_channel_type_quark (channel) ==
                    chan_type_quark)
                    usage_counter++;
                channels = channels->next;
            }
            connections = connections->next;
        }
	managers = managers->next;
    }

    return usage_counter;
}

static void
on_master_abort (McdMaster *master, McdDispatcherPrivate *priv)
{
    g_object_unref (master);
    priv->master = NULL;
}

/* returns TRUE if the channel matches one property criteria
 */
static gboolean
match_property (GHashTable *channel_properties,
                gchar *property_name,
                GValue *filter_value)
{
    GType filter_type = G_VALUE_TYPE (filter_value);

    g_assert (G_IS_VALUE (filter_value));

    if (filter_type == G_TYPE_STRING)
    {
        const gchar *string;

        string = tp_asv_get_string (channel_properties, property_name);
        if (!string)
            return FALSE;

        return !tp_strdiff (string, g_value_get_string (filter_value));
    }

    if (filter_type == DBUS_TYPE_G_OBJECT_PATH)
    {
        const gchar *path;

        path = tp_asv_get_object_path (channel_properties, property_name);
        if (!path)
            return FALSE;

        return !tp_strdiff (path, g_value_get_boxed (filter_value));
    }

    if (filter_type == G_TYPE_BOOLEAN)
    {
        gboolean valid;
        gboolean b;

        b = tp_asv_get_boolean (channel_properties, property_name, &valid);
        if (!valid)
            return FALSE;

        return !!b == !!g_value_get_boolean (filter_value);
    }

    if (filter_type == G_TYPE_UCHAR || filter_type == G_TYPE_UINT ||
        filter_type == G_TYPE_UINT64)
    {
        gboolean valid;
        guint64 i;

        i = tp_asv_get_uint64 (channel_properties, property_name, &valid);
        if (!valid)
            return FALSE;

        if (filter_type == G_TYPE_UCHAR)
            return i == g_value_get_uchar (filter_value);
        else if (filter_type == G_TYPE_UINT)
            return i == g_value_get_uint (filter_value);
        else
            return i == g_value_get_uint64 (filter_value);
    }

    if (filter_type == G_TYPE_INT || filter_type == G_TYPE_INT64)
    {
        gboolean valid;
        gint64 i;

        i = tp_asv_get_int64 (channel_properties, property_name, &valid);
        if (!valid)
            return FALSE;

        if (filter_type == G_TYPE_INT)
            return i == g_value_get_int (filter_value);
        else
            return i == g_value_get_int64 (filter_value);
    }

    g_warning ("%s: Invalid type: %s",
               G_STRFUNC, g_type_name (filter_type));
    return FALSE;
}

/* return TRUE if the two channel classes are equals
 */
static gboolean
channel_classes_equals (GHashTable *channel_class1, GHashTable *channel_class2)
{
    GHashTableIter iter;
    gchar *property_name;
    GValue *property_value;

    if (g_hash_table_size (channel_class1) !=
        g_hash_table_size (channel_class2))
        return FALSE;

    g_hash_table_iter_init (&iter, channel_class1);
    while (g_hash_table_iter_next (&iter, (gpointer *) &property_name,
                                   (gpointer *) &property_value))
    {
        if (!match_property (channel_class2, property_name, property_value))
            return FALSE;
    }
    return TRUE;
}

/* if the channel matches one of the channel filters, returns a positive
 * number that increases with more specific matches; otherwise, returns 0
 *
 * (implementation detail: the positive number is 1 + the number of keys in the
 * largest filter that matched)
 */
static guint
match_filters (McdChannel *channel, GList *filters)
{
    GHashTable *channel_properties;
    McdChannelStatus status;
    GList *list;
    guint best_quality = 0;

    status = mcd_channel_get_status (channel);
    channel_properties =
        (status == MCD_CHANNEL_STATUS_REQUEST ||
         status == MCD_CHANNEL_STATUS_REQUESTED) ?
        _mcd_channel_get_requested_properties (channel) :
        _mcd_channel_get_immutable_properties (channel);

    for (list = filters; list != NULL; list = list->next)
    {
        GHashTable *filter = list->data;
        GHashTableIter filter_iter;
        gboolean filter_matched = TRUE;
        gchar *property_name;
        GValue *filter_value;
        guint quality;

        /* +1 because the empty hash table matches everything :-) */
        quality = g_hash_table_size (filter) + 1;

        if (quality <= best_quality)
        {
            /* even if this filter matches, there's no way it can be a
             * better-quality match than the best one we saw so far */
            continue;
        }

        g_hash_table_iter_init (&filter_iter, filter);
        while (g_hash_table_iter_next (&filter_iter,
                                       (gpointer *) &property_name,
                                       (gpointer *) &filter_value))
        {
            if (! match_property (channel_properties, property_name,
                                filter_value))
            {
                filter_matched = FALSE;
                break;
            }
        }

        if (filter_matched)
        {
            best_quality = quality;
        }
    }

    return best_quality;
}

static McdClient *
get_default_handler (McdDispatcher *dispatcher, McdChannel *channel)
{
    GHashTableIter iter;
    McdClient *client;

    g_hash_table_iter_init (&iter, dispatcher->priv->clients);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &client))
    {
        if (!client->proxy ||
            !(client->interfaces & MCD_CLIENT_HANDLER))
            continue;

        if (match_filters (channel, client->handler_filters) > 0)
            return client;
    }
    return NULL;
}

static void
handle_channels_cb (TpProxy *proxy, const GError *error, gpointer user_data,
                    GObject *weak_object)
{
    McdHandlerCallData *call_data = user_data;
    McdDispatcherContext *context = call_data->context;
    GList *list;

    mcd_dispatcher_context_ref (context); /* unref is done before return */
    if (error)
    {
        GError *mc_error = NULL;

        g_warning ("%s got error: %s", G_STRFUNC, error->message);

        /* We can't reliably map channel handler error codes to MC error
         * codes. So just using generic error message.
         */
        mc_error = g_error_new (MC_ERROR, MC_CHANNEL_REQUEST_GENERIC_ERROR,
                                "Handle channel failed: %s", error->message);

        for (list = call_data->channels; list != NULL; list = list->next)
        {
            McdChannel *channel = MCD_CHANNEL (list->data);

            mcd_channel_take_error (channel, g_error_copy (mc_error));
            g_signal_emit_by_name (context->dispatcher, "dispatch-failed",
                                   channel, mc_error);

            /* FIXME: try to dispatch the channels to another handler, instead
             * of just aborting them */
            mcd_mission_abort (MCD_MISSION (channel));
        }
        g_error_free (mc_error);
    }
    else
    {
        for (list = call_data->channels; list != NULL; list = list->next)
        {
            McdChannel *channel = MCD_CHANNEL (list->data);

            /* TODO: abort the channel if the handler dies */
            _mcd_channel_set_status (channel, MCD_CHANNEL_STATUS_DISPATCHED);
            g_signal_emit_by_name (context->dispatcher, "dispatched", channel);
        }
    }

    mcd_dispatcher_context_handler_done (context);
    mcd_dispatcher_context_unref (context);
}

typedef struct
{
    McdClient *client;
    gsize quality;
} PossibleHandler;

static gint
possible_handler_cmp (gconstpointer a_,
                      gconstpointer b_)
{
    const PossibleHandler *a = a_;
    const PossibleHandler *b = b_;

    if (a->client->bypass_approver)
    {
        if (!b->client->bypass_approver)
        {
            /* BypassApproval wins, so a is better than b */
            return 1;
        }
    }
    else if (b->client->bypass_approver)
    {
        /* BypassApproval wins, so b is better than a */
        return -1;
    }

    if (a->quality < b->quality)
    {
        return -1;
    }

    if (b->quality < a->quality)
    {
        return 1;
    }

    return 0;
}

static GStrv
mcd_dispatcher_get_possible_handlers (McdDispatcher *self,
                                      const GList *channels)
{
    GList *handlers = NULL;
    const GList *iter;
    GHashTableIter client_iter;
    gpointer client_p;
    guint n_handlers = 0;
    guint i;
    GStrv ret;

    g_hash_table_iter_init (&client_iter, self->priv->clients);

    while (g_hash_table_iter_next (&client_iter, NULL, &client_p))
    {
        McdClient *client = client_p;
        gsize total_quality = 0;

        if (client->proxy == NULL ||
            !(client->interfaces & MCD_CLIENT_HANDLER))
        {
            /* not a handler at all */
            continue;
        }

        for (iter = channels; iter != NULL; iter = iter->next)
        {
            McdChannel *channel = MCD_CHANNEL (iter->data);
            guint quality;

            quality = match_filters (channel, client->handler_filters);

            if (quality == 0)
            {
                total_quality = 0;
                break;
            }
            else
            {
                total_quality += quality;
            }
        }

        if (total_quality > 0)
        {
            PossibleHandler *ph = g_slice_new0 (PossibleHandler);

            ph->client = client;
            ph->quality = total_quality;

            handlers = g_list_prepend (handlers, ph);
            n_handlers++;
        }
    }

    /* if no handlers can take them all, fail */
    if (handlers == NULL)
    {
        return NULL;
    }

    /* We have at least one handler that can take the whole batch. Sort
     * the possible handlers, most preferred first (i.e. sort by ascending
     * quality then reverse) */
    handlers = g_list_sort (handlers, possible_handler_cmp);
    handlers = g_list_reverse (handlers);

    ret = g_new0 (gchar *, n_handlers + 1);

    for (iter = handlers, i = 0; iter != NULL; iter = iter->next, i++)
    {
        PossibleHandler *ph = iter->data;

        ret[i] = g_strconcat (MC_CLIENT_BUS_NAME_BASE,
                              ph->client->name, NULL);
        g_slice_free (PossibleHandler, ph);
    }

    ret[n_handlers] = NULL;

    g_list_free (handlers);

    return ret;
}

/*
 * mcd_dispatcher_handle_channels:
 * @context: the #McdDispatcherContext
 * @channels: a #GList of borrowed refs to #McdChannel objects, ownership of
 *  which is stolen by this function
 * @handler: the selected handler
 *
 * Invoke the handler for the given channels.
 */
static void
mcd_dispatcher_handle_channels (McdDispatcherContext *context,
                                GList *channels,
                                McdClient *handler)
{
    guint64 user_action_time;
    McdConnection *connection;
    const gchar *account_path, *connection_path;
    GPtrArray *channels_array, *satisfied_requests;
    McdHandlerCallData *handler_data;
    GHashTable *handler_info;
    const GList *cl;

    connection = mcd_dispatcher_context_get_connection (context);
    connection_path = connection ?
        mcd_connection_get_object_path (connection) : NULL;
    if (G_UNLIKELY (!connection_path)) connection_path = "/";

    g_assert (context->account != NULL);
    account_path = mcd_account_get_object_path (context->account);
    if (G_UNLIKELY (!account_path)) account_path = "/";

    channels_array = _mcd_channel_details_build_from_list (channels);

    user_action_time = 0; /* TODO: if we have a CDO, get it from there */
    satisfied_requests = g_ptr_array_new ();
    for (cl = channels; cl != NULL; cl = cl->next)
    {
        McdChannel *channel = MCD_CHANNEL (cl->data);
        const GList *requests;
        guint64 user_time;

        requests = _mcd_channel_get_satisfied_requests (channel);
        while (requests)
        {
            g_ptr_array_add (satisfied_requests, requests->data);
            requests = requests->next;
        }

        /* FIXME: what if we have more than one request? */
        user_time = _mcd_channel_get_request_user_action_time (channel);
        if (user_time)
            user_action_time = user_time;

        _mcd_channel_set_status (channel,
                                 MCD_CHANNEL_STATUS_HANDLER_INVOKED);
    }

    handler_info = g_hash_table_new (g_str_hash, g_str_equal);

    /* The callback needs to get the dispatcher context, and the channels
     * the handler was asked to handle. The context will keep track of how
     * many channels are still to be dispatched,
     * still pending. When all of them return, the dispatching is
     * considered to be completed. */
    handler_data = g_slice_new (McdHandlerCallData);
    handler_data->context = context;
    handler_data->channels = channels;
    DEBUG ("Invoking handler %s (context %p)", handler->name, context);
    mc_cli_client_handler_call_handle_channels (handler->proxy, -1,
        account_path, connection_path,
        channels_array, satisfied_requests, user_action_time,
        handler_info, handle_channels_cb,
        handler_data, (GDestroyNotify)mcd_handler_call_data_free,
        (GObject *)context->dispatcher);

    g_ptr_array_free (satisfied_requests, TRUE);
    _mcd_channel_details_free (channels_array);
    g_hash_table_unref (handler_info);
}

/*
 * mcd_dispatcher_run_handler:
 * @context: the #McdDispatcherContext.
 * @channels: a #GList of #McdChannel elements to be handled.
 *
 * This functions tries to find a handler to handle @channels, and invokes its
 * HandleChannels method. It returns a list of channels that are still
 * unhandled.
 *
 * FIXME: this should use mcd_dispatcher_get_possible_handlers or similar
 */
static GList *
mcd_dispatcher_run_handler (McdDispatcherContext *context,
                            const GList *channels)
{
    McdDispatcherPrivate *priv = context->dispatcher->priv;
    McdClient *handler = NULL;
    gint num_channels_best, num_channels;
    const GList *cl;
    GList *handled_best = NULL, *unhandled;
    const gchar *approved_handler;
    GHashTableIter iter;
    McdClient *client;

    /* The highest priority goes to the handler chosen by the approver */
    if (context->operation)
        approved_handler =
            mcd_dispatch_operation_get_handler (context->operation);
    else
        approved_handler = NULL;

    /* TODO: in the McdDispatcherContext there should be a hint on what handler
     * to invoke */
    num_channels_best = 0;
    g_hash_table_iter_init (&iter, priv->clients);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &client))
    {
        GList *handled = NULL;
        gboolean the_chosen_one;

        if (!client->proxy ||
            !(client->interfaces & MCD_CLIENT_HANDLER))
            continue;

        /* count the number of channels supported by this handler; we try to
         * send the channels to the handler that can handle the most */
        num_channels = 0;
        for (cl = channels; cl != NULL; cl = cl->next)
        {
            McdChannel *channel = MCD_CHANNEL (cl->data);

            if (match_filters (channel, client->handler_filters))
            {
                num_channels++;
                handled = g_list_prepend (handled, channel);
            }
        }

        the_chosen_one =
            approved_handler != NULL && strcmp (approved_handler,
                                                client->name) == 0;
        if (num_channels > num_channels_best || the_chosen_one)
        {
            /* this is the best candidate handler so far; remember also the
             * list of channels it cannot handle */
            handler = client;
            g_list_free (handled_best);
            handled_best = handled;
            num_channels_best = num_channels;

            /* we don't even look for other handlers, if this is the one chosen
             * by the approver */
            if (the_chosen_one) break;
        }
        else
            g_list_free (handled);
    }

    /* build the list of unhandled channels */
    unhandled = NULL;
    for (cl = channels; cl != NULL; cl = cl->next)
    {
        if (!g_list_find (handled_best, cl->data))
            unhandled = g_list_prepend (unhandled, cl->data);
    }

    if (handler)
    {
        mcd_dispatcher_handle_channels (context, handled_best, handler);
    }
    else
    {
        DEBUG ("Client.Handler not found");
        g_list_free (unhandled);
        unhandled = NULL;
    }
    return unhandled;
}

static void
mcd_dispatcher_run_handlers (McdDispatcherContext *context)
{
    GList *channels;
    GList *unhandled = NULL;

    sp_timestamp ("run handlers");
    mcd_dispatcher_context_ref (context);

    /* call mcd_dispatcher_run_handler until there are no unhandled channels */

    /* g_list_copy() should have a 'const' parameter. */
    channels = g_list_copy ((GList *)
                            mcd_dispatcher_context_get_channels (context));
    while (channels)
    {
        unhandled = mcd_dispatcher_run_handler (context, channels);
        if (g_list_length (unhandled) >= g_list_length (channels))
        {
            /* this could really be an assertion, but just to be on the safe
             * side... */
            g_warning ("Number of unhandled channels not decreasing!");
            break;
        }
        g_list_free (channels);
        channels = unhandled;
    }
    mcd_dispatcher_context_unref (context);
}

static void
mcd_dispatcher_context_release_client_lock (McdDispatcherContext *context)
{
    g_return_if_fail (context->client_locks > 0);
    DEBUG ("called on %p, locks = %d", context, context->client_locks);
    context->client_locks--;
    if (context->client_locks == 0)
    {
        /* no observers left, let's go on with the dispatching */
        mcd_dispatcher_run_handlers (context);
    }
}

static void
observe_channels_cb (TpProxy *proxy, const GError *error,
                     gpointer user_data, GObject *weak_object)
{
    McdDispatcherContext *context = user_data;

    /* we display the error just for debugging, but we don't really care */
    if (error)
        DEBUG ("Observer returned error: %s", error->message);

    mcd_dispatcher_context_release_client_lock (context);
}

/* The returned GPtrArray is allocated, but the contents are borrowed. */
static GPtrArray *
collect_satisfied_requests (GList *channels)
{
    const GList *c, *r;
    GHashTable *set = g_hash_table_new (g_str_hash, g_str_equal);
    GHashTableIter iter;
    gpointer path;
    GPtrArray *ret;

    /* collect object paths into a hash table, to drop duplicates */
    for (c = channels; c != NULL; c = c->next)
    {
        const GList *reqs = _mcd_channel_get_satisfied_requests (c->data);

        for (r = reqs; r != NULL; r = r->next)
        {
            g_hash_table_insert (set, r->data, r->data);
        }
    }

    /* serialize them into a pointer array, which is what dbus-glib wants */
    ret = g_ptr_array_sized_new (g_hash_table_size (set));

    g_hash_table_iter_init (&iter, set);

    while (g_hash_table_iter_next (&iter, &path, NULL))
    {
        g_ptr_array_add (ret, path);
    }

    return ret;
}

static void
mcd_dispatcher_run_observers (McdDispatcherContext *context)
{
    McdDispatcherPrivate *priv = context->dispatcher->priv;
    const GList *cl, *channels;
    const gchar *dispatch_operation_path = "/";
    GHashTable *observer_info;
    GHashTableIter iter;
    McdClient *client;

    sp_timestamp ("run observers");
    channels = context->channels;
    observer_info = g_hash_table_new (g_str_hash, g_str_equal);

    g_hash_table_iter_init (&iter, priv->clients);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &client))
    {
        GList *observed = NULL;
        McdConnection *connection;
        McdAccount *account;
        const gchar *account_path, *connection_path;
        GPtrArray *channels_array, *satisfied_requests;

        if (!client->proxy ||
            !(client->interfaces & MCD_CLIENT_OBSERVER))
            continue;

        for (cl = channels; cl != NULL; cl = cl->next)
        {
            McdChannel *channel = MCD_CHANNEL (cl->data);

            if (match_filters (channel, client->observer_filters))
                observed = g_list_prepend (observed, channel);
        }
        if (!observed) continue;

        /* build up the parameters and invoke the observer */
        connection = mcd_dispatcher_context_get_connection (context);
        g_assert (connection != NULL);
        connection_path = mcd_connection_get_object_path (connection);

        account = mcd_connection_get_account (connection);
        g_assert (account != NULL);
        account_path = mcd_account_get_object_path (account);

        /* TODO: there's room for optimization here: reuse the channels_array,
         * if the observed list is the same */
        channels_array = _mcd_channel_details_build_from_list (observed);

        satisfied_requests = collect_satisfied_requests (observed);

        if (context->operation)
        {
            dispatch_operation_path =
                mcd_dispatch_operation_get_path (context->operation);
        }

        context->client_locks++;
        mcd_dispatcher_context_ref (context);
        mc_cli_client_observer_call_observe_channels (client->proxy, -1,
            account_path, connection_path, channels_array,
            dispatch_operation_path, satisfied_requests, observer_info,
            observe_channels_cb,
            context, (GDestroyNotify)mcd_dispatcher_context_unref,
            (GObject *)context->dispatcher);

        /* don't free the individual object paths, which are borrowed from the
         * McdChannel objects */
        g_ptr_array_free (satisfied_requests, TRUE);

        _mcd_channel_details_free (channels_array);

        g_list_free (observed);
    }

    g_hash_table_destroy (observer_info);
}

/*
 * mcd_dispatcher_context_approver_not_invoked:
 * @context: the #McdDispatcherContext.
 *
 * This function is called when an approver returned error on
 * AddDispatchOperation(), and is used to keep track of how many approvers we
 * have contacted. If all of them fail, then we continue the dispatching.
 */
static void
mcd_dispatcher_context_approver_not_invoked (McdDispatcherContext *context)
{
    g_return_if_fail (context->approvers_invoked > 0);
    context->approvers_invoked--;

    if (context->approvers_invoked == 0)
        mcd_dispatcher_context_release_client_lock (context);
}

static void
add_dispatch_operation_cb (TpProxy *proxy, const GError *error,
                           gpointer user_data, GObject *weak_object)
{
    McdDispatcherContext *context = user_data;

    if (error)
    {
        DEBUG ("Failed to add DO on approver: %s", error->message);

        /* if all approvers fail to add the DO, then we behave as if no
         * approver was registered: i.e., we continue dispatching */
        context->approvers_invoked--;
        if (context->approvers_invoked == 0)
            mcd_dispatcher_context_release_client_lock (context);
    }

    if (context->operation)
    {
        _mcd_dispatch_operation_unblock_finished (context->operation);
    }
}

static void
mcd_dispatcher_run_approvers (McdDispatcherContext *context)
{
    McdDispatcherPrivate *priv = context->dispatcher->priv;
    const GList *cl, *channels;
    GHashTableIter iter;
    McdClient *client;

    g_return_if_fail (context->operation != NULL);
    sp_timestamp ("run approvers");

    /* we temporarily increment this count and decrement it at the end of the
     * function, to make sure it won't become 0 while we are still invoking
     * approvers */
    context->approvers_invoked = 1;

    context->client_locks++;
    channels = context->channels;
    g_hash_table_iter_init (&iter, priv->clients);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &client))
    {
        GPtrArray *channel_details;
        const gchar *dispatch_operation;
        GHashTable *properties;
        gboolean matched = FALSE;

        if (!client->proxy ||
            !(client->interfaces & MCD_CLIENT_APPROVER))
            continue;

        for (cl = channels; cl != NULL; cl = cl->next)
        {
            McdChannel *channel = MCD_CHANNEL (cl->data);

            if (match_filters (channel, client->approver_filters))
            {
                matched = TRUE;
                break;
            }
        }
        if (!matched) continue;

        dispatch_operation =
            mcd_dispatch_operation_get_path (context->operation);
        properties =
            mcd_dispatch_operation_get_properties (context->operation);
        channel_details =
            _mcd_dispatch_operation_dup_channel_details (context->operation);

        context->approvers_invoked++;
        _mcd_dispatch_operation_block_finished (context->operation);

        mcd_dispatcher_context_ref (context);
        mc_cli_client_approver_call_add_dispatch_operation (client->proxy, -1,
            channel_details, dispatch_operation, properties,
            add_dispatch_operation_cb,
            context, (GDestroyNotify)mcd_dispatcher_context_unref,
            (GObject *)context->dispatcher);

        g_boxed_free (TP_ARRAY_TYPE_CHANNEL_DETAILS_LIST, channel_details);
    }

    /* This matches the approvers count set to 1 at the beginning of the
     * function */
    mcd_dispatcher_context_approver_not_invoked (context);
}

static gboolean
handlers_can_bypass_approval (McdDispatcherContext *context)
{
    McdClient *handler;
    GList *cl;

    for (cl = context->channels; cl != NULL; cl = cl->next)
    {
        McdChannel *channel = MCD_CHANNEL (cl->data);

        handler = get_default_handler (context->dispatcher, channel);
        if (!handler || !handler->bypass_approver)
            return FALSE;
    }
    return TRUE;
}

/* Happens at the end of successful filter chain execution (empty chain
 * is always successful)
 */
static void
mcd_dispatcher_run_clients (McdDispatcherContext *context)
{
    mcd_dispatcher_context_ref (context);
    context->client_locks = 1; /* we release this lock at the end of the
                                    function */

    mcd_dispatcher_run_observers (context);

    if (context->operation)
    {
        /* if we have a dispatch operation, it means that the channels were not
         * requested: start the Approvers */

        /* but if the handlers have the BypassApproval flag set, then don't */
        if (!context->skip_approval &&
            !handlers_can_bypass_approval (context))
            mcd_dispatcher_run_approvers (context);
    }

    mcd_dispatcher_context_release_client_lock (context);
    mcd_dispatcher_context_unref (context);
}

static void
_mcd_dispatcher_context_abort (McdDispatcherContext *context,
                               const GError *error)
{
    GList *list;

    g_return_if_fail(context);

    for (list = context->channels; list != NULL; list = list->next)
    {
        McdChannel *channel = MCD_CHANNEL (list->data);

        if (mcd_channel_get_error (channel) == NULL)
            mcd_channel_take_error (channel, g_error_copy (error));

        /* FIXME: try to dispatch the channels to another handler, instead
         * of just aborting them */
        mcd_mission_abort (MCD_MISSION (channel));
    }
    mcd_dispatcher_context_unref (context);
}

static void
on_channel_abort_context (McdChannel *channel, McdDispatcherContext *context)
{
    const GError *error;
    GList *li = g_list_find (context->channels, channel);

    DEBUG ("Channel %p aborted while in a dispatcher context", channel);

    /* if it was a channel request, and it was cancelled, then the whole
     * context should be aborted */
    error = mcd_channel_get_error (channel);
    if (error && error->code == TP_ERROR_CANCELLED)
        context->cancelled = TRUE;

    /* Losing the channel might mean we get freed, which would make some of
     * the operations below very unhappy */
    mcd_dispatcher_context_ref (context);

    if (context->operation)
    {
        /* the CDO owns the linked list and we just borrow it; in case it's
         * the head of the list that we're deleting, we need to ask the CDO
         * to update our idea of what the list is before emitting any signals.
         *
         * FIXME: this is alarmingly fragile */
        _mcd_dispatch_operation_lose_channel (context->operation, channel,
                                              &(context->channels));

        if (li != NULL)
        {
            /* we used to have a ref to it, until the CDO removed it from the
             * linked list. (Do not dereference li at this point - it has
             * been freed!) */
            g_object_unref (channel);
        }
    }
    else
    {
        /* we own the linked list */
        context->channels = g_list_delete_link (context->channels, li);
    }

    if (context->channels == NULL)
    {
        DEBUG ("Nothing left in this context");
    }

    mcd_dispatcher_context_unref (context);
}

static void
on_operation_finished (McdDispatchOperation *operation,
                       McdDispatcherContext *context)
{
    /* This is emitted when the HandleWith() or Claimed() are invoked on the
     * CDO: according to which of these have happened, we run the choosen
     * handler or we don't. */

    if (context->dispatcher->priv->operation_list_active)
    {
        mc_svc_channel_dispatcher_interface_operation_list_emit_dispatch_operation_finished (
            context->dispatcher, mcd_dispatch_operation_get_path (operation));
    }

    if (context->channels == NULL)
    {
        DEBUG ("Nothing left to dispatch");
        mcd_dispatcher_context_handler_done (context);
    }
    else if (mcd_dispatch_operation_is_claimed (operation))
    {
        GList *list;

        /* we don't release the client lock, in order to not run the handlers.
         * But we have to mark all channels as dispatched, and free the
         * @context */
        for (list = context->channels; list != NULL; list = list->next)
        {
            McdChannel *channel = MCD_CHANNEL (list->data);

            /* TODO: abort the channel if the handler dies */
            _mcd_channel_set_status (channel, MCD_CHANNEL_STATUS_DISPATCHED);
            g_signal_emit_by_name (context->dispatcher, "dispatched", channel);
        }

        mcd_dispatcher_context_handler_done (context);
    }
    else
    {
        /* this is the lock set in mcd_dispatcher_run_approvers(): releasing
         * this will make the handlers run */
        mcd_dispatcher_context_release_client_lock (context);
    }
}

/* ownership of channels, possible_handlers is stolen */
static void
_mcd_dispatcher_enter_state_machine (McdDispatcher *dispatcher,
                                     GList *channels,
                                     GStrv possible_handlers,
                                     gboolean requested)
{
    McdDispatcherContext *context;
    McdDispatcherPrivate *priv;
    GList *list;
    McdChannel *channel;
    McdAccount *account;

    g_return_if_fail (MCD_IS_DISPATCHER (dispatcher));
    g_return_if_fail (channels != NULL);
    g_return_if_fail (MCD_IS_CHANNEL (channels->data));

    account = mcd_channel_get_account (channels->data);
    if (G_UNLIKELY (!account))
    {
        g_warning ("%s called with no account", G_STRFUNC);
        return;
    }

    priv = dispatcher->priv;

    /* Preparing and filling the context */
    context = g_new0 (McdDispatcherContext, 1);
    context->ref_count = 1;
    context->dispatcher = dispatcher;
    context->account = account;
    context->channels = channels;
    context->chain = priv->filters;
    context->possible_handlers = possible_handlers;

    priv->contexts = g_list_prepend (priv->contexts, context);
    if (!requested)
    {
        context->operation =
            _mcd_dispatch_operation_new (priv->dbus_daemon, channels,
                                         possible_handlers);

        if (priv->operation_list_active)
        {
            mc_svc_channel_dispatcher_interface_operation_list_emit_new_dispatch_operation (
                dispatcher,
                mcd_dispatch_operation_get_path (context->operation),
                mcd_dispatch_operation_get_properties (context->operation));
        }

        g_signal_connect (context->operation, "finished",
                          G_CALLBACK (on_operation_finished), context);
    }

    for (list = channels; list != NULL; list = list->next)
    {
        channel = MCD_CHANNEL (list->data);

        g_object_ref (channel); /* We hold separate refs for state machine */
        g_signal_connect_after (channel, "abort",
                                G_CALLBACK (on_channel_abort_context),
                                context);
    }

    if (priv->filters != NULL)
    {
        DEBUG ("entering state machine for context %p", context);

        sp_timestamp ("invoke internal filters");
	mcd_dispatcher_context_process (context, TRUE);
    }
    else
    {
        DEBUG ("No filters found for context %p, "
               "starting the channel handler", context);
	mcd_dispatcher_run_clients (context);
    }
}

static void
_mcd_dispatcher_set_property (GObject * obj, guint prop_id,
			      const GValue * val, GParamSpec * pspec)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (obj);
    McdMaster *master;

    switch (prop_id)
    {
    case PROP_DBUS_DAEMON:
	if (priv->dbus_daemon)
	    g_object_unref (priv->dbus_daemon);
	priv->dbus_daemon = TP_DBUS_DAEMON (g_value_dup_object (val));
	break;
    case PROP_MCD_MASTER:
	master = g_value_get_object (val);
	g_object_ref (G_OBJECT (master));
	if (priv->master)
        {
            g_signal_handlers_disconnect_by_func (G_OBJECT (master), G_CALLBACK (on_master_abort), NULL);
	    g_object_unref (priv->master);
        }
	priv->master = master;
        g_signal_connect (G_OBJECT (master), "abort", G_CALLBACK (on_master_abort), priv);
	break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
	break;
    }
}

static const char * const interfaces[] = {
    MC_IFACE_CHANNEL_DISPATCHER_INTERFACE_OPERATION_LIST,
    NULL
};

static void
_mcd_dispatcher_get_property (GObject * obj, guint prop_id,
			      GValue * val, GParamSpec * pspec)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (obj);

    switch (prop_id)
    {
    case PROP_DBUS_DAEMON:
	g_value_set_object (val, priv->dbus_daemon);
	break;

    case PROP_MCD_MASTER:
	g_value_set_object (val, priv->master);
	break;

    case PROP_INTERFACES:
        g_value_set_static_boxed (val, interfaces);
        break;

    case PROP_DISPATCH_OPERATIONS:
        {
            GList *iter;
            GPtrArray *operations = g_ptr_array_new ();

            /* Side-effect: from now on, emit change notification signals for
             * this property */
            priv->operation_list_active = TRUE;

            for (iter = priv->contexts; iter != NULL; iter = iter->next)
            {
                McdDispatcherContext *context = iter->data;

                if (context->operation != NULL)
                {
                    GValueArray *va = g_value_array_new (2);

                    g_value_array_append (va, NULL);
                    g_value_array_append (va, NULL);

                    g_value_init (va->values + 0, DBUS_TYPE_G_OBJECT_PATH);
                    g_value_init (va->values + 1,
                                  TP_HASH_TYPE_STRING_VARIANT_MAP);

                    g_value_set_boxed (va->values + 0,
                        mcd_dispatch_operation_get_path (context->operation));
                    g_value_set_boxed (va->values + 1,
                        mcd_dispatch_operation_get_properties (
                            context->operation));

                    g_ptr_array_add (operations, va);
                }
            }

            g_value_take_boxed (val, operations);
        }
        break;

    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
	break;
    }
}

static void
_mcd_dispatcher_finalize (GObject * object)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (object);

    if (priv->filters)
    {
        GList *list;
        for (list = priv->filters; list != NULL; list = list->next)
            g_slice_free (McdFilter, list->data);
        g_list_free (priv->filters);
    }

    G_OBJECT_CLASS (mcd_dispatcher_parent_class)->finalize (object);
}

static void
_mcd_dispatcher_dispose (GObject * object)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (object);
    
    if (priv->is_disposed)
    {
	return;
    }
    priv->is_disposed = TRUE;

    g_hash_table_destroy (priv->clients);

    if (priv->master)
    {
	g_object_unref (priv->master);
	priv->master = NULL;
    }

    if (priv->dbus_daemon)
    {
	g_object_unref (priv->dbus_daemon);
	priv->dbus_daemon = NULL;
    }

    G_OBJECT_CLASS (mcd_dispatcher_parent_class)->dispose (object);
}

static GHashTable *
parse_client_filter (GKeyFile *file, const gchar *group)
{
    GHashTable *filter;
    gchar **keys;
    gsize len = 0;
    guint i;

    filter = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                    (GDestroyNotify) tp_g_value_slice_free);

    keys = g_key_file_get_keys (file, group, &len, NULL);
    for (i = 0; i < len; i++)
    {
        const gchar *key;
        const gchar *space;
        gchar *file_property;
        gchar file_property_type;

        key = keys[i];
        space = g_strrstr (key, " ");

        if (space == NULL || space[1] == '\0' || space[2] != '\0')
        {
            g_warning ("Invalid key %s in client file", key);
            continue;
        }
        file_property_type = space[1];
        file_property = g_strndup (key, space - key);

        switch (file_property_type)
        {
        case 'q':
        case 'u':
        case 't': /* unsigned integer */
            {
                /* g_key_file_get_integer cannot be used because we need
                 * to support 64 bits */
                guint x;
                GValue *value = tp_g_value_slice_new (G_TYPE_UINT64);
                gchar *str = g_key_file_get_string (file, group, key,
                                                    NULL);
                errno = 0;
                x = g_ascii_strtoull (str, NULL, 0);
                if (errno != 0)
                {
                    g_warning ("Invalid unsigned integer '%s' in client"
                               " file", str);
                }
                else
                {
                    g_value_set_uint64 (value, x);
                    g_hash_table_insert (filter, file_property, value);
                }
                g_free (str);
                break;
            }

        case 'y':
        case 'n':
        case 'i':
        case 'x': /* signed integer */
            {
                gint x;
                GValue *value = tp_g_value_slice_new (G_TYPE_INT64);
                gchar *str = g_key_file_get_string (file, group, key, NULL);
                errno = 0;
                x = g_ascii_strtoll (str, NULL, 0);
                if (errno != 0)
                {
                    g_warning ("Invalid signed integer '%s' in client"
                               " file", str);
                }
                else
                {
                    g_value_set_uint64 (value, x);
                    g_hash_table_insert (filter, file_property, value);
                }
                g_free (str);
                break;
            }

        case 'b':
            {
                GValue *value = tp_g_value_slice_new (G_TYPE_BOOLEAN);
                gboolean b = g_key_file_get_boolean (file, group, key, NULL);
                g_value_set_boolean (value, b);
                g_hash_table_insert (filter, file_property, value);
                break;
            }

        case 's':
            {
                GValue *value = tp_g_value_slice_new (G_TYPE_STRING);
                gchar *str = g_key_file_get_string (file, group, key, NULL);

                g_value_take_string (value, str);
                g_hash_table_insert (filter, file_property, value);
                break;
            }

        case 'o':
            {
                GValue *value = tp_g_value_slice_new
                    (DBUS_TYPE_G_OBJECT_PATH);
                gchar *str = g_key_file_get_string (file, group, key, NULL);

                g_value_take_boxed (value, str);
                g_hash_table_insert (filter, file_property, value);
                break;
            }

        default:
            g_warning ("Invalid key %s in client file", key);
            continue;
        }
    }
    g_strfreev (keys);

    return filter;
}

static void
get_channel_filter_cb (TpProxy *proxy,
                       const GValue *out_Value,
                       const GError *error,
                       gpointer user_data,
                       GObject *weak_object)
{
    GList **client_filters = user_data;
    GPtrArray *filters;
    guint i;

    if (error != NULL)
    {
        DEBUG ("error getting a filter list for client %s: %s #%d: %s",
               tp_proxy_get_object_path (proxy),
               g_quark_to_string (error->domain), error->code, error->message);
        return;
    }

    filters = g_value_get_boxed (out_Value);

    for (i = 0 ; i < filters->len ; i++)
    {
        GHashTable *channel_class = g_ptr_array_index (filters, i);
        GHashTable *new_channel_class;
        GHashTableIter iter;
        gchar *property_name;
        GValue *property_value;
        gboolean valid_filter = TRUE;

        new_channel_class = g_hash_table_new_full
            (g_str_hash, g_str_equal, g_free,
             (GDestroyNotify) tp_g_value_slice_free);

        g_hash_table_iter_init (&iter, channel_class);
        while (g_hash_table_iter_next (&iter, (gpointer *) &property_name,
                                       (gpointer *) &property_value)) 
        {
            GValue *filter_value;
            GType property_type = G_VALUE_TYPE (property_value);

            if (property_type == G_TYPE_BOOLEAN ||
                property_type == G_TYPE_STRING ||
                property_type == DBUS_TYPE_G_OBJECT_PATH)
            {
                filter_value = tp_g_value_slice_new
                    (G_VALUE_TYPE (property_value));
                g_value_copy (property_value, filter_value);
            }
            else if (property_type == G_TYPE_UCHAR ||
                     property_type == G_TYPE_UINT ||
                     property_type == G_TYPE_UINT64)
            {
                filter_value = tp_g_value_slice_new (G_TYPE_UINT64);
                g_value_transform (property_value, filter_value);
            }
            else if (property_type == G_TYPE_INT ||
                     property_type == G_TYPE_INT64)
            {
                filter_value = tp_g_value_slice_new (G_TYPE_INT64);
                g_value_transform (property_value, filter_value);
            }
            else
            {
                /* invalid type, do not add this filter */
                g_warning ("%s: Property %s has an invalid type (%s)",
                           G_STRFUNC, property_name,
                           g_type_name (G_VALUE_TYPE (property_value)));
                valid_filter = FALSE;
                break;
            }

            g_hash_table_insert (new_channel_class, g_strdup (property_name),
                                 filter_value);
        }

        if (valid_filter)
            *client_filters = g_list_prepend
                (*client_filters, new_channel_class);
        else
            g_hash_table_destroy (new_channel_class);
    }
}

static void
client_add_interface_by_id (McdClient *client)
{
    tp_proxy_add_interface_by_id (client->proxy, MC_IFACE_QUARK_CLIENT);
    if (client->interfaces & MCD_CLIENT_APPROVER)
        tp_proxy_add_interface_by_id (client->proxy,
                                      MC_IFACE_QUARK_CLIENT_APPROVER);
    if (client->interfaces & MCD_CLIENT_HANDLER)
        tp_proxy_add_interface_by_id (client->proxy,
                                      MC_IFACE_QUARK_CLIENT_HANDLER);
    if (client->interfaces & MCD_CLIENT_INTERFACE_REQUESTS)
        tp_proxy_add_interface_by_id (client->proxy, MC_IFACE_QUARK_CLIENT_INTERFACE_REQUESTS);
    if (client->interfaces & MCD_CLIENT_OBSERVER)
        tp_proxy_add_interface_by_id (client->proxy,
                                      MC_IFACE_QUARK_CLIENT_OBSERVER);
}

static void
get_interfaces_cb (TpProxy *proxy,
                   const GValue *out_Value,
                   const GError *error,
                   gpointer user_data,
                   GObject *weak_object)
{
    McdDispatcher *self = MCD_DISPATCHER (weak_object);
    /* McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (self); */
    McdClient *client = user_data;
    gchar **arr;

    arr = g_value_get_boxed (out_Value);

    while (arr != NULL && *arr != NULL)
    {
        if (strcmp (*arr, MC_IFACE_CLIENT_APPROVER) == 0)
            client->interfaces |= MCD_CLIENT_APPROVER;
        if (strcmp (*arr, MC_IFACE_CLIENT_HANDLER) == 0)
            client->interfaces |= MCD_CLIENT_HANDLER;
        if (strcmp (*arr, MC_IFACE_CLIENT_INTERFACE_REQUESTS) == 0)
            client->interfaces |= MCD_CLIENT_INTERFACE_REQUESTS;
        if (strcmp (*arr, MC_IFACE_CLIENT_OBSERVER) == 0)
            client->interfaces |= MCD_CLIENT_OBSERVER;
        arr++;
    }

    client_add_interface_by_id (client);
    if (client->interfaces & MCD_CLIENT_APPROVER)
    {
        tp_cli_dbus_properties_call_get
            (client->proxy, -1, MC_IFACE_CLIENT_APPROVER,
             "ApproverChannelFilter", get_channel_filter_cb,
             &client->approver_filters, NULL, G_OBJECT (self));
    }
    if (client->interfaces & MCD_CLIENT_HANDLER)
    {
        tp_cli_dbus_properties_call_get
            (client->proxy, -1, MC_IFACE_CLIENT_HANDLER,
             "HandlerChannelFilter", get_channel_filter_cb,
             &client->handler_filters, NULL, G_OBJECT (self));
    }
    if (client->interfaces & MCD_CLIENT_OBSERVER)
    {
        tp_cli_dbus_properties_call_get
            (client->proxy, -1, MC_IFACE_CLIENT_OBSERVER,
             "ObserverChannelFilter", get_channel_filter_cb,
             &client->observer_filters, NULL, G_OBJECT (self));
    }
}

static void
create_client_proxy (McdDispatcher *self, McdClient *client)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (self);
    gchar *bus_name, *object_path;

    bus_name = g_strconcat (MC_CLIENT_BUS_NAME_BASE, client->name, NULL);
    object_path = g_strconcat (MC_CLIENT_OBJECT_PATH_BASE, client->name, NULL);
    g_strdelimit (object_path, ".", '/');
    client->proxy = g_object_new (TP_TYPE_PROXY,
                                  "dbus-daemon", priv->dbus_daemon,
                                  "object-path", object_path,
                                  "bus-name", bus_name,
                                  NULL);
    g_free (object_path);
    g_free (bus_name);
}

static void
parse_client_file (McdClient *client, GKeyFile *file)
{
    gchar **iface_names, **groups;
    guint i;
    gsize len = 0;

    iface_names = g_key_file_get_string_list (file, MC_IFACE_CLIENT,
                                              "Interfaces", 0, NULL);
    if (!iface_names)
        return;

    for (i = 0; iface_names[i] != NULL; i++)
    {
        if (strcmp (iface_names[i], MC_IFACE_CLIENT_APPROVER) == 0)
            client->interfaces |= MCD_CLIENT_APPROVER;
        else if (strcmp (iface_names[i], MC_IFACE_CLIENT_HANDLER) == 0)
            client->interfaces |= MCD_CLIENT_HANDLER;
        else if (strcmp (iface_names[i], MC_IFACE_CLIENT_INTERFACE_REQUESTS) == 0)
            client->interfaces |= MCD_CLIENT_INTERFACE_REQUESTS;
        else if (strcmp (iface_names[i], MC_IFACE_CLIENT_OBSERVER) == 0)
            client->interfaces |= MCD_CLIENT_OBSERVER;
    }
    g_strfreev (iface_names);

    /* parse filtering rules */
    groups = g_key_file_get_groups (file, &len);
    for (i = 0; i < len; i++)
    {
        if (client->interfaces & MCD_CLIENT_APPROVER &&
            g_str_has_prefix (groups[i], MC_IFACE_CLIENT_APPROVER
                              ".ApproverChannelFilter "))
        {
            client->approver_filters =
                g_list_prepend (client->approver_filters,
                                parse_client_filter (file, groups[i]));
        }
        else if (client->interfaces & MCD_CLIENT_HANDLER &&
            g_str_has_prefix (groups[i], MC_IFACE_CLIENT_HANDLER
                              ".HandlerChannelFilter "))
        {
            client->handler_filters =
                g_list_prepend (client->handler_filters,
                                parse_client_filter (file, groups[i]));
        }
        else if (client->interfaces & MCD_CLIENT_OBSERVER &&
            g_str_has_prefix (groups[i], MC_IFACE_CLIENT_OBSERVER
                              ".ObserverChannelFilter "))
        {
            client->observer_filters =
                g_list_prepend (client->observer_filters,
                                parse_client_filter (file, groups[i]));
        }
    }
    g_strfreev (groups);

    /* Other client options */
    client->bypass_approver =
        g_key_file_get_boolean (file, MC_IFACE_CLIENT_HANDLER,
                                "BypassApproval", NULL);
}

static gchar *
find_client_file (const gchar *client_name)
{
    const gchar * const *dirs;
    const gchar *dirname;
    const gchar *env_dirname;
    gchar *filename, *absolute_filepath;

    /* 
     * The full path is $XDG_DATA_DIRS/telepathy/clients/clientname.client
     * or $XDG_DATA_HOME/telepathy/clients/clientname.client
     * For testing purposes, we also look for $MC_CLIENTS_DIR/clientname.client
     * if $MC_CLIENTS_DIR is set.
     */
    filename = g_strdup_printf ("%s.client", client_name);
    env_dirname = g_getenv ("MC_CLIENTS_DIR");
    if (env_dirname)
    {
        absolute_filepath = g_build_filename (env_dirname, filename, NULL);
        if (g_file_test (absolute_filepath, G_FILE_TEST_IS_REGULAR))
            goto finish;
        g_free (absolute_filepath);
    }

    dirname = g_get_user_data_dir ();
    if (G_LIKELY (dirname))
    {
        absolute_filepath = g_build_filename (dirname, "telepathy/clients",
                                              filename, NULL);
        if (g_file_test (absolute_filepath, G_FILE_TEST_IS_REGULAR))
            goto finish;
        g_free (absolute_filepath);
    }

    dirs = g_get_system_data_dirs ();
    for (dirname = *dirs; dirname != NULL; dirs++, dirname = *dirs)
    {
        absolute_filepath = g_build_filename (dirname, "telepathy/clients",
                                              filename, NULL);
        if (g_file_test (absolute_filepath, G_FILE_TEST_IS_REGULAR))
            goto finish;
        g_free (absolute_filepath);
    }

    absolute_filepath = NULL;
finish:
    g_free (filename);
    return absolute_filepath;
}

static McdClient *
create_mcd_client (McdDispatcher *self,
                   const gchar *name,
                   gboolean activatable)
{
    /* McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (self); */
    McdClient *client;
    gchar *filename;
    gboolean file_found = FALSE;

    g_assert (g_str_has_prefix (name, MC_CLIENT_BUS_NAME_BASE));

    client = g_slice_new0 (McdClient);
    client->name = g_strdup (name + sizeof (MC_CLIENT_BUS_NAME_BASE) - 1);
    client->activatable = activatable;
    if (!activatable)
        client->active = TRUE;
    DEBUG ("McdClient created for %s", name);

    /* The .client file is not mandatory as per the spec. However if it
     * exists, it is better to read it than activating the service to read the
     * D-Bus properties.
     */
    filename = find_client_file (client->name);
    if (filename)
    {
        GKeyFile *file;
        GError *error = NULL;

        file = g_key_file_new ();
        g_key_file_load_from_file (file, filename, 0, &error);
        if (G_LIKELY (!error))
        {
            DEBUG ("File found for %s: %s", name, filename);
            parse_client_file (client, file);
            file_found = TRUE;
        }
        else
        {
            g_warning ("Loading file %s failed: %s", filename, error->message);
            g_error_free (error);
        }
        g_key_file_free (file);
        g_free (filename);
    }

    create_client_proxy (self, client);

    if (!file_found)
    {
        DEBUG ("No .client file for %s. Ask on D-Bus.", name);
        tp_cli_dbus_properties_call_get (client->proxy, -1,
            MC_IFACE_CLIENT, "Interfaces", get_interfaces_cb, client,
            NULL, G_OBJECT (self));
    }
    else
        client_add_interface_by_id (client);


    return client;
}

/* Check the list of strings whether they are valid well-known names of
 * Telepathy clients and create McdClient objects for each of them.
 */
static void
new_names_cb (McdDispatcher *self,
              const gchar **names,
              gboolean activatable)
{
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (self);
  
    while (names != NULL && *names != NULL)
    {
        McdClient *client;
        const char *name = *names;
        names++;

        if (!g_str_has_prefix (name, MC_CLIENT_BUS_NAME_BASE))
        {
            /* This is not a Telepathy Client */
            continue;
        }

        client = g_hash_table_lookup (priv->clients, name);
        if (client)
        {
            /* This Telepathy Client is already known so don't create it
             * again. However, set the activatable bit now.
             */
            if (activatable)
                client->activatable = TRUE;
            else
                client->active = TRUE;
            continue;
        }

        DEBUG ("Register client %s", name);

        g_hash_table_insert (priv->clients, g_strdup (name),
            create_mcd_client (self, name, activatable));
    }
}

static void
list_names_cb (TpDBusDaemon *proxy,
    const gchar **out0,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
    McdDispatcher *self = MCD_DISPATCHER (weak_object);

    new_names_cb (self, out0, FALSE);
}

static void
list_activatable_names_cb (TpDBusDaemon *proxy,
    const gchar **out0,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
    McdDispatcher *self = MCD_DISPATCHER (weak_object);

    new_names_cb (self, out0, TRUE);
}

static void
name_owner_changed_cb (TpDBusDaemon *proxy,
    const gchar *arg0,
    const gchar *arg1,
    const gchar *arg2,
    gpointer user_data,
    GObject *weak_object)
{
    McdDispatcher *self = MCD_DISPATCHER (weak_object);
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (self);

    if (g_strcmp0 (arg1, "") == 0 && g_strcmp0 (arg2, "") != 0)
    {
        /* the name appeared on the bus */
        gchar *names[2] = {NULL, NULL};
        names[0] = g_strdup (arg0);
        new_names_cb (self, (const gchar **) names, FALSE);
        g_free (names[0]);
    }
    else if (g_strcmp0 (arg1, "") != 0 && g_strcmp0 (arg2, "") == 0)
    {
        /* The name disappeared from the bus */
        McdClient *client;

        client = g_hash_table_lookup (priv->clients, arg0);
        if (client)
        {
            if (!client->activatable)
                g_hash_table_remove (priv->clients, arg0);
            else
            {
                client->active = FALSE;
                if (client->handled_channels)
                {
                    g_strfreev (client->handled_channels);
                    client->handled_channels = NULL;
                }
            }
        }
    }
    else if (g_strcmp0 (arg1, "") != 0 && g_strcmp0 (arg2, "") != 0)
    {
        /* The name's ownership changed. Does the Telepathy spec allow that?
         * TODO: Do something smart
         */
        g_warning ("%s: The ownership of name '%s' changed", G_STRFUNC, arg0);
    }
    else
    {
        /* dbus-daemon is sick */
        g_warning ("%s: Malformed message from the D-Bus daemon about '%s'",
                   G_STRFUNC, arg0);
    }
}

static void
mcd_dispatcher_constructed (GObject *object)
{
    DBusGConnection *dgc;
    McdDispatcherPrivate *priv = MCD_DISPATCHER_PRIV (object);
    DBusError error = { 0 };

    tp_cli_dbus_daemon_connect_to_name_owner_changed (priv->dbus_daemon,
        name_owner_changed_cb, NULL, NULL, object, NULL);

    tp_cli_dbus_daemon_call_list_activatable_names (priv->dbus_daemon,
        -1, list_activatable_names_cb, NULL, NULL, object);

    tp_cli_dbus_daemon_call_list_names (priv->dbus_daemon,
        -1, list_names_cb, NULL, NULL, object);

    dgc = TP_PROXY (priv->dbus_daemon)->dbus_connection;

    dbus_bus_request_name (dbus_g_connection_get_connection (dgc),
                           MCD_CHANNEL_DISPATCHER_BUS_NAME, 0, &error);

    if (dbus_error_is_set (&error))
    {
        /* FIXME: put in proper error handling when MC gains the ability to
         * be the AM or the CD but not both */
        g_error ("Unable to be the channel dispatcher: %s: %s", error.name,
                 error.message);
        dbus_error_free (&error);
        return;
    }

    dbus_g_connection_register_g_object (dgc,
                                         MCD_CHANNEL_DISPATCHER_OBJECT_PATH,
                                         object);
}

static void
mcd_dispatcher_class_init (McdDispatcherClass * klass)
{
    static TpDBusPropertiesMixinPropImpl cd_props[] = {
        { "Interfaces", "interfaces", NULL },
        { NULL }
    };
    static TpDBusPropertiesMixinPropImpl op_list_props[] = {
        { "DispatchOperations", "dispatch-operations", NULL },
        { NULL }
    };
    static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
        { MC_IFACE_CHANNEL_DISPATCHER,
          tp_dbus_properties_mixin_getter_gobject_properties,
          NULL,
          cd_props,
        },
        { MC_IFACE_CHANNEL_DISPATCHER_INTERFACE_OPERATION_LIST,
          tp_dbus_properties_mixin_getter_gobject_properties,
          NULL,
          op_list_props,
        },
        { NULL }
    };
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (McdDispatcherPrivate));

    object_class->constructed = mcd_dispatcher_constructed;
    object_class->set_property = _mcd_dispatcher_set_property;
    object_class->get_property = _mcd_dispatcher_get_property;
    object_class->finalize = _mcd_dispatcher_finalize;
    object_class->dispose = _mcd_dispatcher_dispose;

    tp_proxy_or_subclass_hook_on_interface_add
        (TP_TYPE_PROXY, mc_cli_client_add_signals);

    signals[CHANNEL_ADDED] =
	g_signal_new ("channel_added",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      G_STRUCT_OFFSET (McdDispatcherClass,
				       channel_added_signal),
		      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
		      G_TYPE_NONE, 1, MCD_TYPE_CHANNEL);
    
    signals[CHANNEL_REMOVED] =
	g_signal_new ("channel_removed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      G_STRUCT_OFFSET (McdDispatcherClass,
				       channel_removed_signal),
		      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
		      G_TYPE_NONE, 1, MCD_TYPE_CHANNEL);
    
    signals[DISPATCHED] =
	g_signal_new ("dispatched",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      G_STRUCT_OFFSET (McdDispatcherClass,
				       dispatched_signal),
		      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
		      G_TYPE_NONE, 1, MCD_TYPE_CHANNEL);
    
    signals[DISPATCH_FAILED] =
	g_signal_new ("dispatch-failed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      G_STRUCT_OFFSET (McdDispatcherClass,
				       dispatch_failed_signal),
		      NULL, NULL, _mcd_marshal_VOID__OBJECT_POINTER,
		      G_TYPE_NONE, 2, MCD_TYPE_CHANNEL, G_TYPE_POINTER);

    /**
     * McdDispatcher::dispatch-completed:
     * @dispatcher: the #McdDispatcher.
     * @context: a #McdDispatcherContext.
     *
     * This signal is emitted when a dispatch operation has terminated. One can
     * inspect @context to get the status of the channels.
     * After this signal returns, @context is no longer valid.
     */
    signals[DISPATCH_COMPLETED] =
        g_signal_new ("dispatch-completed",
                      G_OBJECT_CLASS_TYPE (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);

    /* Properties */
    g_object_class_install_property
        (object_class, PROP_DBUS_DAEMON,
         g_param_spec_object ("dbus-daemon", "DBus daemon", "DBus daemon",
                              TP_TYPE_DBUS_DAEMON,
                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
    g_object_class_install_property
        (object_class, PROP_MCD_MASTER,
         g_param_spec_object ("mcd-master", "McdMaster", "McdMaster",
                              MCD_TYPE_MASTER,
                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

    g_object_class_install_property
        (object_class, PROP_INTERFACES,
         g_param_spec_boxed ("interfaces", "Interfaces", "Interfaces",
                             G_TYPE_STRV,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property
        (object_class, PROP_DISPATCH_OPERATIONS,
         g_param_spec_boxed ("dispatch-operations",
                             "ChannelDispatchOperation details",
                             "A dbus-glib a(oa{sv})",
                             MC_ARRAY_TYPE_DISPATCH_OPERATION_DETAILS_LIST,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    client_ready_quark = g_quark_from_static_string ("mcd_client_ready");

    klass->dbus_properties_class.interfaces = prop_interfaces,
    tp_dbus_properties_mixin_class_init (object_class,
        G_STRUCT_OFFSET (McdDispatcherClass, dbus_properties_class));
}

static void
_build_channel_capabilities (const gchar *channel_type, guint type_flags,
			     GPtrArray *capabilities)
{
    GValue cap = {0,};
    GType cap_type;

    cap_type = dbus_g_type_get_struct ("GValueArray", G_TYPE_STRING,
				       G_TYPE_UINT, G_TYPE_INVALID);
    g_value_init (&cap, cap_type);
    g_value_take_boxed (&cap, dbus_g_type_specialized_construct (cap_type));

    dbus_g_type_struct_set (&cap,
			    0, channel_type,
			    1, type_flags,
			    G_MAXUINT);

    g_ptr_array_add (capabilities, g_value_get_boxed (&cap));
}

static void
mcd_dispatcher_init (McdDispatcher * dispatcher)
{
    McdDispatcherPrivate *priv;

    priv = G_TYPE_INSTANCE_GET_PRIVATE (dispatcher, MCD_TYPE_DISPATCHER,
                                        McdDispatcherPrivate);
    dispatcher->priv = priv;

    priv->operation_list_active = FALSE;

    priv->clients = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
        (GDestroyNotify) mcd_client_free);
}

McdDispatcher *
mcd_dispatcher_new (TpDBusDaemon *dbus_daemon, McdMaster *master)
{
    McdDispatcher *obj;
    obj = MCD_DISPATCHER (g_object_new (MCD_TYPE_DISPATCHER,
					"dbus-daemon", dbus_daemon,
					"mcd-master", master, 
					NULL));
    return obj;
}

/* The new state machine walker function for pluginized filters*/

void
mcd_dispatcher_context_process (McdDispatcherContext * context, gboolean result)
{
    if (result && !context->cancelled)
    {
	McdFilter *filter;

	filter = g_list_nth_data (context->chain, context->next_func_index);
	/* Do we still have functions to go through? */
	if (filter)
	{
	    context->next_func_index++;
	    
            DEBUG ("Next filter");
	    filter->func (context, filter->user_data);
	    return; /*State machine goes on...*/
	}
	else
	{
	    /* Context would be destroyed somewhere in this call */
	    mcd_dispatcher_run_clients (context);
	}
    }
    else
    {
        GError error;

        if (context->cancelled)
        {
            error.domain = TP_ERRORS;
            error.code = TP_ERROR_CANCELLED;
            error.message = "Context cancelled";
        }
        else
        {
            DEBUG ("Filters failed, disposing request");
            error.domain = TP_ERRORS;
            error.code = TP_ERROR_NOT_AVAILABLE;
            error.message = "Filters failed";
        }
        _mcd_dispatcher_context_abort (context, &error);
    }
}

static void
mcd_dispatcher_context_unref (McdDispatcherContext * context)
{
    McdDispatcherPrivate *priv;
    GList *list;

    /* FIXME: check for leaks */
    g_return_if_fail (context);
    g_return_if_fail (context->ref_count > 0);

    DEBUG ("called on %p (ref = %d)", context, context->ref_count);
    context->ref_count--;
    if (context->ref_count == 0)
    {
        DEBUG ("freeing the context %p", context);
        for (list = context->channels; list != NULL; list = list->next)
        {
            McdChannel *channel = MCD_CHANNEL (list->data);
            g_signal_handlers_disconnect_by_func (channel,
                G_CALLBACK (on_channel_abort_context), context);
            g_object_unref (channel);
        }
        /* disposing the dispatch operation also frees the channels list */
        if (context->operation)
        {
            g_signal_handlers_disconnect_by_func (context->operation,
                                                  on_operation_finished,
                                                  context);
            g_object_unref (context->operation);
        }
        else
            g_list_free (context->channels);

        /* remove the context from the list of active contexts */
        priv = MCD_DISPATCHER_PRIV (context->dispatcher);
        priv->contexts = g_list_remove (priv->contexts, context);

        g_strfreev (context->possible_handlers);
        g_free (context->protocol);
        g_free (context);
    }
}

/* CONTEXT API */

/* Context getters */
TpChannel *
mcd_dispatcher_context_get_channel_object (McdDispatcherContext * ctx)
{
    TpChannel *tp_chan;
    g_return_val_if_fail (ctx, 0);
    g_object_get (G_OBJECT (mcd_dispatcher_context_get_channel (ctx)),
                  "tp-channel", &tp_chan, NULL);
    g_object_unref (G_OBJECT (tp_chan));
    return tp_chan;
}

McdDispatcher*
mcd_dispatcher_context_get_dispatcher (McdDispatcherContext * ctx)
{
    return ctx->dispatcher;
}

/**
 * mcd_dispatcher_context_get_connection:
 * @context: the #McdDispatcherContext.
 *
 * Returns: the #McdConnection.
 */
McdConnection *
mcd_dispatcher_context_get_connection (McdDispatcherContext *context)
{
    g_return_val_if_fail (context, NULL);
    g_return_val_if_fail (context->channels != NULL, NULL);
    return MCD_CONNECTION (mcd_mission_get_parent
                           (MCD_MISSION (context->channels->data)));
}

TpConnection *
mcd_dispatcher_context_get_connection_object (McdDispatcherContext * ctx)
{
    const McdConnection *connection;
    TpConnection *tp_conn;
   
    connection = mcd_dispatcher_context_get_connection (ctx); 
    g_object_get (G_OBJECT (connection), "tp-connection",
		  &tp_conn, NULL);
   
    g_object_unref (tp_conn); 
    return tp_conn;
}

McdChannel *
mcd_dispatcher_context_get_channel (McdDispatcherContext * ctx)
{
    g_return_val_if_fail (ctx, 0);
    if (ctx->main_channel)
        return ctx->main_channel;
    else
        return ctx->channels ? MCD_CHANNEL (ctx->channels->data) : NULL;
}

/**
 * mcd_dispatcher_context_get_channels:
 * @context: the #McdDispatcherContext.
 *
 * Returns: a #GList of #McdChannel elements.
 */
const GList *
mcd_dispatcher_context_get_channels (McdDispatcherContext *context)
{
    g_return_val_if_fail (context != NULL, NULL);
    return context->channels;
}

/**
 * mcd_dispatcher_context_get_channel_by_type:
 * @context: the #McdDispatcherContext.
 * @type: the #GQuark representing the channel type.
 *
 * Returns: the first #McdChannel of the requested type, or %NULL.
 */
McdChannel *
mcd_dispatcher_context_get_channel_by_type (McdDispatcherContext *context,
                                            GQuark type)
{
    GList *list;

    g_return_val_if_fail (context != NULL, NULL);
    for (list = context->channels; list != NULL; list = list->next)
    {
        McdChannel *channel = MCD_CHANNEL (list->data);

        if (mcd_channel_get_channel_type_quark (channel) == type)
            return channel;
    }
    return NULL;
}

GPtrArray *
_mcd_dispatcher_get_channel_capabilities (McdDispatcher *dispatcher,
                                          const gchar *protocol)
{
    McdDispatcherPrivate *priv = dispatcher->priv;
    GPtrArray *channel_handler_caps;
    GHashTableIter iter;
    gpointer key, value;

    channel_handler_caps = g_ptr_array_new ();

    /* Add the capabilities from the new-style clients */
    g_hash_table_iter_init (&iter, priv->clients);
    while (g_hash_table_iter_next (&iter, &key, &value)) 
    {
        McdClient *client = value;
        GList *list;

        for (list = client->handler_filters; list != NULL; list = list->next)
        {
            GHashTable *channel_class = list->data;
            const gchar *channel_type;
            guint type_flags;

            channel_type = tp_asv_get_string (channel_class,
                                              TP_IFACE_CHANNEL ".ChannelType");
            if (!channel_type) continue;

            /* There is currently no way to map the HandlerChannelFilter client
             * property into type-specific capabilities. Let's pretend we
             * support everything. */
            type_flags = 0xffffffff;

            _build_channel_capabilities (channel_type, type_flags,
                                         channel_handler_caps);
        }
    }
    return channel_handler_caps;
}

GPtrArray *
_mcd_dispatcher_get_channel_enhanced_capabilities (McdDispatcher *dispatcher)
{
    McdDispatcherPrivate *priv = dispatcher->priv;
    GHashTableIter iter;
    gpointer key, value;
    GPtrArray *caps = g_ptr_array_new ();

    g_hash_table_iter_init (&iter, priv->clients);
    while (g_hash_table_iter_next (&iter, &key, &value)) 
    {
        McdClient *client = value;
        GList *list;

        for (list = client->handler_filters; list != NULL; list = list->next)
        {
            GHashTable *channel_class = list->data;
            guint i;
            gboolean already_in_caps = FALSE;

            /* Check if the filter is already in the caps variable */
            for (i = 0 ; i < caps->len ; i++)
            {
                GHashTable *channel_class2 = g_ptr_array_index (caps, i);
                if (channel_classes_equals (channel_class, channel_class2))
                {
                    already_in_caps = TRUE;
                    break;
                }
            }

            if (! already_in_caps)
                g_ptr_array_add (caps, channel_class);
        }
    }

    return caps;
}

const gchar *
mcd_dispatcher_context_get_protocol_name (McdDispatcherContext *context)
{
    McdConnection *conn;
    McdAccount *account;

    if (!context->protocol)
    {
	conn = mcd_dispatcher_context_get_connection (context);
	account = mcd_connection_get_account (conn);
	context->protocol = g_strdup (mcd_account_get_protocol_name (account));
    }
    
    return context->protocol;
}

static void
remove_request_data_free (McdRemoveRequestData *rrd)
{
    g_object_unref (rrd->handler);
    g_free (rrd->request_path);
    g_slice_free (McdRemoveRequestData, rrd);
}

static void
on_request_status_changed (McdChannel *channel, McdChannelStatus status,
                           McdRemoveRequestData *rrd)
{
    if (status != MCD_CHANNEL_STATUS_FAILED &&
        status != MCD_CHANNEL_STATUS_DISPATCHED)
        return;

    DEBUG ("called, %u", status);
    if (status == MCD_CHANNEL_STATUS_FAILED)
    {
        const GError *error;
        gchar *err_string;
        error = mcd_channel_get_error (channel);
        err_string = _mcd_build_error_string (error);
        /* no callback, as we don't really care */
        mc_cli_client_interface_requests_call_remove_request
            (rrd->handler, -1, rrd->request_path, err_string, error->message,
             NULL, NULL, NULL, NULL);
        g_free (err_string);
    }

    /* we don't need the McdRemoveRequestData anymore */
    remove_request_data_free (rrd);
    g_signal_handlers_disconnect_by_func (channel, on_request_status_changed,
                                          rrd);
}

/*
 * _mcd_dispatcher_add_request:
 * @context: the #McdDispatcherContext.
 * @account: the #McdAccount.
 * @channels: a #McdChannel in MCD_CHANNEL_REQUEST state.
 *
 * Add a request; this basically means invoking AddRequest (and maybe
 * RemoveRequest) on the channel handler.
 */
void
_mcd_dispatcher_add_request (McdDispatcher *dispatcher, McdAccount *account,
                             McdChannel *channel)
{
    McdDispatcherPrivate *priv;
    McdClient *handler = NULL;
    GHashTable *properties;
    GValue v_user_time = { 0, };
    GValue v_requests = { 0, };
    GValue v_account = { 0, };
    GValue v_preferred_handler = { 0, };
    GValue v_interfaces = { 0, };
    GPtrArray *requests;
    McdRemoveRequestData *rrd;

    g_return_if_fail (MCD_IS_DISPATCHER (dispatcher));
    g_return_if_fail (MCD_IS_CHANNEL (channel));

    priv = dispatcher->priv;

    handler = get_default_handler (dispatcher, channel);
    if (!handler)
    {
        /* No handler found. But it's possible that by the time that the
         * channel will be created some handler will have popped up, so we
         * must not destroy it. */
        DEBUG ("No handler for request %s",
               _mcd_channel_get_request_path (channel));
        return;
    }

    if (!(handler->interfaces & MCD_CLIENT_INTERFACE_REQUESTS))
    {
        DEBUG ("Default handler %s for request %s doesn't want AddRequest",
               handler->name, _mcd_channel_get_request_path (channel));
        return;
    }

    DEBUG ("Calling AddRequest on default handler %s for request %s",
           handler->name, _mcd_channel_get_request_path (channel));

    properties = g_hash_table_new (g_str_hash, g_str_equal);

    g_value_init (&v_user_time, G_TYPE_UINT64);
    g_value_set_uint64 (&v_user_time,
                        _mcd_channel_get_request_user_action_time (channel));
    g_hash_table_insert (properties, "org.freedesktop.Telepathy.ChannelRequest"
                         ".UserActionTime", &v_user_time);

    requests = g_ptr_array_sized_new (1);
    g_ptr_array_add (requests,
                     _mcd_channel_get_requested_properties (channel));
    g_value_init (&v_requests, dbus_g_type_get_collection ("GPtrArray",
         TP_HASH_TYPE_QUALIFIED_PROPERTY_VALUE_MAP));
    g_value_set_static_boxed (&v_requests, requests);
    g_hash_table_insert (properties, "org.freedesktop.Telepathy.ChannelRequest"
                         ".Requests", &v_requests);

    g_value_init (&v_account, DBUS_TYPE_G_OBJECT_PATH);
    g_value_set_static_boxed (&v_account,
                              mcd_account_get_object_path (account));
    g_hash_table_insert (properties, "org.freedesktop.Telepathy.ChannelRequest"
                         ".Account", &v_account);

    g_value_init (&v_interfaces, G_TYPE_STRV);
    g_value_set_static_boxed (&v_interfaces, NULL);
    g_hash_table_insert (properties, "org.freedesktop.Telepathy.ChannelRequest"
                         ".Interfaces", &v_interfaces);

    g_value_init (&v_preferred_handler, G_TYPE_STRING);
    g_value_set_static_string (&v_preferred_handler,
        _mcd_channel_get_request_preferred_handler (channel));
    g_hash_table_insert (properties, "org.freedesktop.Telepathy.ChannelRequest"
                         ".PreferredHandler", &v_preferred_handler);

    mc_cli_client_interface_requests_call_add_request (
        handler->proxy, -1,
        _mcd_channel_get_request_path (channel), properties,
        NULL, NULL, NULL, NULL);

    g_hash_table_unref (properties);
    g_ptr_array_free (requests, TRUE);

    /* Prepare for a RemoveRequest */
    rrd = g_slice_new (McdRemoveRequestData);
    /* store the request path, because it might not be available when the
     * channel status changes */
    rrd->request_path = g_strdup (_mcd_channel_get_request_path (channel));
    rrd->handler = handler->proxy;
    g_object_ref (handler->proxy);
    /* we must watch whether the request fails and in that case call
     * RemoveRequest */
    g_signal_connect (channel, "status-changed",
                      G_CALLBACK (on_request_status_changed), rrd);
}

/*
 * _mcd_dispatcher_take_channels:
 * @dispatcher: the #McdDispatcher.
 * @channels: a #GList of #McdChannel elements.
 * @requested: whether the channels were requested by MC.
 *
 * Dispatch @channels. The #GList @channels will be no longer valid after this
 * function has been called.
 */
void
_mcd_dispatcher_take_channels (McdDispatcher *dispatcher, GList *channels,
                               gboolean requested)
{
    GList *list;
    GStrv possible_handlers;

    if (channels == NULL)
    {
        /* trivial case */
        return;
    }

    /* See if there are any handlers that can take all these channels */
    possible_handlers = mcd_dispatcher_get_possible_handlers (dispatcher,
                                                              channels);

    if (possible_handlers == NULL)
    {
        if (channels->next == NULL)
        {
            /* There's exactly one channel and we can't handle it. It must
             * die. */
            _mcd_channel_undispatchable (channels->data);
            g_list_free (channels);
        }
        else
        {
            /* there are >= 2 channels - split the batch up and try again */
            while (channels != NULL)
            {
                list = channels;
                channels = g_list_remove_link (channels, list);
                _mcd_dispatcher_take_channels (dispatcher, list, requested);
            }
        }
    }
    else
    {
        for (list = channels; list != NULL; list = list->next)
            _mcd_channel_set_status (MCD_CHANNEL (list->data),
                                     MCD_CHANNEL_STATUS_DISPATCHING);

        _mcd_dispatcher_enter_state_machine (dispatcher, channels,
                                             possible_handlers, requested);
    }
}

/**
 * mcd_dispatcher_add_filter:
 * @dispatcher: The #McdDispatcher.
 * @filter: the filter function to be registered.
 * @priority: The priority of the filter.
 * @user_data: user data to be passed to @filter on invocation.
 *
 * Register a filter into the dispatcher chain: @filter will be invoked
 * whenever channels need to be dispatched.
 */
void
mcd_dispatcher_add_filter (McdDispatcher *dispatcher,
                           McdFilterFunc filter,
                           guint priority,
                           gpointer user_data)
{
    McdDispatcherPrivate *priv;

    g_return_if_fail (MCD_IS_DISPATCHER (dispatcher));
    priv = dispatcher->priv;
    priv->filters =
        chain_add_filter (priv->filters, filter, priority, user_data);
}

/**
 * mcd_dispatcher_add_filters:
 * @dispatcher: The #McdDispatcher.
 * @filters: a zero-terminated array of #McdFilter elements.
 *
 * Convenience function to add a batch of filters at once.
 */
void
mcd_dispatcher_add_filters (McdDispatcher *dispatcher,
                            const McdFilter *filters)
{
    const McdFilter *filter;

    g_return_if_fail (filters != NULL);

    for (filter = filters; filter->func != NULL; filter++)
        mcd_dispatcher_add_filter (dispatcher, filter->func,
                                   filter->priority,
                                   filter->user_data);
}

static void
on_redispatched_channel_status_changed (McdChannel *channel,
                                        McdChannelStatus status)
{
    if (status == MCD_CHANNEL_STATUS_DISPATCHED)
    {
        mcd_mission_abort (MCD_MISSION (channel));
    }
}

/*
 * _mcd_dispatcher_reinvoke_handler:
 * @dispatcher: The #McdDispatcher.
 * @channel: a #McdChannel.
 *
 * Re-invoke the channel handler for @channel.
 */
static void
_mcd_dispatcher_reinvoke_handler (McdDispatcher *dispatcher,
                                  McdChannel *channel)
{
    McdDispatcherContext *context;
    GList *list;

    /* Preparing and filling the context */
    context = g_new0 (McdDispatcherContext, 1);
    context->ref_count = 1;
    context->dispatcher = dispatcher;
    context->channels = g_list_prepend (NULL, channel);
    context->account = mcd_channel_get_account (channel);

    list = g_list_append (NULL, channel);
    context->possible_handlers = mcd_dispatcher_get_possible_handlers (
        dispatcher, list);
    g_list_free (list);

    /* We must ref() the channel, because
     * mcd_dispatcher_context_unref() will unref() it */
    g_object_ref (channel);
    mcd_dispatcher_run_handlers (context);
    /* the context will be unreferenced once it leaves the state machine */
}

static McdDispatcherContext *
find_context_from_channel (McdDispatcher *dispatcher, McdChannel *channel)
{
    GList *list;

    g_return_val_if_fail (MCD_IS_CHANNEL (channel), NULL);
    for (list = dispatcher->priv->contexts; list != NULL; list = list->next)
    {
        McdDispatcherContext *context = list->data;

        if (g_list_find (context->channels, channel))
            return context;
    }
    return NULL;
}

void
_mcd_dispatcher_add_channel_request (McdDispatcher *dispatcher,
                                     McdChannel *channel, McdChannel *request)
{
    McdChannelStatus status;

    status = mcd_channel_get_status (channel);

    /* if the channel is already dispatched, just reinvoke the handler; if it
     * is not, @request must mirror the status of @channel */
    if (status == MCD_CHANNEL_STATUS_DISPATCHED)
    {
        DEBUG ("reinvoking handler on channel %p", channel);

        /* copy the object path and the immutable properties from the
         * existing channel */
        _mcd_channel_copy_details (request, channel);

        /* destroy the McdChannel object after it is dispatched */
        g_signal_connect_after
            (request, "status-changed",
             G_CALLBACK (on_redispatched_channel_status_changed), NULL);

        _mcd_dispatcher_reinvoke_handler (dispatcher, request);
    }
    else
    {
        _mcd_channel_set_request_proxy (request, channel);
        if (status == MCD_CHANNEL_STATUS_DISPATCHING)
        {
            McdDispatcherContext *context;

            context = find_context_from_channel (dispatcher, channel);
            DEBUG ("channel %p is in context %p", channel, context);
            if (context->approvers_invoked > 0)
            {
                /* the existing channel is waiting for approval; but since the
                 * same channel has been requested, the approval operation must
                 * terminate */
                if (G_LIKELY (context->operation))
                    mcd_dispatch_operation_handle_with (context->operation,
                                                        NULL, NULL);
            }
            else
                context->skip_approval = TRUE;
        }
        DEBUG ("channel %p is proxying %p", request, channel);
    }
}

static void
get_handled_channels_cb (TpProxy *proxy, const GValue *v_channels,
                         const GError *error, gpointer user_data,
                         GObject *weak_object)
{
    McdClient *client = user_data;

    DEBUG ("called");
    client->got_handled_channels = TRUE;

    if (G_LIKELY (!error))
    {
        if (G_LIKELY (G_VALUE_TYPE (v_channels) == MC_ARRAY_TYPE_OBJECT))
        {
            GPtrArray *a_channels;
            gchar **channels;
            guint i;

            g_return_if_fail (client->handled_channels == NULL);
            a_channels = g_value_get_boxed (v_channels);
            channels = g_malloc ((a_channels->len + 1) * sizeof (char *));

            for (i = 0; i < a_channels->len; i++)
                channels[i] = g_strdup (g_ptr_array_index (a_channels, i));
            channels[i] = NULL;
            client->handled_channels = channels;
        }
        else
            g_warning ("%s: client %s returned wrong type %s", G_STRFUNC,
                       client->name, G_VALUE_TYPE_NAME (v_channels));
    }
    else
        g_warning ("%s: Got error: %s", G_STRFUNC, error->message);

    _mcd_object_ready (proxy, client_ready_quark, error);
}

static void
mcd_client_call_when_got_handled_channels (McdClient *client,
                                           McdReadyCb callback,
                                           gpointer user_data)
{
    DEBUG ("called");
    if (client->got_handled_channels)
        callback (client, NULL, user_data);
    else
    {
        if (!client->getting_handled_channels)
        {
            client->getting_handled_channels = TRUE;
            tp_cli_dbus_properties_call_get
                (client->proxy, -1, MC_IFACE_CLIENT_HANDLER, "HandledChannels",
                 get_handled_channels_cb, client, NULL, NULL);
        }
        _mcd_object_call_on_struct_when_ready
            (client->proxy, client, client_ready_quark, callback, user_data);
    }
}

static void
channel_recover_release_lock (McdChannelRecover *cr)
{
    DEBUG ("called on %p (locks = %d)", cr, cr->handler_locks);
    cr->handler_locks--;
    if (cr->handler_locks == 0)
    {
        /* re-dispatch unhandled channels */
        if (!cr->handled)
        {
            gboolean requested;

            DEBUG ("channel %p is not handled, redispatching", cr->channel);

            requested = mcd_channel_is_requested (cr->channel);
            _mcd_dispatcher_take_channels (cr->dispatcher,
                                           g_list_prepend (NULL, cr->channel),
                                           requested);
        }
        g_object_unref (cr->channel);
        g_slice_free (McdChannelRecover, cr);
    }
}

static void
check_handled_channels (gpointer object, const GError *error,
                        gpointer user_data)
{
    McdClient *client = object;
    McdChannelRecover *cr = user_data;

    DEBUG ("called");
    if (G_LIKELY (!error) && client->handled_channels != NULL)
    {
        const gchar *path;
        gint i;

        path = mcd_channel_get_object_path (cr->channel);
        for (i = 0; client->handled_channels[i] != NULL; i++)
        {
            if (g_strcmp0 (path, client->handled_channels[i]) == 0)
            {
                DEBUG ("Channel %s is handled by %s", path, client->name);
                cr->handled = TRUE;
                _mcd_channel_set_status (cr->channel,
                                         MCD_CHANNEL_STATUS_DISPATCHED);
                break;
            }
        }
    }

    channel_recover_release_lock (cr);
}

void
_mcd_dispatcher_recover_channel (McdDispatcher *dispatcher,
                                 McdChannel *channel)
{
    McdDispatcherPrivate *priv;
    GHashTableIter iter;
    McdClient *client;
    McdChannelRecover *cr;

    /* we must check if the channel is already being handled by some client; to
     * do this, we can examine the active handlers' "HandledChannel" property.
     */
    g_return_if_fail (MCD_IS_DISPATCHER (dispatcher));
    priv = dispatcher->priv;

    cr = g_slice_new0 (McdChannelRecover);
    cr->channel = g_object_ref (channel);
    cr->dispatcher = dispatcher;
    cr->handler_locks = 1;

    g_hash_table_iter_init (&iter, priv->clients);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &client))
    {
        if (!client->proxy ||
            !client->active ||
            !(client->interfaces & MCD_CLIENT_HANDLER))
            continue;

        cr->handler_locks++;
        mcd_client_call_when_got_handled_channels (client,
                                                   check_handled_channels,
                                                   cr);
    }
    /* this pairs with the initial lock set to 1 */
    channel_recover_release_lock (cr);
}

static void
dispatcher_request_channel (McdDispatcher *self,
                            const gchar *account_path,
                            GHashTable *requested_properties,
                            gint64 user_action_time,
                            const gchar *preferred_handler,
                            DBusGMethodInvocation *context,
                            gboolean ensure)
{
    McdAccountManager *am;
    McdAccount *account;
    McdChannel *channel;
    GError *error = NULL;
    const gchar *path;

    g_object_get (self->priv->master,
                  "account-manager", &am,
                  NULL);

    g_assert (am != NULL);

    account = mcd_account_manager_lookup_account_by_path (am,
                                                          account_path);

    if (account == NULL)
    {
        g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "No such account: %s", account_path);
        goto despair;
    }

    if (!tp_dbus_check_valid_bus_name (preferred_handler,
                                       TP_DBUS_NAME_TYPE_WELL_KNOWN,
                                       &error))
    {
        /* The error is TP_DBUS_ERROR_INVALID_BUS_NAME, which has no D-Bus
         * representation; re-map to InvalidArgument. */
        error->domain = TP_ERRORS;
        error->code = TP_ERROR_INVALID_ARGUMENT;
        goto despair;
    }

    if (!g_str_has_prefix (preferred_handler, MC_CLIENT_BUS_NAME_BASE))
    {
        g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Not a Telepathy Client: %s", preferred_handler);
        goto despair;
    }

    channel = _mcd_account_create_request (account, requested_properties,
                                           user_action_time, preferred_handler,
                                           ensure, FALSE, &error);

    if (channel == NULL)
    {
        /* FIXME: ideally this would be emitted as a Failed signal after
         * Proceed is called, but for the particular failure case here (low
         * memory) perhaps we don't want to */
        goto despair;
    }

    path = _mcd_channel_get_request_path (channel);

    g_assert (path != NULL);

    /* This is OK because the signatures of CreateChannel and EnsureChannel
     * are the same */
    mc_svc_channel_dispatcher_return_from_create_channel (context, path);

    _mcd_dispatcher_add_request (self, account, channel);

    /* We've done all we need to with this channel: the ChannelRequests code
     * keeps it alive as long as is necessary */
    g_object_unref (channel);
    return;

despair:
    dbus_g_method_return_error (context, error);
    g_error_free (error);
}

static void
dispatcher_create_channel (McSvcChannelDispatcher *iface,
                           const gchar *account_path,
                           GHashTable *requested_properties,
                           gint64 user_action_time,
                           const gchar *preferred_handler,
                           DBusGMethodInvocation *context)
{
    dispatcher_request_channel (MCD_DISPATCHER (iface),
                                account_path,
                                requested_properties,
                                user_action_time,
                                preferred_handler,
                                context,
                                FALSE);
}

static void
dispatcher_ensure_channel (McSvcChannelDispatcher *iface,
                           const gchar *account_path,
                           GHashTable *requested_properties,
                           gint64 user_action_time,
                           const gchar *preferred_handler,
                           DBusGMethodInvocation *context)
{
    dispatcher_request_channel (MCD_DISPATCHER (iface),
                                account_path,
                                requested_properties,
                                user_action_time,
                                preferred_handler,
                                context,
                                TRUE);
}

static void
dispatcher_iface_init (gpointer g_iface,
                       gpointer iface_data G_GNUC_UNUSED)
{
#define IMPLEMENT(x) mc_svc_channel_dispatcher_implement_##x (\
    g_iface, dispatcher_##x)
    IMPLEMENT (create_channel);
    IMPLEMENT (ensure_channel);
#undef IMPLEMENT
}
