// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

#include <sys/stat.h>

#define DROP_CLASS "howl-drop-target"

typedef struct {
    HowlWindow *win;
    GtkWidget *widget;
    GtkDropTarget *target;
    HowlDropLocate locate;
    gpointer locate_data;

    GFile *dest;
    gboolean dest_is_trash;
    gboolean dest_dev_valid;
    dev_t dest_dev;
    GtkWidget *highlight;
    GdkDragAction action;
    double x, y;

    gboolean sources_loaded;
    GList *sources;
    gboolean sources_dev_valid;
    dev_t sources_dev;
} DropSite;

GdkContentProvider *
howl_dnd_content_for_files(GList *gfiles) {
    GSList *list = NULL;
    GValue value = G_VALUE_INIT;
    GdkContentProvider *content;

    for (GList *l = gfiles; l != NULL; l = l->next) {
        list = g_slist_prepend(list, l->data);
    }
    list = g_slist_reverse(list);

    g_value_init(&value, GDK_TYPE_FILE_LIST);
    g_value_take_boxed(&value, gdk_file_list_new_from_list(list));
    content = gdk_content_provider_new_for_value(&value);
    g_value_unset(&value);
    g_slist_free(list);
    return content;
}

GtkWidget *
howl_dnd_icon_new(GdkPaintable *paintable, GIcon *gicon, const char *name, guint count, int size) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *image, *label;

    gtk_widget_add_css_class(box, "howl-drag-icon");

    if (paintable != NULL) {
        image = gtk_image_new_from_paintable(paintable);
    } else if (gicon != NULL) {
        image = gtk_image_new_from_gicon(gicon);
    } else {
        image = gtk_image_new_from_icon_name("text-x-generic");
    }
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), image);

    label = gtk_label_new(name);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(box), label);

    if (count > 1) {
        GtkWidget *overlay = gtk_overlay_new();
        char *text = g_strdup_printf("%u", count);
        GtkWidget *badge = gtk_label_new(text);

        gtk_widget_add_css_class(badge, "howl-drag-badge");
        gtk_widget_set_halign(badge, GTK_ALIGN_END);
        gtk_widget_set_valign(badge, GTK_ALIGN_START);
        gtk_overlay_set_child(GTK_OVERLAY(overlay), box);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), badge);
        g_free(text);
        return overlay;
    }
    return box;
}

static gboolean
is_trash_dir(GFile *dir) {
    char *path = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
    GFile *trash = g_file_new_for_path(path);
    gboolean result = g_file_equal(dir, trash);

    g_object_unref(trash);
    g_free(path);
    return result;
}

static gboolean
file_device(GFile *file, gboolean follow, dev_t *dev) {
    char *path = g_file_get_path(file);
    struct stat st;
    gboolean ok;

    if (path == NULL) {
        return FALSE;
    }
    ok = (follow ? stat(path, &st) : lstat(path, &st)) == 0;
    g_free(path);
    if (ok) {
        *dev = st.st_dev;
    }
    return ok;
}

static GList *
native_files(const GValue *value) {
    GdkFileList *list;
    GSList *files;
    GList *out = NULL;

    if (value == NULL || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        return NULL;
    }
    list = g_value_get_boxed(value);
    files = list != NULL ? gdk_file_list_get_files(list) : NULL;
    for (GSList *l = files; l != NULL; l = l->next) {
        if (g_file_is_native(l->data)) {
            out = g_list_prepend(out, g_object_ref(l->data));
        }
    }
    g_slist_free(files);
    return g_list_reverse(out);
}

static void
site_set_highlight(DropSite *site, GtkWidget *widget) {
    if (site->highlight == widget) {
        return;
    }
    if (site->highlight != NULL) {
        gtk_widget_remove_css_class(site->highlight, DROP_CLASS);
        g_object_remove_weak_pointer(G_OBJECT(site->highlight), (gpointer *)&site->highlight);
    }
    site->highlight = widget;
    if (widget != NULL) {
        gtk_widget_add_css_class(widget, DROP_CLASS);
        g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)&site->highlight);
    }
}

static void
site_reset(DropSite *site) {
    site_set_highlight(site, NULL);
    g_clear_object(&site->dest);
    site->dest_is_trash = FALSE;
    site->dest_dev_valid = FALSE;
    site->action = 0;
    g_list_free_full(site->sources, g_object_unref);
    site->sources = NULL;
    site->sources_loaded = FALSE;
    site->sources_dev_valid = FALSE;
}

static void
site_free(gpointer data) {
    DropSite *site = data;

    site_reset(site);
    g_free(site);
}

static void
site_load_sources(DropSite *site) {
    const GValue *value;

    if (site->sources_loaded) {
        return;
    }
    value = gtk_drop_target_get_value(site->target);
    if (value == NULL) {
        return;
    }
    site->sources_loaded = TRUE;
    site->sources = native_files(value);
    site->sources_dev_valid = site->sources != NULL;
    for (GList *l = site->sources; l != NULL && site->sources_dev_valid; l = l->next) {
        dev_t dev;

        if (!file_device(l->data, FALSE, &dev) || (l != site->sources && dev != site->sources_dev)) {
            site->sources_dev_valid = FALSE;
        } else {
            site->sources_dev = dev;
        }
    }
}

#define SUPPORTED_ACTIONS (GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK)

