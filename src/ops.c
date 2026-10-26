// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

#include <string.h>

#define MIME_GNOME_COPIED_FILES "x-special/gnome-copied-files"
#define MIME_URI_LIST "text/uri-list"

static GHashTable *cut_paths;
static gboolean clipboard_watched;

GList *
howl_files_to_gfiles(GList *files) {
    GList *out = NULL;

    for (GList *l = files; l != NULL; l = l->next) {
        out = g_list_prepend(out, g_object_ref(howl_file_get_file(l->data)));
    }
    return g_list_reverse(out);
}

static GList *
copy_gfiles(GList *gfiles) {
    GList *out = NULL;

    for (GList *l = gfiles; l != NULL; l = l->next) {
        out = g_list_prepend(out, g_object_ref(l->data));
    }
    return g_list_reverse(out);
}

static char *
display_name(GFile *file) {
    char *name = g_file_get_basename(file);
    char *display = g_filename_display_name(name);

    g_free(name);
    return display;
}

static HowlWindow *
window_from_gtk(GtkWindow *window) {
    if (window == NULL) {
        return NULL;
    }
    return g_object_get_data(G_OBJECT(window), "howl-window");
}

static void
report(GtkWindow *window, const char *message, const char *detail) {
    HowlWindow *win = window_from_gtk(window);

    if (win != NULL) {
        howl_window_show_error(win, message, detail);
    } else {
        g_printerr("howl: %s: %s\n", message, detail != NULL ? detail : "");
    }
}

typedef struct {
    GtkWindow *window;
} LaunchData;

static void
launch_data_free(LaunchData *d) {
    if (d->window != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(d->window), (gpointer *)&d->window);
    }
    g_free(d);
}

static void
launched_cb(GObject *source, GAsyncResult *result, gpointer data) {
    LaunchData *d = data;
    GError *error = NULL;

    if (!g_app_info_launch_uris_finish(G_APP_INFO(source), result, &error)) {
        report(d->window, "Could not open", error->message);
        g_error_free(error);
    }
    launch_data_free(d);
}

static void
launch(GtkWindow *window, GAppInfo *app, GList *gfiles) {
    GdkAppLaunchContext *context;
    LaunchData *d = g_new0(LaunchData, 1);
    GList *uris = NULL;

    d->window = window;
    g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&d->window);

    for (GList *l = gfiles; l != NULL; l = l->next) {
        uris = g_list_prepend(uris, g_file_get_uri(l->data));
    }
    uris = g_list_reverse(uris);

    context = gdk_display_get_app_launch_context(gtk_widget_get_display(GTK_WIDGET(window)));
    g_app_info_launch_uris_async(app, uris, G_APP_LAUNCH_CONTEXT(context), NULL, launched_cb, d);

    g_object_unref(context);
    g_list_free_full(uris, g_free);
}

static gboolean
is_binary_executable(HowlFile *file) {
    const char *ct = howl_file_get_content_type(file);

    if (!howl_file_is_executable(file)) {
        return FALSE;
    }
    return g_content_type_is_a(ct, "application/x-executable") ||
        g_content_type_is_a(ct, "application/x-sharedlib") ||
        g_str_equal(ct, "application/x-pie-executable");
}

void howl_ops_run(HowlWindow *win, HowlFile *file) {
    char *path = g_file_get_path(howl_file_get_file(file));
    char *dir;
    char *argv[2];
    GError *error = NULL;

    if (path == NULL) {
        return;
    }
    dir = g_path_get_dirname(path);
    argv[0] = path;
    argv[1] = NULL;
    if (!g_spawn_async(dir, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error)) {
        howl_window_show_error(win, "Could not run program", error->message);
        g_error_free(error);
    }
    g_free(dir);
    g_free(path);
}

void howl_ops_open(HowlWindow *win, GList *files) {
    GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter iter;
    gpointer key, value;
    gboolean navigated = FALSE;

    for (GList *l = files; l != NULL; l = l->next) {
        HowlFile *file = l->data;
        const char *ct;

        if (howl_file_is_directory(file)) {
            if (!navigated) {
                howl_window_navigate(win, howl_file_get_file(file));
            } else {
                howl_window_new(howl_file_get_file(file), NULL);
            }
            navigated = TRUE;
            continue;
        }
        if (is_binary_executable(file)) {
            howl_ops_run(win, file);
            continue;
        }
        ct = howl_file_get_content_type(file);
        g_hash_table_insert(groups, g_strdup(ct), g_list_append(g_hash_table_lookup(groups, ct), file));
    }

    g_hash_table_iter_init(&iter, groups);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GAppInfo *app = g_app_info_get_default_for_type(key, FALSE);
        GList *group = value;

        if (app == NULL) {
            howl_ops_open_with_dialog(win, group);
        } else {
            GList *gfiles = howl_files_to_gfiles(group);
            launch(howl_window_get_gtk(win), app, gfiles);
            g_list_free_full(gfiles, g_object_unref);
            g_object_unref(app);
        }
        g_list_free(group);
    }
    g_hash_table_unref(groups);
}

