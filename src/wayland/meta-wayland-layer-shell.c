/*
 * Copyright (C) 2024-2026 Tin Švagelj (Caellian) <tin.svagelj@live.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "wayland/meta-wayland-layer-shell.h"

#include "backends/meta-backend-private.h"
#include "backends/meta-logical-monitor-private.h"
#include "backends/meta-monitor-manager-private.h"
#include "backends/meta-monitor-private.h"
#include "compositor/meta-surface-actor-wayland.h"
#include "meta/compositor.h"
#include "wayland/meta-wayland-outputs.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-surface-private.h"
#include "wayland/meta-wayland-versions.h"

#include "wlr-layer-shell-unstable-v1-server-protocol.h"

enum
{
  LAYER_BACKGROUND = 0,
  LAYER_BOTTOM     = 1,
  LAYER_TOP        = 2,
  LAYER_OVERLAY    = 3,
};

typedef struct
{
  uint32_t serial;
  uint32_t width;
  uint32_t height;
} MetaWaylandLayerSurfaceConfigure;

struct _MetaWaylandLayerSurface
{
  MetaWaylandActorSurface parent;

  struct wl_resource *resource;
  MetaWaylandCompositor *compositor;
  MetaWaylandOutput *output;

  uint32_t layer;
  uint32_t anchor;
  int32_t exclusive_zone;
  uint32_t keyboard_interactivity;
  uint32_t exclusive_edge;

  struct
  {
    int32_t top;
    int32_t right;
    int32_t bottom;
    int32_t left;
  } margin;

  uint32_t pending_width;
  uint32_t pending_height;

  GList *configure_list;
  uint32_t configure_serial;
  uint32_t acked_serial;

  gboolean mapped;
  gboolean closed;

  gulong output_destroyed_handler_id;
};

G_DEFINE_TYPE (MetaWaylandLayerSurface,
               meta_wayland_layer_surface,
               META_TYPE_WAYLAND_ACTOR_SURFACE)

static MetaDisplay *
display_from_layer_surface (MetaWaylandLayerSurface *layer_surface)
{
  MetaContext *context =
    meta_wayland_compositor_get_context (layer_surface->compositor);

  return meta_context_get_display (context);
}

static ClutterActor *
get_layer_group (MetaWaylandLayerSurface *layer_surface)
{
  MetaDisplay *display = display_from_layer_surface (layer_surface);
  MetaCompositor *compositor = meta_display_get_compositor (display);

  switch (layer_surface->layer)
    {
    case LAYER_BACKGROUND:
      return meta_compositor_get_layer_background_group (compositor);
    case LAYER_BOTTOM:
      return meta_compositor_get_layer_bottom_group (compositor);
    case LAYER_TOP:
      return meta_compositor_get_layer_top_group (compositor);
    case LAYER_OVERLAY:
      return meta_compositor_get_layer_overlay_group (compositor);
    default:
      g_assert_not_reached ();
    }
}

static MtkRectangle
get_output_rect (MetaWaylandLayerSurface *layer_surface)
{
  MetaContext *context =
    meta_wayland_compositor_get_context (layer_surface->compositor);
  MetaBackend *backend = meta_context_get_backend (context);
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaLogicalMonitor *logical_monitor = NULL;
  MtkRectangle rect = { 0, 0, 0, 0 };

  if (layer_surface->output)
    {
      MetaMonitor *monitor =
        meta_wayland_output_get_monitor (layer_surface->output);

      if (monitor)
        logical_monitor = meta_monitor_get_logical_monitor (monitor);
    }

  if (!logical_monitor)
    logical_monitor =
      meta_monitor_manager_get_primary_logical_monitor (monitor_manager);

  if (logical_monitor)
    rect = meta_logical_monitor_get_layout (logical_monitor);

  return rect;
}

static void
compute_size (MetaWaylandLayerSurface *layer_surface,
              uint32_t                *out_width,
              uint32_t                *out_height)
{
  MtkRectangle output_rect = get_output_rect (layer_surface);
  uint32_t width = layer_surface->pending_width;
  uint32_t height = layer_surface->pending_height;
  uint32_t anchor = layer_surface->anchor;

  if (width == 0)
    {
      gboolean anchored_left =
        (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
      gboolean anchored_right =
        (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;

      if (anchored_left && anchored_right)
        width = output_rect.width
                - layer_surface->margin.left
                - layer_surface->margin.right;
    }

  if (height == 0)
    {
      gboolean anchored_top =
        (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
      gboolean anchored_bottom =
        (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;

      if (anchored_top && anchored_bottom)
        height = output_rect.height
                 - layer_surface->margin.top
                 - layer_surface->margin.bottom;
    }

  *out_width = width;
  *out_height = height;
}

static void
compute_position (MetaWaylandLayerSurface *layer_surface,
                  uint32_t                 width,
                  uint32_t                 height,
                  int                     *out_x,
                  int                     *out_y)
{
  MtkRectangle output_rect = get_output_rect (layer_surface);
  uint32_t anchor = layer_surface->anchor;
  int x, y;

  gboolean anchored_left =
    (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
  gboolean anchored_right =
    (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;
  gboolean anchored_top =
    (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
  gboolean anchored_bottom =
    (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;

  if (anchored_left && anchored_right)
    x = output_rect.x + (output_rect.width - (int) width) / 2
        + layer_surface->margin.left - layer_surface->margin.right;
  else if (anchored_left)
    x = output_rect.x + layer_surface->margin.left;
  else if (anchored_right)
    x = output_rect.x + output_rect.width - (int) width
        - layer_surface->margin.right;
  else
    x = output_rect.x + (output_rect.width - (int) width) / 2;

  if (anchored_top && anchored_bottom)
    y = output_rect.y + (output_rect.height - (int) height) / 2
        + layer_surface->margin.top - layer_surface->margin.bottom;
  else if (anchored_top)
    y = output_rect.y + layer_surface->margin.top;
  else if (anchored_bottom)
    y = output_rect.y + output_rect.height - (int) height
        - layer_surface->margin.bottom;
  else
    y = output_rect.y + (output_rect.height - (int) height) / 2;

  *out_x = x;
  *out_y = y;
}

static void
send_configure (MetaWaylandLayerSurface *layer_surface)
{
  MetaWaylandLayerSurfaceConfigure *configure;
  uint32_t width, height;

  compute_size (layer_surface, &width, &height);

  layer_surface->configure_serial++;

  configure = g_new0 (MetaWaylandLayerSurfaceConfigure, 1);
  configure->serial = layer_surface->configure_serial;
  configure->width = width;
  configure->height = height;

  layer_surface->configure_list =
    g_list_append (layer_surface->configure_list, configure);

  zwlr_layer_surface_v1_send_configure (layer_surface->resource,
                                         configure->serial,
                                         configure->width,
                                         configure->height);
}

static void
unmap_layer_surface (MetaWaylandLayerSurface *layer_surface)
{
  MetaWaylandActorSurface *actor_surface;
  MetaSurfaceActor *surface_actor;
  ClutterActor *actor;
  ClutterActor *parent;

  if (!layer_surface->mapped)
    return;

  layer_surface->mapped = FALSE;

  actor_surface = META_WAYLAND_ACTOR_SURFACE (layer_surface);
  surface_actor = meta_wayland_actor_surface_get_actor (actor_surface);
  if (!surface_actor)
    return;

  actor = CLUTTER_ACTOR (surface_actor);
  parent = clutter_actor_get_parent (actor);
  if (parent)
    clutter_actor_remove_child (parent, actor);
}

static void
map_layer_surface (MetaWaylandLayerSurface *layer_surface)
{
  MetaWaylandActorSurface *actor_surface;
  MetaSurfaceActor *surface_actor;
  ClutterActor *actor;
  ClutterActor *layer_group;
  uint32_t width, height;
  int x, y;

  if (layer_surface->mapped)
    return;

  actor_surface = META_WAYLAND_ACTOR_SURFACE (layer_surface);
  surface_actor = meta_wayland_actor_surface_get_actor (actor_surface);
  if (!surface_actor)
    return;

  actor = CLUTTER_ACTOR (surface_actor);
  layer_group = get_layer_group (layer_surface);

  /* Remove from current parent if any */
  if (clutter_actor_get_parent (actor))
    clutter_actor_remove_child (clutter_actor_get_parent (actor), actor);

  clutter_actor_add_child (layer_group, actor);

  compute_size (layer_surface, &width, &height);
  compute_position (layer_surface, width, height, &x, &y);

  clutter_actor_set_position (actor, x, y);
  clutter_actor_show (actor);

  layer_surface->mapped = TRUE;
}