static GdkDragAction
site_preferred_action(DropSite *site, GdkDrop *drop) {
    GdkDragAction available = gdk_drop_get_actions(drop) & SUPPORTED_ACTIONS;

    if (available == 0 || site->dest == NULL) {
        return 0;
    }
    if (site->sources_loaded && site->sources == NULL) {
        return 0;
    }
    if (site->dest_is_trash) {
        return GDK_ACTION_MOVE;
    }
    if (!site->sources_loaded) {
        return gdk_drag_action_is_unique(available) ? available : GDK_ACTION_COPY;
    }
    for (GList *l = site->sources; l != NULL; l = l->next) {
        if (g_file_equal(l->data, site->dest) || g_file_has_prefix(site->dest, l->data)) {
            return 0;
        }
    }
    if (site->sources_dev_valid && site->dest_dev_valid && site->sources_dev == site->dest_dev) {
        return GDK_ACTION_MOVE;
    }
    return GDK_ACTION_COPY;
}

static GdkDragAction
site_update(DropSite *site, double x, double y) {
    GdkDrop *drop = gtk_drop_target_get_current_drop(site->target);
    GtkWidget *highlight = NULL;
    GFile *dest = site->locate(site->widget, x, y, &highlight, site->locate_data);
    GdkDragAction action = 0;

    site->x = x;
    site->y = y;
    if (dest == NULL) {
        g_clear_object(&site->dest);
    } else if (site->dest == NULL || !g_file_equal(site->dest, dest)) {
        g_set_object(&site->dest, dest);
        site->dest_is_trash = is_trash_dir(dest);
        site->dest_dev_valid = file_device(dest, TRUE, &site->dest_dev);
    }
    g_clear_object(&dest);

    if (drop != NULL) {
        site_load_sources(site);
        action = site_preferred_action(site, drop);
    }
    site_set_highlight(site, action != 0 ? highlight : NULL);
    site->action = action;
    return action;
}

static GdkDragAction
site_enter_cb(GtkDropTarget *target, double x, double y, gpointer data) {
    site_reset(data);
    return site_update(data, x, y);
}

static GdkDragAction
site_motion_cb(GtkDropTarget *target, double x, double y, gpointer data) {
    return site_update(data, x, y);
}

static void
site_leave_cb(GtkDropTarget *target, gpointer data) {
    site_set_highlight(data, NULL);
}

static void
site_value_cb(GObject *object, GParamSpec *pspec, gpointer data) {
    DropSite *site = data;

    if (site->dest != NULL && gtk_drop_target_get_current_drop(site->target) != NULL &&
        gtk_drop_target_get_value(site->target) != NULL) {
        site_update(site, site->x, site->y);
    }
}

static void
site_current_drop_cb(GObject *object, GParamSpec *pspec, gpointer data) {
    DropSite *site = data;

    if (gtk_drop_target_get_current_drop(site->target) == NULL) {
        site_reset(site);
    }
}

static gboolean
site_drop_cb(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data) {
    DropSite *site = data;
    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    GdkDragAction action = site->action;
    GdkDragAction available;
    GFile *dest = site->dest != NULL ? g_object_ref(site->dest) : NULL;
    gboolean trash = site->dest_is_trash;
    GList *files = NULL;

    available = drop != NULL ? gdk_drop_get_actions(drop) & SUPPORTED_ACTIONS : 0;
    if (gdk_drag_action_is_unique(available)) {
        action = available;
    }
    if (gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)) != NULL && dest != NULL && action != 0) {
        files = native_files(value);
    }
    site_reset(site);

    if (files == NULL) {
        g_clear_object(&dest);
        return FALSE;
    }

    if (trash) {
        g_debug("drop: trashing %u files", g_list_length(files));
        howl_ops_trash_gfiles(site->win, files);
    } else if (action == GDK_ACTION_LINK) {
        g_debug("drop: linking %u files in %s", g_list_length(files), g_file_peek_path(dest));
        howl_ops_link(site->win, files, dest);
    } else {
        g_debug("drop: %s %u files to %s", action == GDK_ACTION_MOVE ? "moving" : "copying",
                g_list_length(files), g_file_peek_path(dest));
        howl_ops_transfer(site->win, files, dest, action == GDK_ACTION_MOVE);
    }
    g_list_free_full(files, g_object_unref);
    g_object_unref(dest);
    return TRUE;
}

void howl_dnd_add_drop_site(HowlWindow *win, GtkWidget *widget, HowlDropLocate locate, gpointer user_data) {
    DropSite *site = g_new0(DropSite, 1);

    site->win = win;
    site->widget = widget;
    site->locate = locate;
    site->locate_data = user_data;
    site->target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, SUPPORTED_ACTIONS);
    gtk_drop_target_set_preload(site->target, TRUE);
    g_object_set_data_full(G_OBJECT(site->target), "howl-drop-site", site, site_free);

    g_signal_connect(site->target, "enter", G_CALLBACK(site_enter_cb), site);
    g_signal_connect(site->target, "motion", G_CALLBACK(site_motion_cb), site);
    g_signal_connect(site->target, "leave", G_CALLBACK(site_leave_cb), site);
    g_signal_connect(site->target, "drop", G_CALLBACK(site_drop_cb), site);
    g_signal_connect(site->target, "notify::value", G_CALLBACK(site_value_cb), site);
    g_signal_connect(site->target, "notify::current-drop", G_CALLBACK(site_current_drop_cb), site);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(site->target));
}