void howl_ops_open_with_id(HowlWindow *win, GList *files, const char *desktop_id) {
    GDesktopAppInfo *app = g_desktop_app_info_new(desktop_id);
    GList *gfiles;

    if (app == NULL) {
        howl_window_show_error(win, "Application not found", desktop_id);
        return;
    }
    gfiles = howl_files_to_gfiles(files);
    launch(howl_window_get_gtk(win), G_APP_INFO(app), gfiles);
    g_list_free_full(gfiles, g_object_unref);
    g_object_unref(app);
}

GMenuModel *
howl_ops_open_with_menu(GList *files) {
    GMenu *menu = g_menu_new();
    GMenu *apps = g_menu_new();
    GMenu *other = g_menu_new();

    if (files != NULL) {
        GList *list = g_app_info_get_all_for_type(howl_file_get_content_type(files->data));
        GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

        for (GList *l = list; l != NULL; l = l->next) {
            const char *id = g_app_info_get_id(l->data);
            GMenuItem *item;

            if (id == NULL || g_hash_table_contains(seen, id)) {
                continue;
            }
            g_hash_table_add(seen, (gpointer)id);
            item = g_menu_item_new(g_app_info_get_display_name(l->data), NULL);
            g_menu_item_set_action_and_target(item, "win.open-with", "s", id);
            g_menu_append_item(apps, item);
            g_object_unref(item);
        }
        g_hash_table_unref(seen);
        g_list_free_full(list, g_object_unref);
    }

    g_menu_append_section(menu, NULL, G_MENU_MODEL(apps));
    g_menu_append(other, "Other Application\342\200\246", "win.open-with-dialog");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(other));
    g_object_unref(apps);
    g_object_unref(other);
    return G_MENU_MODEL(menu);
}

typedef struct {
    GtkWindow *parent;
    GtkWidget *dialog;
    GtkWidget *check;
    GtkSingleSelection *selection;
    GList *gfiles;
    char *content_type;
} OpenWith;

static void
open_with_free(gpointer data) {
    OpenWith *ow = data;

    if (ow->parent != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(ow->parent), (gpointer *)&ow->parent);
    }
    g_list_free_full(ow->gfiles, g_object_unref);
    g_clear_object(&ow->selection);
    g_free(ow->content_type);
    g_free(ow);
}

static void
open_with_accept(OpenWith *ow) {
    GAppInfo *app = gtk_single_selection_get_selected_item(ow->selection);

    if (app == NULL || ow->parent == NULL) {
        return;
    }

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(ow->check))) {
        GError *error = NULL;

        if (!g_app_info_set_as_default_for_type(app, ow->content_type, &error)) {
            report(ow->parent, "Could not set default application", error->message);
            g_error_free(error);
        }
    }
    launch(ow->parent, app, ow->gfiles);
    gtk_window_destroy(GTK_WINDOW(ow->dialog));
}

static void
open_with_button_cb(GtkButton *button, gpointer data) {
    open_with_accept(data);
}

static void
open_with_activate_cb(GtkListView *view, guint position, gpointer data) {
    open_with_accept(data);
}

static void
open_with_cancel_cb(GtkButton *button, gpointer data) {
    OpenWith *ow = data;

    gtk_window_destroy(GTK_WINDOW(ow->dialog));
}

static gboolean
open_with_key_cb(GtkEventControllerKey *controller, guint keyval, guint keycode,
                 GdkModifierType state, gpointer data) {
    OpenWith *ow = data;

    if (keyval == GDK_KEY_Escape) {
        gtk_window_destroy(GTK_WINDOW(ow->dialog));
        return TRUE;
    }
    return FALSE;
}

static void
app_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *image = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);

    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(item, box);
}

static void
app_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    GtkWidget *box = gtk_list_item_get_child(item);
    GtkWidget *image = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_last_child(box);
    GAppInfo *app = gtk_list_item_get_item(item);
    GIcon *icon = g_app_info_get_icon(app);

    if (icon != NULL) {
        gtk_image_set_from_gicon(GTK_IMAGE(image), icon);
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(image), "application-x-executable");
    }
    gtk_label_set_text(GTK_LABEL(label), g_app_info_get_display_name(app));
}

static int
compare_app_names(gconstpointer a, gconstpointer b) {
    return g_utf8_collate(g_app_info_get_display_name(G_APP_INFO(a)),
                          g_app_info_get_display_name(G_APP_INFO(b)));
}