static void
update_position (MetaWaylandLayerSurface *layer_surface)
{
  MetaWaylandActorSurface *actor_surface;
  MetaSurfaceActor *surface_actor;
  uint32_t width, height;
  int x, y;

  if (!layer_surface->mapped)
    return;

  actor_surface = META_WAYLAND_ACTOR_SURFACE (layer_surface);
  surface_actor = meta_wayland_actor_surface_get_actor (actor_surface);
  if (!surface_actor)
    return;

  compute_size (layer_surface, &width, &height);
  compute_position (layer_surface, width, height, &x, &y);

  clutter_actor_set_position (CLUTTER_ACTOR (surface_actor), x, y);
}

static void
update_layer (MetaWaylandLayerSurface *layer_surface)
{
  MetaWaylandActorSurface *actor_surface;
  MetaSurfaceActor *surface_actor;
  ClutterActor *actor;
  ClutterActor *layer_group;
  ClutterActor *current_parent;

  if (!layer_surface->mapped)
    return;

  actor_surface = META_WAYLAND_ACTOR_SURFACE (layer_surface);
  surface_actor = meta_wayland_actor_surface_get_actor (actor_surface);
  if (!surface_actor)
    return;

  actor = CLUTTER_ACTOR (surface_actor);
  layer_group = get_layer_group (layer_surface);
  current_parent = clutter_actor_get_parent (actor);

  if (current_parent != layer_group)
    {
      g_object_ref (actor);
      if (current_parent)
        clutter_actor_remove_child (current_parent, actor);
      clutter_actor_add_child (layer_group, actor);
      g_object_unref (actor);
    }
}

