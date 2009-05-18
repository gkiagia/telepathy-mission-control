/* vi: set et sw=4 ts=4 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of mission-control
 *
 * Copyright (C) 2007-2009 Nokia Corporation.
 * Copyright (C) 2009 Collabora Ltd.
 *
 * Contact: Alberto Mardegan  <alberto.mardegan@nokia.com>
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

#ifndef MCD_MISC_H
#define MCD_MISC_H

#include <glib.h>
#include <glib-object.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/gtypes.h>

G_BEGIN_DECLS

GHashTable *_mcd_deepcopy_asv (GHashTable *asv);

gchar *_mcd_build_error_string (const GError *error);

typedef void (*McdReadyCb) (gpointer object, const GError *error,
                            gpointer user_data);
void _mcd_object_call_when_ready (gpointer object, GQuark quark,
                                  McdReadyCb callback, gpointer user_data);
void _mcd_object_call_on_struct_when_ready (gpointer object, gpointer strukt,
                                            GQuark quark, McdReadyCb callback,
                                            gpointer user_data);
void _mcd_object_ready (gpointer object, GQuark quark, const GError *error);

/* not exported */
#define MC_ARRAY_TYPE_OBJECT (_mcd_type_dbus_ao ())
G_GNUC_INTERNAL
GType _mcd_type_dbus_ao (void);

G_GNUC_INTERNAL
gboolean _mcd_file_set_contents (const gchar *filename, const gchar *contents,
                                 gssize length, GError **error);
G_GNUC_INTERNAL
void _mc_ext_register_dbus_glib_marshallers (void);

gboolean _mcd_dbus_daemon_request_name (TpDBusDaemon *self,
                                        const gchar *well_known_name,
                                        gboolean idempotent,
                                        GError **error);

G_END_DECLS
#endif /* MCD_MISC_H */