void howl_ops_open_with_dialog(HowlWindow *win, GList *files) {
    OpenWith *ow;
    GtkWindow *parent = howl_window_get_gtk(win);
    GtkWidget *header, *cancel, *open, *box, *label, *scrolled, *view;
    GtkListItemFactory *factory;
    GListStore *store;
    GHashTable *seen;
    GList *recommended, *all, *l;
    GtkEventController *keys;
    char *description, *text;
    guint n = g_list_length(files);

    if (files == NULL) {
        return;
    }

    ow = g_new0(OpenWith, 1);
    ow->parent = parent;
    g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&ow->parent);
    ow->gfiles = howl_files_to_gfiles(files);
    ow->content_type = g_strdup(howl_file_get_content_type(files->data));

    ow->dialog = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(ow->dialog), parent);
    gtk_window_set_modal(GTK_WINDOW(ow->dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(ow->dialog), TRUE);
    gtk_window_set_title(GTK_WINDOW(ow->dialog), "Open With");
    gtk_window_set_default_size(GTK_WINDOW(ow->dialog), 440, 520);
    g_object_set_data_full(G_OBJECT(ow->dialog), "open-with", ow, open_with_free);

    header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    cancel = gtk_button_new_with_label("Cancel");
    open = gtk_button_new_with_label("Open");
    gtk_widget_add_css_class(open, "suggested-action");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), cancel);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), open);
    gtk_window_set_titlebar(GTK_WINDOW(ow->dialog), header);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    if (n == 1) {
        text = g_strdup_printf("Choose an application to open \342\200\234%s\342\200\235.",
                               howl_file_get_display_name(files->data));
    } else {
        text = g_strdup_printf("Choose an application to open %u files.", n);
    }
    label = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_append(GTK_BOX(box), label);
    g_free(text);

    store = g_list_store_new(G_TYPE_DESKTOP_APP_INFO);
    seen = g_hash_table_new(g_str_hash, g_str_equal);
    recommended = g_app_info_get_all_for_type(ow->content_type);
    for (l = recommended; l != NULL; l = l->next) {
        const char *id = g_app_info_get_id(l->data);

        if (id == NULL || g_hash_table_contains(seen, id) || !G_IS_DESKTOP_APP_INFO(l->data)) {
            continue;
        }
        g_hash_table_add(seen, (gpointer)id);
        g_list_store_append(store, l->data);
    }
    all = g_list_sort(g_app_info_get_all(), compare_app_names);
    for (l = all; l != NULL; l = l->next) {
        const char *id = g_app_info_get_id(l->data);

        if (id == NULL || g_hash_table_contains(seen, id) || !G_IS_DESKTOP_APP_INFO(l->data) ||
            !g_app_info_should_show(l->data)) {
            continue;
        }
        g_hash_table_add(seen, (gpointer)id);
        g_list_store_append(store, l->data);
    }

    factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(app_item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(app_item_bind), NULL);
    ow->selection = gtk_single_selection_new(G_LIST_MODEL(store));
    view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(ow->selection)), factory);
    g_signal_connect(view, "activate", G_CALLBACK(open_with_activate_cb), ow);

    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);

    description = g_content_type_get_description(ow->content_type);
    text = g_strdup_printf("Always use for \342\200\234%s\342\200\235 files", description);
    ow->check = gtk_check_button_new_with_label(text);
    gtk_box_append(GTK_BOX(box), ow->check);
    g_free(text);
    g_free(description);

    gtk_window_set_child(GTK_WINDOW(ow->dialog), box);

    g_signal_connect(cancel, "clicked", G_CALLBACK(open_with_cancel_cb), ow);
    g_signal_connect(open, "clicked", G_CALLBACK(open_with_button_cb), ow);
    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(open_with_key_cb), ow);
    gtk_widget_add_controller(ow->dialog, keys);

    gtk_window_present(GTK_WINDOW(ow->dialog));

    g_list_free_full(recommended, g_object_unref);
    g_list_free_full(all, g_object_unref);
    g_hash_table_unref(seen);
}

gboolean
howl_ops_have_terminal(void) {
    const char *term = g_getenv("TERMINAL");

    return term != NULL && *term != '\0';
}

void howl_ops_open_terminal(HowlWindow *win, GFile *dir) {
    const char *term = g_getenv("TERMINAL");
    char **argv = NULL;
    char *path;
    GError *error = NULL;

    if (term == NULL || !g_shell_parse_argv(term, NULL, &argv, &error)) {
        howl_window_show_error(win, "Could not open terminal",
                               error != NULL ? error->message : "Set $TERMINAL to your terminal emulator.");
        g_clear_error(&error);
        return;
    }

    path = g_file_get_path(dir);
    if (!g_spawn_async(path, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        howl_window_show_error(win, "Could not open terminal", error->message);
        g_error_free(error);
    }
    g_free(path);
    g_strfreev(argv);
}

typedef struct {
    GtkWindow *window;
    GFile *parent;
    GFile *target;
    char *current_name;
    GtkWidget *popover;
    GtkWidget *entry;
    GtkWidget *error;
    GtkWidget *button;
    guint select_source;
} NameDialog;

static void
name_dialog_free(gpointer data) {
    NameDialog *d = data;

    if (d->window != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(d->window), (gpointer *)&d->window);
    }
    if (d->select_source != 0) {
        g_source_remove(d->select_source);
    }
    g_clear_object(&d->parent);
    g_clear_object(&d->target);
    g_free(d->current_name);
    g_free(d);
}