/* --- zwlr_layer_surface_v1 interface --- */

static void
layer_surface_set_size (struct wl_client   *client,
                        struct wl_resource *resource,
                        uint32_t            width,
                        uint32_t            height)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);

  layer_surface->pending_width = width;
  layer_surface->pending_height = height;
}

static void
layer_surface_set_anchor (struct wl_client   *client,
                          struct wl_resource *resource,
                          uint32_t            anchor)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);

  if (anchor > (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
    {
      wl_resource_post_error (resource,
                              ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
                              "Invalid anchor %u", anchor);
      return;
    }

  layer_surface->anchor = anchor;
}

static void
layer_surface_set_exclusive_zone (struct wl_client   *client,
                                  struct wl_resource *resource,
                                  int32_t             zone)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);

  layer_surface->exclusive_zone = zone;
}

static void
layer_surface_set_margin (struct wl_client   *client,
                          struct wl_resource *resource,
                          int32_t             top,
                          int32_t             right,
                          int32_t             bottom,
                          int32_t             left)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);

  layer_surface->margin.top = top;
  layer_surface->margin.right = right;
  layer_surface->margin.bottom = bottom;
  layer_surface->margin.left = left;
}

static void
layer_surface_set_keyboard_interactivity (struct wl_client   *client,
                                          struct wl_resource *resource,
                                          uint32_t            keyboard_interactivity)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);

  if (keyboard_interactivity >
      ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)
    {
      wl_resource_post_error (
        resource,
        ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
        "Invalid keyboard interactivity %u", keyboard_interactivity);
      return;
    }

  layer_surface->keyboard_interactivity = keyboard_interactivity;
}

