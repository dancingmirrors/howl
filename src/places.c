// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

#include <gio/gunixmounts.h>
#include <string.h>

#if !GLIB_CHECK_VERSION(2, 84, 0)
#define g_unix_mount_entries_get g_unix_mounts_get
#define g_unix_mount_entry_free g_unix_mount_free
#define g_unix_mount_entry_get_mount_path g_unix_mount_get_mount_path
#define g_unix_mount_entry_guess_name g_unix_mount_guess_name
#define g_unix_mount_entry_guess_symbolic_icon g_unix_mount_guess_symbolic_icon
#define g_unix_mount_entry_guess_should_display g_unix_mount_guess_should_display
#endif

typedef struct {
    HowlWindow *win;
    GtkWidget *list;
    GUnixMountMonitor *mount_monitor;
    GFile *current;
} Places;

static void
places_free(gpointer data) {
    Places *p = data;

    if (p->mount_monitor != NULL) {
        g_signal_handlers_disconnect_by_data(p->mount_monitor, p);
        g_object_unref(p->mount_monitor);
    }
    g_clear_object(&p->current);
    g_free(p);
}

static GtkWidget *
add_row(Places *p, GIcon *gicon, const char *icon_name, const char *label,
        GFile *file, gboolean section_start) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *image = gicon != NULL ? gtk_image_new_from_gicon(gicon)
                                     : gtk_image_new_from_icon_name(icon_name);
    GtkWidget *text = gtk_label_new(label);
    char *path = g_file_get_path(file);

    gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), text);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_widget_set_tooltip_text(row, path);
    g_object_set_data_full(G_OBJECT(row), "file", g_object_ref(file), g_object_unref);
    if (section_start) {
        g_object_set_data(G_OBJECT(row), "section-start", GINT_TO_POINTER(1));
    }
    gtk_list_box_append(GTK_LIST_BOX(p->list), row);

    g_free(path);
    return row;
}

static void
add_path(Places *p, const char *icon_name, const char *label, const char *path, gboolean section_start) {
    GFile *file = g_file_new_for_path(path);

    add_row(p, NULL, icon_name, label, file, section_start);
    g_object_unref(file);
}

static void
add_user_dirs(Places *p) {
    static const struct {
        GUserDirectory dir;
        const char *icon;
    } special[] = {
        {G_USER_DIRECTORY_DESKTOP, "user-desktop-symbolic"},
        {G_USER_DIRECTORY_DOCUMENTS, "folder-documents-symbolic"},
        {G_USER_DIRECTORY_DOWNLOAD, "folder-download-symbolic"},
        {G_USER_DIRECTORY_MUSIC, "folder-music-symbolic"},
        {G_USER_DIRECTORY_PICTURES, "folder-pictures-symbolic"},
        {G_USER_DIRECTORY_VIDEOS, "folder-videos-symbolic"},
    };
    const char *home = g_get_home_dir();
    char *trash;

    add_path(p, "user-home-symbolic", "Home", home, FALSE);

    for (gsize i = 0; i < G_N_ELEMENTS(special); i++) {
        const char *path = g_get_user_special_dir(special[i].dir);
        char *label;

        if (path == NULL || g_strcmp0(path, home) == 0 || !g_file_test(path, G_FILE_TEST_IS_DIR)) {
            continue;
        }
        label = g_filename_display_basename(path);
        add_path(p, special[i].icon, label, path, FALSE);
        g_free(label);
    }

    trash = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
    if (g_file_test(trash, G_FILE_TEST_IS_DIR)) {
        add_path(p, "user-trash-symbolic", "Trash", trash, FALSE);
    }
    g_free(trash);
}

static void
add_bookmarks(Places *p) {
    char *path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    char *contents = NULL;
    char **lines;
    gboolean first = TRUE;

    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        lines = g_strsplit(contents, "\n", -1);
        for (char **line = lines; *line != NULL; line++) {
            char *space, *label = NULL;
            GFile *file;

            g_strstrip(*line);
            if (**line == '\0') {
                continue;
            }
            space = strchr(*line, ' ');
            if (space != NULL) {
                *space = '\0';
                label = g_strdup(g_strstrip(space + 1));
            }
            file = g_file_new_for_uri(*line);
            if (g_file_is_native(file)) {
                if (label == NULL || *label == '\0') {
                    g_free(label);
                    label = g_file_get_basename(file);
                }
                add_row(p, NULL, "folder-symbolic", label, file, first);
                first = FALSE;
            }
            g_object_unref(file);
            g_free(label);
        }
        g_strfreev(lines);
    }
    g_free(contents);
    g_free(path);
}