static void
name_dialog_validate(GtkEditable *editable, gpointer data) {
    NameDialog *d = data;
    const char *text = gtk_editable_get_text(editable);
    const char *problem = NULL;
    gboolean ok = TRUE;

    if (*text == '\0') {
        ok = FALSE;
    } else if (strchr(text, '/') != NULL) {
        problem = "Names cannot contain \342\200\234/\342\200\235.";
    } else if (g_str_equal(text, ".") || g_str_equal(text, "..")) {
        problem = "That name is not allowed.";
    } else if (d->current_name != NULL && g_str_equal(text, d->current_name)) {
        ok = FALSE;
    } else {
        GFile *child = g_file_get_child_for_display_name(d->parent, text, NULL);

        if (child != NULL) {
            if (g_file_query_exists(child, NULL)) {
                problem = g_file_query_file_type(child, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY
                    ? "A folder with that name already exists."
                    : "A file with that name already exists.";
            }
            g_object_unref(child);
        } else {
            problem = "That name cannot be used here.";
        }
    }

    gtk_label_set_text(GTK_LABEL(d->error), problem != NULL ? problem : "");
    gtk_widget_set_visible(d->error, problem != NULL);
    gtk_widget_set_sensitive(d->button, ok && problem == NULL);
}

static void
name_dialog_accept(NameDialog *d) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(d->entry));
    HowlWindow *win = window_from_gtk(d->window);
    GError *error = NULL;
    GFile *result = NULL;

    if (!gtk_widget_get_sensitive(d->button) || win == NULL) {
        return;
    }

    if (d->target != NULL) {
        result = g_file_set_display_name(d->target, text, NULL, &error);
    } else {
        GFile *child = g_file_get_child_for_display_name(d->parent, text, &error);

        if (child != NULL) {
            if (g_file_make_directory(child, NULL, &error)) {
                result = child;
            } else {
                g_object_unref(child);
            }
        }
    }

    if (result != NULL) {
        char *name = g_file_get_basename(result);
        howl_window_select_name(win, name);
        g_free(name);
        g_object_unref(result);
    } else {
        howl_window_show_error(win, d->target != NULL ? "Could not rename" : "Could not create folder",
                               error->message);
        g_error_free(error);
    }
    gtk_popover_popdown(GTK_POPOVER(d->popover));
}

static void
name_dialog_activate_cb(GtkEntry *entry, gpointer data) {
    name_dialog_accept(data);
}

static void
name_dialog_button_cb(GtkButton *button, gpointer data) {
    name_dialog_accept(data);
}

static gboolean
unparent_idle(gpointer data) {
    gtk_widget_unparent(data);
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}

#define OPEN_NAME_DIALOG "howl-name-dialog"

static void
name_dialog_closed_cb(GtkPopover *popover, gpointer data) {
    GtkWidget *anchor = gtk_widget_get_parent(GTK_WIDGET(popover));

    if (anchor != NULL && g_object_get_data(G_OBJECT(anchor), OPEN_NAME_DIALOG) == popover) {
        g_object_set_data(G_OBJECT(anchor), OPEN_NAME_DIALOG, NULL);
    }
    g_idle_add(unparent_idle, g_object_ref(popover));
}

static gboolean
select_stem_idle(gpointer data) {
    NameDialog *d = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(d->entry));
    const char *dot = strrchr(text, '.');
    int end = -1;

    if (dot != NULL && dot != text) {
        end = (int)g_utf8_strlen(text, dot - text);
    }
    gtk_editable_select_region(GTK_EDITABLE(d->entry), 0, end);
    d->select_source = 0;
    return G_SOURCE_REMOVE;
}

static void
name_dialog_show(HowlWindow *win, GFile *parent, HowlFile *file,
                 GtkWidget *anchor, const GdkRectangle *rect) {
    NameDialog *d;
    GtkWidget *box, *title;
    GtkWidget *open = g_object_get_data(G_OBJECT(anchor), OPEN_NAME_DIALOG);

    if (open != NULL && gtk_widget_get_visible(open)) {
        NameDialog *other = g_object_get_data(G_OBJECT(open), "name-dialog");

        gtk_widget_grab_focus(other->entry);
        return;
    }

    d = g_new0(NameDialog, 1);
    d->window = howl_window_get_gtk(win);
    g_object_add_weak_pointer(G_OBJECT(d->window), (gpointer *)&d->window);
    d->parent = g_object_ref(parent);
    if (file != NULL) {
        d->target = g_object_ref(howl_file_get_file(file));
        d->current_name = g_strdup(howl_file_get_display_name(file));
    }

    d->popover = gtk_popover_new();
    gtk_widget_set_parent(d->popover, anchor);
    gtk_popover_set_pointing_to(GTK_POPOVER(d->popover), rect);
    gtk_popover_set_position(GTK_POPOVER(d->popover), GTK_POS_BOTTOM);
    g_object_set_data_full(G_OBJECT(d->popover), "name-dialog", d, name_dialog_free);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    title = gtk_label_new(file != NULL ? "Rename" : "New Folder");
    gtk_widget_add_css_class(title, "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(box), title);

    d->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->entry), file != NULL ? "Name" : "Folder name");
    gtk_widget_set_size_request(d->entry, 260, -1);
    if (d->current_name != NULL) {
        gtk_editable_set_text(GTK_EDITABLE(d->entry), d->current_name);
    }
    gtk_box_append(GTK_BOX(box), d->entry);

    d->error = gtk_label_new("");
    gtk_widget_add_css_class(d->error, "howl-error");
    gtk_label_set_xalign(GTK_LABEL(d->error), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(d->error), TRUE);
    gtk_widget_set_visible(d->error, FALSE);
    gtk_box_append(GTK_BOX(box), d->error);

    d->button = gtk_button_new_with_label(file != NULL ? "Rename" : "Create");
    gtk_widget_add_css_class(d->button, "suggested-action");
    gtk_widget_set_halign(d->button, GTK_ALIGN_END);
    gtk_widget_set_sensitive(d->button, FALSE);
    gtk_box_append(GTK_BOX(box), d->button);

    gtk_popover_set_child(GTK_POPOVER(d->popover), box);

    g_signal_connect(d->entry, "changed", G_CALLBACK(name_dialog_validate), d);
    g_signal_connect(d->entry, "activate", G_CALLBACK(name_dialog_activate_cb), d);
    g_signal_connect(d->button, "clicked", G_CALLBACK(name_dialog_button_cb), d);
    g_signal_connect(d->popover, "closed", G_CALLBACK(name_dialog_closed_cb), NULL);

    g_object_set_data(G_OBJECT(anchor), OPEN_NAME_DIALOG, d->popover);
    gtk_popover_popup(GTK_POPOVER(d->popover));
    gtk_widget_grab_focus(d->entry);
    if (file != NULL && !howl_file_is_directory(file)) {
        d->select_source = g_idle_add(select_stem_idle, d);
    }
}