static void
layer_surface_get_popup (struct wl_client   *client,
                         struct wl_resource *resource,
                         struct wl_resource *popup_resource)
{
  /* TODO: proper popup parenting support */
}

static void
layer_surface_ack_configure (struct wl_client   *client,
                             struct wl_resource *resource,
                             uint32_t            serial)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);
  GList *l;

  layer_surface->acked_serial = serial;

  /* Remove all configure events up to and including this serial */
  while (layer_surface->configure_list)
    {
      MetaWaylandLayerSurfaceConfigure *configure =
        layer_surface->configure_list->data;
      gboolean is_match = (configure->serial == serial);

      layer_surface->configure_list =
        g_list_delete_link (layer_surface->configure_list,
                            layer_surface->configure_list);
      g_free (configure);

      if (is_match)
        break;
    }
}

static void
layer_surface_destroy (struct wl_client   *client,
                       struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
layer_surface_set_layer (struct wl_client   *client,
                         struct wl_resource *resource,
                         uint32_t            layer)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);

  if (layer > LAYER_OVERLAY)
    {
      wl_resource_post_error (
        resource,
        ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
        "Invalid layer %u", layer);
      return;
    }

  layer_surface->layer = layer;
  update_layer (layer_surface);
  update_position (layer_surface);
}

static void
layer_surface_set_exclusive_edge (struct wl_client   *client,
                                  struct wl_resource *resource,
                                  uint32_t            edge)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);

  layer_surface->exclusive_edge = edge;
}

static const struct zwlr_layer_surface_v1_interface
meta_wayland_layer_surface_interface =
{
  .set_size = layer_surface_set_size,
  .set_anchor = layer_surface_set_anchor,
  .set_exclusive_zone = layer_surface_set_exclusive_zone,
  .set_margin = layer_surface_set_margin,
  .set_keyboard_interactivity = layer_surface_set_keyboard_interactivity,
  .get_popup = layer_surface_get_popup,
  .ack_configure = layer_surface_ack_configure,
  .destroy = layer_surface_destroy,
  .set_layer = layer_surface_set_layer,
  .set_exclusive_edge = layer_surface_set_exclusive_edge,
};

/* --- MetaWaylandActorSurface role vtable --- */

static void
layer_surface_role_assigned (MetaWaylandSurfaceRole *surface_role)
{
  MetaWaylandSurfaceRoleClass *surface_role_class =
    META_WAYLAND_SURFACE_ROLE_CLASS (meta_wayland_layer_surface_parent_class);

  surface_role_class->assigned (surface_role);
}

static void
layer_surface_role_commit_state (MetaWaylandSurfaceRole  *surface_role,
                                 MetaWaylandTransaction  *transaction,
                                 MetaWaylandSurfaceState *pending)
{
  MetaWaylandLayerSurface *layer_surface =
    META_WAYLAND_LAYER_SURFACE (surface_role);

  if (layer_surface->closed)
    return;

  /* Initial commit (no buffer): send first configure */
  if (!layer_surface->configure_list && !layer_surface->mapped)
    {
      send_configure (layer_surface);
      return;
    }
}

static void
layer_surface_role_apply_state (MetaWaylandSurfaceRole  *surface_role,
                                MetaWaylandSurfaceState *pending)
{
  MetaWaylandSurfaceRoleClass *surface_role_class =
    META_WAYLAND_SURFACE_ROLE_CLASS (meta_wayland_layer_surface_parent_class);
  MetaWaylandLayerSurface *layer_surface =
    META_WAYLAND_LAYER_SURFACE (surface_role);
  MetaWaylandSurface *surface =
    meta_wayland_surface_role_get_surface (surface_role);

  if (layer_surface->closed)
    return;

  surface_role_class->apply_state (surface_role, pending);

  /* Null buffer -> unmap */
  if (!meta_wayland_surface_get_buffer (surface))
    {
      unmap_layer_surface (layer_surface);
      return;
    }

  /* Map if we have a buffer and an acked configure */
  if (!layer_surface->mapped && layer_surface->acked_serial > 0)
    map_layer_surface (layer_surface);

  if (layer_surface->mapped)
    {
      update_layer (layer_surface);
      update_position (layer_surface);
    }
}