static void
add_mounts(Places *p) {
    GList *mounts, *l;

    add_path(p, "drive-harddisk-symbolic", "File System", "/", TRUE);

    mounts = g_unix_mount_entries_get(NULL);
    for (l = mounts; l != NULL; l = l->next) {
        const char *path = g_unix_mount_entry_get_mount_path(l->data);
        GFile *file;
        GIcon *icon;
        char *name;

        if (!g_unix_mount_entry_guess_should_display(l->data) || g_strcmp0(path, "/") == 0) {
            continue;
        }

        file = g_file_new_for_path(path);
        icon = g_unix_mount_entry_guess_symbolic_icon(l->data);
        name = g_unix_mount_entry_guess_name(l->data);
        add_row(p, icon, NULL, name, file, FALSE);
        g_free(name);
        g_object_unref(icon);
        g_object_unref(file);
    }
    g_list_free_full(mounts, (GDestroyNotify)g_unix_mount_entry_free);
}

static void
select_current(Places *p) {
    GtkWidget *row;

    if (p->current == NULL) {
        return;
    }

    for (row = gtk_widget_get_first_child(p->list); row != NULL; row = gtk_widget_get_next_sibling(row)) {
        GFile *file = g_object_get_data(G_OBJECT(row), "file");

        if (file != NULL && g_file_equal(file, p->current)) {
            gtk_list_box_select_row(GTK_LIST_BOX(p->list), GTK_LIST_BOX_ROW(row));
            return;
        }
    }
    gtk_list_box_unselect_all(GTK_LIST_BOX(p->list));
}

static void
rebuild(Places *p) {
    GtkWidget *row;

    while ((row = gtk_widget_get_first_child(p->list)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(p->list), row);
    }

    add_user_dirs(p);
    add_bookmarks(p);
    add_mounts(p);
    select_current(p);
}

static void
mounts_changed(GUnixMountMonitor *monitor, gpointer data) {
    rebuild(data);
}

static void
row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
    Places *p = data;
    GFile *file = g_object_get_data(G_OBJECT(row), "file");

    if (file != NULL) {
        howl_window_navigate(p->win, file);
    }
}

static GFile *
places_drop_locate(GtkWidget *widget, double x, double y, GtkWidget **highlight, gpointer data) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(widget), (int)y);
    GFile *file = row != NULL ? g_object_get_data(G_OBJECT(row), "file") : NULL;

    if (file == NULL) {
        return NULL;
    }
    *highlight = GTK_WIDGET(row);
    return g_object_ref(file);
}

static void
header_func(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data) {
    if (before != NULL && g_object_get_data(G_OBJECT(row), "section-start") != NULL) {
        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

        gtk_widget_set_margin_top(sep, 4);
        gtk_widget_set_margin_bottom(sep, 4);
        gtk_list_box_row_set_header(row, sep);
    } else {
        gtk_list_box_row_set_header(row, NULL);
    }
}

GtkWidget *
howl_places_new(HowlWindow *win) {
    Places *p = g_new0(Places, 1);
    GtkWidget *scrolled = gtk_scrolled_window_new();

    p->win = win;
    p->list = gtk_list_box_new();
    gtk_widget_add_css_class(p->list, "navigation-sidebar");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p->list), TRUE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(p->list), header_func, NULL, NULL);
    g_signal_connect(p->list, "row-activated", G_CALLBACK(row_activated), p);
    howl_dnd_add_drop_site(win, p->list, places_drop_locate, NULL);

    p->mount_monitor = g_unix_mount_monitor_get();
    g_signal_connect(p->mount_monitor, "mounts-changed", G_CALLBACK(mounts_changed), p);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), p->list);
    gtk_widget_set_size_request(scrolled, 180, -1);
    g_object_set_data_full(G_OBJECT(scrolled), "places", p, places_free);

    rebuild(p);
    return scrolled;
}

void howl_places_set_location(GtkWidget *places, GFile *location) {
    Places *p = g_object_get_data(G_OBJECT(places), "places");

    g_set_object(&p->current, location);
    select_current(p);
}