void howl_ops_new_folder(HowlWindow *win, GFile *parent, GtkWidget *anchor, const GdkRectangle *rect) {
    name_dialog_show(win, parent, NULL, anchor, rect);
}

void howl_ops_rename(HowlWindow *win, HowlFile *file, GtkWidget *anchor, const GdkRectangle *rect) {
    GFile *parent = g_file_get_parent(howl_file_get_file(file));

    if (parent == NULL) {
        return;
    }
    name_dialog_show(win, parent, file, anchor, rect);
    g_object_unref(parent);
}

typedef struct {
    GtkWindow *window;
    GList *gfiles;
    GList *sources;
    GFile *target;
    gboolean move;
    gboolean from_clipboard;
    GPtrArray *errors;
    char *first_name;
} Job;

static void
job_free(Job *job) {
    if (job->window != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(job->window), (gpointer *)&job->window);
    }
    g_list_free_full(job->gfiles, g_object_unref);
    g_list_free_full(job->sources, g_object_unref);
    g_clear_object(&job->target);
    if (job->errors != NULL) {
        g_ptr_array_unref(job->errors);
    }
    g_free(job->first_name);
    g_free(job);
}

static Job *
job_new(GtkWindow *window) {
    Job *job = g_new0(Job, 1);

    job->window = window;
    g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&job->window);
    job->errors = g_ptr_array_new_with_free_func(g_free);
    return job;
}

static void
job_add_error(Job *job, GFile *file, GError *error) {
    char *name = g_file_get_basename(file);

    g_ptr_array_add(job->errors, g_strdup_printf("%s: %s", name, error->message));
    g_free(name);
}

static void
job_report_errors(Job *job, const char *message) {
    if (job->errors->len == 0) {
        return;
    }
    g_ptr_array_add(job->errors, NULL);
    {
        char *detail = g_strjoinv("\n", (char **)job->errors->pdata);
        report(job->window, message, detail);
        g_free(detail);
    }
}

static void
job_set_status(Job *job, const char *text) {
    HowlWindow *win = window_from_gtk(job->window);

    if (win != NULL) {
        howl_window_set_status(win, text);
    }
}

static gboolean
delete_recursively(GFile *file, GError **error) {
    GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);

    if (type == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
        GFileInfo *info;

        if (en == NULL) {
            return FALSE;
        }
        while ((info = g_file_enumerator_next_file(en, NULL, error)) != NULL) {
            GFile *child = g_file_get_child(file, g_file_info_get_name(info));
            gboolean ok = delete_recursively(child, error);

            g_object_unref(child);
            g_object_unref(info);
            if (!ok) {
                g_object_unref(en);
                return FALSE;
            }
        }
        g_object_unref(en);
        if (error != NULL && *error != NULL) {
            return FALSE;
        }
    }
    return g_file_delete(file, NULL, error);
}

static void
delete_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
    Job *job = data;

    for (GList *l = job->gfiles; l != NULL; l = l->next) {
        GError *error = NULL;

        if (!delete_recursively(l->data, &error)) {
            job_add_error(job, l->data, error);
            g_error_free(error);
        }
    }
    g_task_return_boolean(task, TRUE);
}

static void
delete_done(GObject *source, GAsyncResult *result, gpointer data) {
    Job *job = data;

    job_set_status(job, NULL);
    job_report_errors(job, "Could not delete");
    job_free(job);
    howl_app_release();
}

static void
delete_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
    Job *job = data;
    GTask *task;

    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL) != 1) {
        job_free(job);
        return;
    }

    job_set_status(job, "Deleting\342\200\246");
    howl_app_hold();
    task = g_task_new(NULL, NULL, delete_done, job);
    g_task_set_task_data(task, job, NULL);
    g_task_run_in_thread(task, delete_thread);
    g_object_unref(task);
}

static void
delete_gfiles(HowlWindow *win, GList *gfiles) {
    GtkAlertDialog *dialog;
    Job *job;
    guint n = g_list_length(gfiles);
    const char *buttons[] = {"Cancel", "Delete", NULL};

    if (n == 0) {
        return;
    }

    job = job_new(howl_window_get_gtk(win));
    job->gfiles = copy_gfiles(gfiles);

    if (n == 1) {
        char *name = display_name(gfiles->data);

        dialog = gtk_alert_dialog_new("Permanently delete \342\200\234%s\342\200\235?", name);
        g_free(name);
    } else {
        dialog = gtk_alert_dialog_new("Permanently delete %u items?", n);
    }
    gtk_alert_dialog_set_detail(dialog, "Deleted items cannot be recovered.");
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, howl_window_get_gtk(win), NULL, delete_confirmed, job);
    g_object_unref(dialog);
}