static int
layer_surface_get_geometry_scale (MetaWaylandActorSurface *actor_surface)
{
  MetaWaylandSurfaceRole *role = META_WAYLAND_SURFACE_ROLE (actor_surface);
  MetaWaylandSurface *surface = meta_wayland_surface_role_get_surface (role);
  MetaContext *context =
    meta_wayland_compositor_get_context (surface->compositor);
  MetaBackend *backend = meta_context_get_backend (context);

  if (!meta_backend_is_stage_views_scaled (backend))
    {
      MetaWaylandLayerSurface *layer_surface =
        META_WAYLAND_LAYER_SURFACE (actor_surface);
      MetaMonitorManager *monitor_manager =
        meta_backend_get_monitor_manager (backend);
      MetaLogicalMonitor *logical_monitor = NULL;

      if (layer_surface->output)
        {
          MetaMonitor *monitor =
            meta_wayland_output_get_monitor (layer_surface->output);
          if (monitor)
            logical_monitor = meta_monitor_get_logical_monitor (monitor);
        }

      if (!logical_monitor)
        logical_monitor =
          meta_monitor_manager_get_primary_logical_monitor (monitor_manager);

      if (logical_monitor)
        return (int) roundf (meta_logical_monitor_get_scale (logical_monitor));
    }

  return 1;
}

static void
layer_surface_sync_actor_state (MetaWaylandActorSurface *actor_surface)
{
  MetaWaylandActorSurfaceClass *actor_surface_class =
    META_WAYLAND_ACTOR_SURFACE_CLASS (meta_wayland_layer_surface_parent_class);

  actor_surface_class->sync_actor_state (actor_surface);
}

/* --- resource destructor --- */

static void
layer_surface_resource_destroy (struct wl_resource *resource)
{
  MetaWaylandLayerSurface *layer_surface =
    wl_resource_get_user_data (resource);

  if (!layer_surface)
    return;

  unmap_layer_surface (layer_surface);

  g_list_free_full (layer_surface->configure_list,
                    (GDestroyNotify) g_free);
  layer_surface->configure_list = NULL;

  layer_surface->resource = NULL;
}

/* --- output destroyed --- */

static void
on_output_destroyed (MetaWaylandOutput       *output,
                     MetaWaylandLayerSurface *layer_surface)
{
  layer_surface->output = NULL;
  layer_surface->output_destroyed_handler_id = 0;

  if (layer_surface->resource && !layer_surface->closed)
    {
      layer_surface->closed = TRUE;
      zwlr_layer_surface_v1_send_closed (layer_surface->resource);
    }
}

/* Signal name for MetaWaylandOutput destruction */
#define OUTPUT_DESTROYED_SIGNAL "output-destroyed"

/* --- GObject boilerplate --- */

static void
meta_wayland_layer_surface_dispose (GObject *object)
{
  MetaWaylandLayerSurface *layer_surface =
    META_WAYLAND_LAYER_SURFACE (object);

  unmap_layer_surface (layer_surface);

  if (layer_surface->output && layer_surface->output_destroyed_handler_id)
    {
      g_signal_handler_disconnect (layer_surface->output,
                                   layer_surface->output_destroyed_handler_id);
      layer_surface->output_destroyed_handler_id = 0;
    }

  g_list_free_full (layer_surface->configure_list,
                    (GDestroyNotify) g_free);
  layer_surface->configure_list = NULL;

  G_OBJECT_CLASS (meta_wayland_layer_surface_parent_class)->dispose (object);
}

static void
meta_wayland_layer_surface_init (MetaWaylandLayerSurface *layer_surface)
{
}