void howl_ops_delete(HowlWindow *win, GList *files) {
    GList *gfiles = howl_files_to_gfiles(files);

    delete_gfiles(win, gfiles);
    g_list_free_full(gfiles, g_object_unref);
}

void howl_ops_trash_gfiles(HowlWindow *win, GList *gfiles) {
    GList *unsupported = NULL;
    GPtrArray *errors = g_ptr_array_new_with_free_func(g_free);

    for (GList *l = gfiles; l != NULL; l = l->next) {
        GError *error = NULL;

        if (g_file_trash(l->data, NULL, &error)) {
            continue;
        }
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
            unsupported = g_list_append(unsupported, l->data);
        } else {
            char *name = display_name(l->data);

            g_ptr_array_add(errors, g_strdup_printf("%s: %s", name, error->message));
            g_free(name);
        }
        g_error_free(error);
    }

    if (errors->len > 0) {
        char *detail;

        g_ptr_array_add(errors, NULL);
        detail = g_strjoinv("\n", (char **)errors->pdata);
        howl_window_show_error(win, "Could not move to trash", detail);
        g_free(detail);
    }
    g_ptr_array_unref(errors);

    if (unsupported != NULL) {
        delete_gfiles(win, unsupported);
        g_list_free(unsupported);
    }
}

void howl_ops_trash(HowlWindow *win, GList *files) {
    GList *gfiles = howl_files_to_gfiles(files);

    howl_ops_trash_gfiles(win, gfiles);
    g_list_free_full(gfiles, g_object_unref);
}

static void
refresh_cut_marks(void) {
    GListModel *toplevels = gtk_window_get_toplevels();
    guint n = g_list_model_get_n_items(toplevels);

    for (guint i = 0; i < n; i++) {
        GtkWindow *window = g_list_model_get_item(toplevels, i);
        HowlWindow *win = window_from_gtk(window);

        if (win != NULL) {
            howl_window_refresh_cut(win);
        }
        g_object_unref(window);
    }
}

static void
clear_cut_marks(void) {
    if (cut_paths != NULL && g_hash_table_size(cut_paths) > 0) {
        g_hash_table_remove_all(cut_paths);
        refresh_cut_marks();
    }
}

static void
clipboard_local_changed(GdkClipboard *clipboard, GParamSpec *pspec, gpointer data) {
    if (!gdk_clipboard_is_local(clipboard)) {
        clear_cut_marks();
    }
}

gboolean
howl_ops_is_cut(GFile *file) {
    char *path;
    gboolean cut;

    if (cut_paths == NULL || g_hash_table_size(cut_paths) == 0) {
        return FALSE;
    }
    path = g_file_get_path(file);
    cut = path != NULL && g_hash_table_contains(cut_paths, path);
    g_free(path);
    return cut;
}

void howl_ops_copy(HowlWindow *win, GList *files, gboolean cut) {
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(howl_window_get_gtk(win)));
    GString *gnome = g_string_new(cut ? "cut" : "copy");
    GString *uris = g_string_new(NULL);
    GString *paths = g_string_new(NULL);
    GdkContentProvider *providers[3];
    GdkContentProvider *content;
    GBytes *gnome_bytes, *uri_bytes;
    GValue value = G_VALUE_INIT;

    if (files == NULL) {
        return;
    }

    if (cut_paths == NULL) {
        cut_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    g_hash_table_remove_all(cut_paths);

    for (GList *l = files; l != NULL; l = l->next) {
        GFile *file = howl_file_get_file(l->data);
        char *uri = g_file_get_uri(file);
        char *path = g_file_get_path(file);

        g_string_append_c(gnome, '\n');
        g_string_append(gnome, uri);
        g_string_append(uris, uri);
        g_string_append(uris, "\r\n");
        if (paths->len > 0) {
            g_string_append_c(paths, '\n');
        }
        g_string_append(paths, path != NULL ? path : uri);
        if (cut && path != NULL) {
            g_hash_table_add(cut_paths, g_strdup(path));
        }
        g_free(uri);
        g_free(path);
    }

    gnome_bytes = g_string_free_to_bytes(gnome);
    uri_bytes = g_string_free_to_bytes(uris);
    providers[0] = gdk_content_provider_new_for_bytes(MIME_GNOME_COPIED_FILES, gnome_bytes);
    providers[1] = gdk_content_provider_new_for_bytes(MIME_URI_LIST, uri_bytes);
    g_bytes_unref(gnome_bytes);
    g_bytes_unref(uri_bytes);
    g_value_init(&value, G_TYPE_STRING);
    g_value_take_string(&value, g_string_free(paths, FALSE));
    providers[2] = gdk_content_provider_new_for_value(&value);
    g_value_unset(&value);

    content = gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
    gdk_clipboard_set_content(clipboard, content);
    g_object_unref(content);

    if (!clipboard_watched) {
        g_signal_connect(clipboard, "notify::local", G_CALLBACK(clipboard_local_changed), NULL);
        clipboard_watched = TRUE;
    }
    refresh_cut_marks();
}

static gboolean copy_recursively(GFile *src, GFile *dest, GError **error);

static gboolean
copy_one(GFile *src, GFile *dest, GError **error) {
    if (g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY) {
        return copy_recursively(src, dest, error);
    }
    return g_file_copy(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, error);
}

static gboolean
copy_recursively(GFile *src, GFile *dest, GError **error) {
    GFileEnumerator *en;
    GFileInfo *info;
    gboolean ok = TRUE;

    if (!g_file_make_directory(dest, NULL, error)) {
        return FALSE;
    }

    en = g_file_enumerate_children(src, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (en == NULL) {
        return FALSE;
    }

    while (ok && (info = g_file_enumerator_next_file(en, NULL, error)) != NULL) {
        GFile *child = g_file_get_child(src, g_file_info_get_name(info));
        GFile *dchild = g_file_get_child(dest, g_file_info_get_name(info));

        ok = copy_one(child, dchild, error);
        g_object_unref(child);
        g_object_unref(dchild);
        g_object_unref(info);
    }
    g_object_unref(en);
    if (error != NULL && *error != NULL) {
        return FALSE;
    }

    g_file_copy_attributes(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL);
    return ok;
}

static gboolean
move_one(GFile *src, GFile *dest, GError **error) {
    GError *local = NULL;

    if (g_file_move(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &local)) {
        return TRUE;
    }

    if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
        g_error_free(local);
        return copy_recursively(src, dest, error) && delete_recursively(src, error);
    }
    g_propagate_error(error, local);
    return FALSE;
}

static GFile *
unique_copy_name(GFile *dir, const char *name, gboolean is_dir) {
    const char *dot = is_dir ? NULL : strrchr(name, '.');
    char *stem = dot != NULL && dot != name ? g_strndup(name, dot - name) : g_strdup(name);
    const char *ext = dot != NULL && dot != name ? dot : "";
    GFile *candidate = NULL;

    for (int i = 1; i < 1000; i++) {
        char *try = i == 1 ? g_strdup_printf("%s (copy)%s", stem, ext)
                           : g_strdup_printf("%s (copy %d)%s", stem, i, ext);

        candidate = g_file_get_child(dir, try);
        g_free(try);
        if (!g_file_query_exists(candidate, NULL)) {
            break;
        }
        g_clear_object(&candidate);
    }
    g_free(stem);
    return candidate;
}

static void
transfer_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
    Job *job = data;

    for (GList *l = job->sources; l != NULL; l = l->next) {
        GFile *src = l->data;
        GFile *src_parent = g_file_get_parent(src);
        GFile *dest;
        GError *error = NULL;
        char *name = g_file_get_basename(src);
        gboolean same_dir = src_parent != NULL && g_file_equal(src_parent, job->target);
        gboolean is_dir = g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY;

        if (same_dir && job->move) {
            g_clear_object(&src_parent);
            g_free(name);
            continue;
        }

        if (same_dir) {
            dest = unique_copy_name(job->target, name, is_dir);
        } else {
            dest = g_file_get_child(job->target, name);
        }

        if (dest == NULL) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_EXISTS, "could not find a free name");
        } else if (g_file_equal(src, dest) || g_file_equal(job->target, src) || g_file_has_prefix(job->target, src)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "cannot %s a folder into itself", job->move ? "move" : "copy");
        } else if (g_file_query_exists(dest, NULL)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_EXISTS, "already exists in the destination");
        } else if (job->move) {
            move_one(src, dest, &error);
        } else {
            copy_one(src, dest, &error);
        }

        if (error != NULL) {
            job_add_error(job, src, error);
            g_error_free(error);
        } else if (job->first_name == NULL) {
            job->first_name = g_file_get_basename(dest);
        }

        g_clear_object(&dest);
        g_clear_object(&src_parent);
        g_free(name);
    }
    g_task_return_boolean(task, TRUE);
}

static void
transfer_done(GObject *source, GAsyncResult *result, gpointer data) {
    Job *job = data;
    HowlWindow *win = window_from_gtk(job->window);

    g_debug("%s done, %u errors", job->move ? "move" : "copy", job->errors->len);
    job_set_status(job, NULL);
    job_report_errors(job, job->move ? "Could not move" : "Could not copy");

    if (job->move && job->from_clipboard) {
        GdkClipboard *clipboard = job->window != NULL
            ? gtk_widget_get_clipboard(GTK_WIDGET(job->window))
            : NULL;

        clear_cut_marks();
        if (clipboard != NULL && gdk_clipboard_is_local(clipboard)) {
            gdk_clipboard_set_content(clipboard, NULL);
        }
    }

    if (win != NULL && job->first_name != NULL) {
        GFile *location = howl_window_get_location(win);

        if (location != NULL && g_file_equal(location, job->target)) {
            howl_window_select_name(win, job->first_name);
        }
    }
    job_free(job);
    howl_app_release();
}

static void
start_transfer(Job *job) {
    GTask *task;

    g_debug("%s %u items", job->move ? "moving" : "copying", g_list_length(job->sources));
    job_set_status(job, job->move ? "Moving\342\200\246" : "Copying\342\200\246");
    howl_app_hold();
    task = g_task_new(NULL, NULL, transfer_done, job);
    g_task_set_task_data(task, job, NULL);
    g_task_run_in_thread(task, transfer_thread);
    g_object_unref(task);
}