static void
meta_wayland_layer_surface_class_init (MetaWaylandLayerSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaWaylandSurfaceRoleClass *surface_role_class =
    META_WAYLAND_SURFACE_ROLE_CLASS (klass);
  MetaWaylandActorSurfaceClass *actor_surface_class =
    META_WAYLAND_ACTOR_SURFACE_CLASS (klass);

  object_class->dispose = meta_wayland_layer_surface_dispose;

  surface_role_class->assigned = layer_surface_role_assigned;
  surface_role_class->commit_state = layer_surface_role_commit_state;
  surface_role_class->apply_state = layer_surface_role_apply_state;

  actor_surface_class->get_geometry_scale = layer_surface_get_geometry_scale;
  actor_surface_class->sync_actor_state = layer_surface_sync_actor_state;
}

/* --- zwlr_layer_shell_v1 interface --- */

static void
layer_shell_get_layer_surface (struct wl_client   *client,
                               struct wl_resource *shell_resource,
                               uint32_t            id,
                               struct wl_resource *surface_resource,
                               struct wl_resource *output_resource,
                               uint32_t            layer,
                               const char         *namespace)
{
  MetaWaylandCompositor *compositor =
    wl_resource_get_user_data (shell_resource);
  MetaWaylandSurface *surface =
    wl_resource_get_user_data (surface_resource);
  MetaWaylandLayerSurface *layer_surface;
  struct wl_resource *layer_surface_resource;
  MetaWaylandOutput *output = NULL;

  if (layer > LAYER_OVERLAY)
    {
      wl_resource_post_error (shell_resource,
                              ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                              "Invalid layer %u", layer);
      return;
    }

  if (meta_wayland_surface_get_buffer (surface))
    {
      wl_resource_post_error (
        shell_resource,
        ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
        "wl_surface@%d already has a buffer attached",
        wl_resource_get_id (surface_resource));
      return;
    }

  if (!meta_wayland_surface_assign_role (surface,
                                         META_TYPE_WAYLAND_LAYER_SURFACE,
                                         NULL))
    {
      wl_resource_post_error (shell_resource,
                              ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                              "wl_surface@%d already has a different role",
                              wl_resource_get_id (surface_resource));
      return;
    }

  layer_surface_resource =
    wl_resource_create (client,
                        &zwlr_layer_surface_v1_interface,
                        wl_resource_get_version (shell_resource),
                        id);

  layer_surface = META_WAYLAND_LAYER_SURFACE (surface->role);
  layer_surface->resource = layer_surface_resource;
  layer_surface->compositor = compositor;
  layer_surface->layer = layer;

  if (output_resource)
    {
      output = wl_resource_get_user_data (output_resource);
      layer_surface->output = output;

      if (output)
        {
          layer_surface->output_destroyed_handler_id =
            g_signal_connect (output, OUTPUT_DESTROYED_SIGNAL,
                              G_CALLBACK (on_output_destroyed),
                              layer_surface);
        }
    }

  wl_resource_set_implementation (layer_surface_resource,
                                  &meta_wayland_layer_surface_interface,
                                  layer_surface,
                                  layer_surface_resource_destroy);
}

static void
layer_shell_destroy (struct wl_client   *client,
                     struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct zwlr_layer_shell_v1_interface
meta_wayland_layer_shell_interface =
{
  .get_layer_surface = layer_shell_get_layer_surface,
  .destroy = layer_shell_destroy,
};

static void
layer_shell_bind (struct wl_client *client,
                  void             *data,
                  uint32_t          version,
                  uint32_t          id)
{
  MetaWaylandCompositor *compositor = data;
  struct wl_resource *resource;

  resource = wl_resource_create (client,
                                 &zwlr_layer_shell_v1_interface,
                                 version, id);
  wl_resource_set_implementation (resource,
                                  &meta_wayland_layer_shell_interface,
                                  compositor, NULL);
}

void
meta_wayland_layer_shell_init (MetaWaylandCompositor *compositor)
{
  if (wl_global_create (compositor->wayland_display,
                        &zwlr_layer_shell_v1_interface,
                        META_ZWLR_LAYER_SHELL_V1_VERSION,
                        compositor, layer_shell_bind) == NULL)
    g_error ("Failed to register a global wlr-layer-shell object");
}