void howl_ops_transfer(HowlWindow *win, GList *gfiles, GFile *target_dir, gboolean move) {
    Job *job;

    if (gfiles == NULL) {
        return;
    }
    job = job_new(howl_window_get_gtk(win));
    job->sources = copy_gfiles(gfiles);
    job->target = g_object_ref(target_dir);
    job->move = move;
    start_transfer(job);
}

static GFile *
unique_link_name(GFile *dir, const char *name) {
    for (int i = 1; i < 1000; i++) {
        char *try = i == 1 ? g_strdup_printf("Link to %s", name)
                           : g_strdup_printf("Link to %s (%d)", name, i);
        GFile *candidate = g_file_get_child(dir, try);

        g_free(try);
        if (!g_file_query_exists(candidate, NULL)) {
            return candidate;
        }
        g_object_unref(candidate);
    }
    return NULL;
}

void howl_ops_link(HowlWindow *win, GList *gfiles, GFile *target_dir) {
    GPtrArray *errors = g_ptr_array_new_with_free_func(g_free);
    char *first_name = NULL;

    for (GList *l = gfiles; l != NULL; l = l->next) {
        GFile *src = l->data;
        char *name = g_file_get_basename(src);
        char *path = g_file_get_path(src);
        GFile *link = g_file_get_child(target_dir, name);
        GError *error = NULL;

        if (g_file_query_exists(link, NULL)) {
            g_object_unref(link);
            link = unique_link_name(target_dir, name);
        }
        if (path == NULL) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "not a local file");
        } else if (link == NULL) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_EXISTS, "could not find a free name");
        } else if (g_file_make_symbolic_link(link, path, NULL, &error) && first_name == NULL) {
            first_name = g_file_get_basename(link);
        }

        if (error != NULL) {
            char *display = display_name(src);

            g_ptr_array_add(errors, g_strdup_printf("%s: %s", display, error->message));
            g_free(display);
            g_error_free(error);
        }
        g_clear_object(&link);
        g_free(path);
        g_free(name);
    }

    if (errors->len > 0) {
        char *detail;

        g_ptr_array_add(errors, NULL);
        detail = g_strjoinv("\n", (char **)errors->pdata);
        howl_window_show_error(win, "Could not create link", detail);
        g_free(detail);
    }
    g_ptr_array_unref(errors);

    if (first_name != NULL) {
        GFile *location = howl_window_get_location(win);

        if (location != NULL && g_file_equal(location, target_dir)) {
            howl_window_select_name(win, first_name);
        }
        g_free(first_name);
    }
}

static void
paste_parse(Job *job, const char *mime, const char *text) {
    char **lines = g_strsplit(text, "\n", -1);
    char **line = lines;

    if (g_str_equal(mime, MIME_GNOME_COPIED_FILES) && *line != NULL) {
        job->move = g_str_equal(g_strstrip(*line), "cut");
        line++;
    }

    for (; *line != NULL; line++) {
        g_strstrip(*line);
        if (**line == '\0' || **line == '#') {
            continue;
        }
        {
            GFile *file = g_file_new_for_uri(*line);

            if (g_file_is_native(file)) {
                job->sources = g_list_append(job->sources, file);
            } else {
                g_object_unref(file);
            }
        }
    }
    g_strfreev(lines);
}

static void
paste_spliced(GObject *source, GAsyncResult *result, gpointer data) {
    Job *job = data;
    GMemoryOutputStream *out = G_MEMORY_OUTPUT_STREAM(source);
    const char *mime = g_object_get_data(G_OBJECT(out), "mime");
    char *text;

    g_debug("paste: received %" G_GSIZE_FORMAT " bytes", g_memory_output_stream_get_data_size(out));
    if (g_output_stream_splice_finish(G_OUTPUT_STREAM(out), result, NULL) < 0) {
        job_free(job);
        g_object_unref(out);
        return;
    }

    if (g_memory_output_stream_get_data_size(out) == 0) {
        job_free(job);
        g_object_unref(out);
        return;
    }
    text = g_strndup(g_memory_output_stream_get_data(out), g_memory_output_stream_get_data_size(out));
    paste_parse(job, mime, text);
    g_free(text);
    g_object_unref(out);

    if (job->sources == NULL) {
        job_free(job);
    } else {
        start_transfer(job);
    }
}

static void
paste_read(GObject *source, GAsyncResult *result, gpointer data) {
    Job *job = data;
    const char *mime = NULL;
    GInputStream *in;
    GOutputStream *out;

    in = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), result, &mime, NULL);
    g_debug("paste: clipboard offers %s", in != NULL ? mime : "nothing usable");
    if (in == NULL) {
        job_free(job);
        return;
    }

    out = g_memory_output_stream_new_resizable();
    g_object_set_data_full(G_OBJECT(out), "mime", g_strdup(mime), g_free);
    g_output_stream_splice_async(out, in,
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                 G_PRIORITY_DEFAULT, NULL, paste_spliced, job);
    g_object_unref(in);
}

void howl_ops_paste(HowlWindow *win, GFile *target_dir) {
    static const char *mimes[] = {MIME_GNOME_COPIED_FILES, MIME_URI_LIST, NULL};
    GtkWindow *window = howl_window_get_gtk(win);
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(window));
    Job *job = job_new(window);

    job->target = g_object_ref(target_dir);
    job->from_clipboard = TRUE;
    gdk_clipboard_read_async(clipboard, mimes, G_PRIORITY_DEFAULT, NULL, paste_read, job);
}
