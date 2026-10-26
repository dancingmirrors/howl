// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static const int zoom_steps[] = {16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512};
#define PENDING_SELECT_TIMEOUT (3 * G_USEC_PER_SEC)

typedef enum {
    CELL_GRID,
    CELL_NAME,
    CELL_SIZE,
    CELL_TYPE,
    CELL_MODIFIED
} CellKind;

typedef struct {
    HowlWindow *win;
    CellKind kind;
} CellFactory;

struct _HowlWindow {
    GtkWindow *window;
    GSimpleActionGroup *actions;

    GtkWidget *title_stack;
    GtkWidget *pathbar_scroller;
    GtkWidget *pathbar_box;
    GtkWidget *location_entry;

    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *paned;
    GtkWidget *places;
    GtkWidget *content_box;
    GtkWidget *stack;
    GtkWidget *grid_view;
    GtkWidget *column_view;
    GtkWidget *empty_label;
    GtkWidget *error_label;
    GtkWidget *status_label;
    GtkWidget *context_menu;
    GtkColumnViewColumn *col_name, *col_size, *col_type, *col_modified;

    HowlDir *dir;
    GtkCustomFilter *hidden_filter;
    GtkStringFilter *search_filter;
    GtkMultiSelection *selection;

    GFile *location;
    GList *back;
    GList *forward;
    char *pending_select;
    gint64 pending_select_time;
    GHashTable *item_widgets;
    char *status_override;
    gboolean show_hidden;
    gboolean grid;
    int grid_size;
    int list_size;
    double menu_x, menu_y;
    gint64 menu_time;
    gboolean closing;
};

static void go(HowlWindow *win, GFile *location, gboolean record);
static void show_context_menu(HowlWindow *win, gboolean on_item, double x, double y);
static void update_actions(HowlWindow *win);

GtkWindow *
howl_window_get_gtk(HowlWindow *win) {
    return win->window;
}

GFile *
howl_window_get_location(HowlWindow *win) {
    return win->location;
}

GList *
howl_window_get_selection(HowlWindow *win) {
    GtkBitset *set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(win->selection));
    GtkBitsetIter iter;
    guint pos;
    GList *list = NULL;

    if (gtk_bitset_iter_init_first(&iter, set, &pos)) {
        do {
            list = g_list_prepend(list, g_list_model_get_item(G_LIST_MODEL(win->selection), pos));
        } while (gtk_bitset_iter_next(&iter, &pos));
    }
    gtk_bitset_unref(set);
    return g_list_reverse(list);
}

static guint
selection_count(HowlWindow *win) {
    GtkBitset *set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(win->selection));
    guint n = (guint)gtk_bitset_get_size(set);

    gtk_bitset_unref(set);
    return n;
}

static GList *
selected_regular_files(HowlWindow *win) {
    GList *sel = howl_window_get_selection(win), *files = NULL;

    for (GList *l = sel; l != NULL; l = l->next) {
        if (!howl_file_is_directory(l->data)) {
            files = g_list_prepend(files, g_object_ref(l->data));
        }
    }
    g_list_free_full(sel, g_object_unref);
    return g_list_reverse(files);
}

static GtkWidget *
current_view(HowlWindow *win) {
    return win->grid ? win->grid_view : win->column_view;
}

static int
cell_icon_size(HowlWindow *win, CellKind kind) {
    return kind == CELL_GRID ? win->grid_size : win->list_size;
}

static int
grid_cell_width(HowlWindow *win) {
    return MAX(win->grid_size + 48, 112);
}

static void
update_max_columns(HowlWindow *win) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win->window));
    int width = gtk_widget_get_width(GTK_WIDGET(win->window));
    guint columns;

    if (surface != NULL) {
        width = MAX(width, gdk_surface_get_width(surface));
    }
    columns = MAX(1, width / grid_cell_width(win) + 1);
    if (gtk_grid_view_get_max_columns(GTK_GRID_VIEW(win->grid_view)) != columns) {
        gtk_grid_view_set_max_columns(GTK_GRID_VIEW(win->grid_view), columns);
    }
}

static void
surface_layout_cb(GdkSurface *surface, int width, int height, gpointer data) {
    update_max_columns(data);
}

static void
window_size_cb(GObject *object, GParamSpec *pspec, gpointer data) {
    GtkWindow *window = GTK_WINDOW(object);
    int width = 0, height = 0;

    howl_opts.maximized = gtk_window_is_maximized(window);
    if (!howl_opts.maximized && !gtk_window_is_fullscreen(window)) {
        gtk_window_get_default_size(window, &width, &height);
        if (width > 0 && height > 0) {
            howl_opts.window_width = width;
            howl_opts.window_height = height;
        }
    }
    howl_settings_save();
}

static void
window_unrealize_cb(GtkWidget *widget, gpointer data) {
    HowlWindow *win = data;

    win->closing = TRUE;
}

static void
window_realize_cb(GtkWidget *widget, gpointer data) {
    HowlWindow *win = data;

    win->closing = FALSE;
    g_signal_connect(gtk_native_get_surface(GTK_NATIVE(widget)), "layout",
                     G_CALLBACK(surface_layout_cb), data);
    update_max_columns(data);
}

void howl_window_show_error(HowlWindow *win, const char *message, const char *detail) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);

    if (detail != NULL) {
        gtk_alert_dialog_set_detail(dialog, detail);
    }
    gtk_alert_dialog_show(dialog, win->window);
    g_object_unref(dialog);
}

static void
update_status(HowlWindow *win) {
    char *text = NULL;
    GList *sel;
    guint visible, total, selected;
    gboolean markup = FALSE;

    if (win->closing) {
        return;
    }
    if (win->status_override != NULL) {
        gtk_label_set_text(GTK_LABEL(win->status_label), win->status_override);
        gtk_widget_set_visible(win->status_label, TRUE);
        return;
    }

    if (howl_dir_get_error(win->dir) != NULL) {
        gtk_widget_set_visible(win->status_label, FALSE);
        return;
    }

    visible = g_list_model_get_n_items(G_LIST_MODEL(win->selection));
    total = g_list_model_get_n_items(howl_dir_get_model(win->dir));
    sel = howl_window_get_selection(win);
    selected = g_list_length(sel);

    if (howl_dir_is_loading(win->dir) && visible == 0) {
        text = g_strdup("Loading\342\200\246");
    } else if (sel == NULL) {
        GString *s = g_string_new(NULL);
        const char *search = gtk_string_filter_get_search(win->search_filter);

        g_string_append_printf(s, "%u item%s", visible, visible == 1 ? "" : "s");
        if (total > visible) {
            g_string_append_printf(s, " (%u %s)", total - visible,
                                   search != NULL && *search != '\0' ? "filtered" : "hidden");
        }
        text = g_string_free(s, FALSE);
    } else {
        guint64 bytes = 0;
        guint files = 0;

        for (GList *l = sel; l != NULL; l = l->next) {
            if (!howl_file_is_directory(l->data)) {
                bytes += howl_file_get_size(l->data);
                files++;
            }
        }

        if (sel->next == NULL) {
            char *name = g_markup_escape_text(howl_file_get_display_name(sel->data), -1);
            char *details = howl_file_describe(sel->data);

            text = g_strdup_printf("\342\200\234%s\342\200\235 selected\n%s", name, details);
            markup = TRUE;
            g_free(details);
            g_free(name);
        } else if (files > 0) {
            char *size = g_format_size(bytes);
            text = g_strdup_printf("%u items selected (%s)", selected, size);
            g_free(size);
        } else {
            text = g_strdup_printf("%u items selected", selected);
        }
    }
    g_list_free_full(sel, g_object_unref);

    if (markup) {
        gtk_label_set_markup(GTK_LABEL(win->status_label), text);
    } else {
        gtk_label_set_text(GTK_LABEL(win->status_label), text);
    }
    gtk_widget_set_visible(win->status_label, TRUE);
    g_free(text);
}

static void
update_page(HowlWindow *win) {
    const GError *error = howl_dir_get_error(win->dir);
    guint visible = g_list_model_get_n_items(G_LIST_MODEL(win->selection));

    if (win->closing) {
        return;
    }
    if (error != NULL) {
        gtk_label_set_text(GTK_LABEL(win->error_label), error->message);
        gtk_stack_set_visible_child_name(GTK_STACK(win->stack), "error");
    } else if (visible == 0 && !howl_dir_is_loading(win->dir)) {
        const char *search = gtk_string_filter_get_search(win->search_filter);

        gtk_label_set_text(GTK_LABEL(win->empty_label),
                           search != NULL && *search != '\0' ? "No Results Found" : "Folder is Empty");
        gtk_stack_set_visible_child_name(GTK_STACK(win->stack), "empty");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(win->stack), win->grid ? "grid" : "list");
    }
}

void howl_window_set_status(HowlWindow *win, const char *text) {
    g_free(win->status_override);
    win->status_override = g_strdup(text);
    update_status(win);
}

static void
select_and_reveal(HowlWindow *win, guint position) {
    if (win->closing) {
        return;
    }
#if GTK_CHECK_VERSION(4, 12, 0)
    if (win->grid) {
        gtk_grid_view_scroll_to(GTK_GRID_VIEW(win->grid_view), position,
                                GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, NULL);
    } else {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(win->column_view), position, NULL,
                                  GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, NULL);
    }
#else
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(win->selection), position, TRUE);
#endif
}

static gboolean
try_pending_select(HowlWindow *win) {
    guint n;

    if (win->pending_select == NULL) {
        return FALSE;
    }

    n = g_list_model_get_n_items(G_LIST_MODEL(win->selection));
    for (guint i = 0; i < n; i++) {
        HowlFile *file = g_list_model_get_item(G_LIST_MODEL(win->selection), i);
        gboolean match = g_strcmp0(howl_file_get_name(file), win->pending_select) == 0;

        g_object_unref(file);
        if (match) {
            g_clear_pointer(&win->pending_select, g_free);
            select_and_reveal(win, i);
            return TRUE;
        }
    }
    return FALSE;
}

void howl_window_select_name(HowlWindow *win, const char *name) {
    g_free(win->pending_select);
    win->pending_select = g_strdup(name);
    win->pending_select_time = g_get_monotonic_time();
    try_pending_select(win);
}

static void
dir_changed_cb(HowlDir *dir, gpointer data) {
    HowlWindow *win = data;

    update_page(win);
    update_status(win);

    if (win->pending_select != NULL && !try_pending_select(win) && !howl_dir_is_loading(dir) &&
        g_get_monotonic_time() - win->pending_select_time > PENDING_SELECT_TIMEOUT) {
        g_clear_pointer(&win->pending_select, g_free);
    }
}

static void
items_changed_cb(GListModel *model, guint position, guint removed, guint added, gpointer data) {
    HowlWindow *win = data;

    update_page(win);
    update_status(win);
}

static void
selection_changed_cb(GtkSelectionModel *model, guint position, guint n_items, gpointer data) {
    HowlWindow *win = data;

    update_status(win);
    update_actions(win);
}

static void
configure_cell_sizes(HowlWindow *win, GtkWidget *root, CellKind kind) {
    GtkWidget *image = g_object_get_data(G_OBJECT(root), "image");
    GtkWidget *label = g_object_get_data(G_OBJECT(root), "label");
    int size;

    if (image == NULL) {
        return;
    }

    size = cell_icon_size(win, kind);
    gtk_widget_set_size_request(image, size, size);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);

    if (kind == CELL_GRID) {
        int cell_width = grid_cell_width(win);

        gtk_widget_set_size_request(root, cell_width, -1);
        gtk_label_set_max_width_chars(GTK_LABEL(label), MAX(6, (cell_width - 16) / 9));

        if (g_object_get_data(G_OBJECT(label), "measured") == NULL) {
            PangoLayout *layout = gtk_widget_create_pango_layout(label, "Xg\nXg");
            int height = 0;

            pango_layout_get_pixel_size(layout, NULL, &height);
            gtk_widget_set_size_request(label, -1, height);
            g_object_unref(layout);
            g_object_set_data(G_OBJECT(label), "measured", GINT_TO_POINTER(1));
        }
    }
}

static void
refresh_image(HowlWindow *win, GtkWidget *root, HowlFile *file) {
    GtkWidget *image = g_object_get_data(G_OBJECT(root), "image");
    CellKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(root), "kind"));
    int size = cell_icon_size(win, kind);
    GdkPaintable *thumbnail = NULL;

    if (howl_opts.thumbnails && !howl_file_is_directory(file)) {
        thumbnail = howl_file_get_thumbnail(file, size);
        if (thumbnail == NULL) {
            howl_thumbs_request(file, size, gtk_widget_get_scale_factor(root));
            thumbnail = howl_file_get_thumbnail(file, size);
        }
    }

    if (thumbnail != NULL) {
        gtk_image_set_from_paintable(GTK_IMAGE(image), thumbnail);
    } else if (howl_file_get_icon(file) != NULL) {
        gtk_image_set_from_gicon(GTK_IMAGE(image), howl_file_get_icon(file));
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(image), howl_file_is_directory(file) ? "folder" : "text-x-generic");
    }
}

static void
refresh_cell(HowlWindow *win, GtkWidget *root, HowlFile *file) {
    CellKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(root), "kind"));
    GtkWidget *label = g_object_get_data(G_OBJECT(root), "label");
    char *text;

    switch (kind) {
    case CELL_GRID:
    case CELL_NAME:
        gtk_label_set_text(GTK_LABEL(label), howl_file_get_display_name(file));
        refresh_image(win, root, file);
        gtk_widget_set_opacity(root, howl_ops_is_cut(howl_file_get_file(file)) ? 0.5 : 1.0);
        break;
    case CELL_SIZE:
        text = howl_file_format_size(file);
        gtk_label_set_text(GTK_LABEL(label), text);
        g_free(text);
        break;
    case CELL_TYPE:
        gtk_label_set_text(GTK_LABEL(label), howl_file_get_type_description(file));
        break;
    case CELL_MODIFIED:
        text = howl_file_format_mtime(file);
        gtk_label_set_text(GTK_LABEL(label), text);
        g_free(text);
        break;
    }
}

static void
file_changed_cb(HowlFile *file, gpointer data) {
    GtkWidget *root = data;
    HowlWindow *win = g_object_get_data(G_OBJECT(root), "window");

    refresh_cell(win, root, file);
}

static void
file_thumbnail_cb(GObject *object, GParamSpec *pspec, gpointer data) {
    GtkWidget *root = data;
    HowlWindow *win = g_object_get_data(G_OBJECT(root), "window");

    refresh_image(win, root, HOWL_FILE(object));
}

static void
set_rubberband(HowlWindow *win, gboolean enable) {
    gtk_grid_view_set_enable_rubberband(GTK_GRID_VIEW(win->grid_view), enable);
    gtk_column_view_set_enable_rubberband(GTK_COLUMN_VIEW(win->column_view), enable);
}

static void
item_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    HowlWindow *win = data;
    GtkWidget *root = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GtkListItem *item = g_object_get_data(G_OBJECT(root), "list-item");
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    guint position;
    HowlFile *file;

    if (item == NULL) {
        return;
    }
    position = gtk_list_item_get_position(item);
    file = gtk_list_item_get_item(item);
    if (file == NULL || position == GTK_INVALID_LIST_POSITION) {
        return;
    }

    if (button == GDK_BUTTON_PRIMARY) {
        set_rubberband(win, FALSE);
    } else if (button == GDK_BUTTON_SECONDARY) {
        graphene_point_t p = GRAPHENE_POINT_INIT((float)x, (float)y), out;

        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(win->selection), position)) {
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(win->selection), position, TRUE);
        }
        if (!gtk_widget_compute_point(root, win->content_box, &p, &out)) {
            out = p;
        }
        show_context_menu(win, TRUE, out.x, out.y);
    } else if (button == GDK_BUTTON_MIDDLE && howl_file_is_directory(file)) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        howl_window_new(howl_file_get_file(file), NULL);
    }
}

static void
item_released(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    HowlWindow *win = data;

    if (!win->closing) {
        set_rubberband(win, TRUE);
    }
}

static void
item_stopped(GtkGestureClick *gesture, gpointer data) {
    item_released(gesture, 0, 0, 0, data);
}

static void
drag_files_free(gpointer data) {
    g_list_free_full(data, g_object_unref);
}

static GdkContentProvider *
item_drag_prepare(GtkDragSource *source, double x, double y, gpointer data) {
    HowlWindow *win = data;
    GtkWidget *root = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    GtkListItem *item = g_object_get_data(G_OBJECT(root), "list-item");
    HowlFile *file;
    guint position;
    GList *files, *gfiles;
    GdkContentProvider *content;
    GdkDragAction actions = GDK_ACTION_COPY | GDK_ACTION_LINK;
    char *path;

    if (item == NULL || win->closing) {
        return NULL;
    }
    position = gtk_list_item_get_position(item);
    file = gtk_list_item_get_item(item);
    if (file == NULL || position == GTK_INVALID_LIST_POSITION) {
        return NULL;
    }

    if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(win->selection), position)) {
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(win->selection), position, TRUE);
    }
    files = howl_window_get_selection(win);
    if (files == NULL) {
        return NULL;
    }
    g_object_set_data_full(G_OBJECT(source), "drag-files", files, drag_files_free);
    g_object_set_data(G_OBJECT(source), "drag-x", GINT_TO_POINTER((int)x));
    g_object_set_data(G_OBJECT(source), "drag-y", GINT_TO_POINTER((int)y));
    gtk_gesture_set_state(GTK_GESTURE(source), GTK_EVENT_SEQUENCE_CLAIMED);

    path = win->location != NULL ? g_file_get_path(win->location) : NULL;
    if (path != NULL && g_access(path, W_OK) == 0) {
        actions |= GDK_ACTION_MOVE;
    }
    g_free(path);
    gtk_drag_source_set_actions(source, actions);

    gfiles = howl_files_to_gfiles(files);
    content = howl_dnd_content_for_files(gfiles);
    g_list_free_full(gfiles, g_object_unref);
    return content;
}

static void
item_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer data) {
    HowlWindow *win = data;
    GtkWidget *root = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    GtkListItem *item = g_object_get_data(G_OBJECT(root), "list-item");
    HowlFile *file = item != NULL ? gtk_list_item_get_item(item) : NULL;
    GList *files = g_object_get_data(G_OBJECT(source), "drag-files");
    GtkWidget *name_root, *image, *icon;
    GdkPaintable *paintable = NULL;
    int size, width = 0, height = 0, hot_x = 0, hot_y = 0;

    set_rubberband(win, FALSE);

    if (file == NULL) {
        return;
    }

    name_root = g_hash_table_lookup(win->item_widgets, file);
    image = name_root != NULL ? g_object_get_data(G_OBJECT(name_root), "image") : NULL;
    if (image != NULL && gtk_image_get_storage_type(GTK_IMAGE(image)) == GTK_IMAGE_PAINTABLE) {
        paintable = gtk_image_get_paintable(GTK_IMAGE(image));
    }

    size = CLAMP(win->grid ? win->grid_size : win->list_size, 32, 64);
    icon = howl_dnd_icon_new(paintable, howl_file_get_icon(file), howl_file_get_display_name(file),
                             g_list_length(files), size);
    gtk_drag_icon_set_child(GTK_DRAG_ICON(gtk_drag_icon_get_for_drag(drag)), icon);
    gtk_widget_measure(icon, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &width, NULL, NULL);
    gtk_widget_measure(icon, GTK_ORIENTATION_VERTICAL, width, NULL, &height, NULL, NULL);
    if (win->grid) {
        hot_x = CLAMP(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(source), "drag-x")), 0, width);
        hot_y = CLAMP(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(source), "drag-y")), 0, height);
    }
    gdk_drag_set_hotspot(drag, hot_x, hot_y);
}

static void
item_drag_end(GtkDragSource *source, GdkDrag *drag, gboolean delete_data, gpointer data) {
    HowlWindow *win = data;

    if (gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source)) != NULL && !win->closing) {
        set_rubberband(win, TRUE);
    }
    g_object_set_data(G_OBJECT(source), "drag-files", NULL);
}

static void
cell_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    CellFactory *cf = data;
    GtkWidget *root, *image = NULL, *label;
    GtkGesture *gesture;
    GtkDragSource *drag;

    label = gtk_label_new(NULL);

    switch (cf->kind) {
    case CELL_GRID:
        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(root, "howl-grid-item");
        image = gtk_image_new();
        gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_lines(GTK_LABEL(label), 2);
        gtk_widget_set_valign(label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(root), image);
        gtk_box_append(GTK_BOX(root), label);
        break;
    case CELL_NAME:
        root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        image = gtk_image_new();
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_append(GTK_BOX(root), image);
        gtk_box_append(GTK_BOX(root), label);
        break;
    case CELL_SIZE:
        root = label;
        gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
        gtk_widget_add_css_class(label, "numeric");
        break;
    case CELL_TYPE:
    case CELL_MODIFIED:
    default:
        root = label;
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        break;
    }

    g_object_set_data(G_OBJECT(root), "image", image);
    g_object_set_data(G_OBJECT(root), "label", label);
    g_object_set_data(G_OBJECT(root), "list-item", item);
    g_object_set_data(G_OBJECT(root), "kind", GINT_TO_POINTER(cf->kind));
    g_object_set_data(G_OBJECT(root), "window", cf->win);

    gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
    g_signal_connect(gesture, "pressed", G_CALLBACK(item_pressed), cf->win);
    g_signal_connect(gesture, "released", G_CALLBACK(item_released), cf->win);
    g_signal_connect(gesture, "stopped", G_CALLBACK(item_stopped), cf->win);
    gtk_widget_add_controller(root, GTK_EVENT_CONTROLLER(gesture));

    drag = gtk_drag_source_new();
    g_signal_connect(drag, "prepare", G_CALLBACK(item_drag_prepare), cf->win);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(item_drag_begin), cf->win);
    g_signal_connect(drag, "drag-end", G_CALLBACK(item_drag_end), cf->win);
    gtk_widget_add_controller(root, GTK_EVENT_CONTROLLER(drag));

    gtk_list_item_set_child(item, root);
}

static void
cell_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    CellFactory *cf = data;
    GtkWidget *root = gtk_list_item_get_child(item);
    HowlFile *file = gtk_list_item_get_item(item);
    gulong id;

    configure_cell_sizes(cf->win, root, cf->kind);

    if (cf->kind == CELL_GRID || cf->kind == CELL_NAME) {
        g_hash_table_insert(cf->win->item_widgets, file, root);
        id = g_signal_connect(file, "notify::thumbnail", G_CALLBACK(file_thumbnail_cb), root);
        g_object_set_data(G_OBJECT(root), "thumbnail-handler", GSIZE_TO_POINTER(id));
    }
    id = g_signal_connect(file, "changed", G_CALLBACK(file_changed_cb), root);
    g_object_set_data(G_OBJECT(root), "changed-handler", GSIZE_TO_POINTER(id));

    refresh_cell(cf->win, root, file);
}

static void
cell_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    CellFactory *cf = data;
    GtkWidget *root = gtk_list_item_get_child(item);
    HowlFile *file = gtk_list_item_get_item(item);
    gulong id;

    id = GPOINTER_TO_SIZE(g_object_steal_data(G_OBJECT(root), "thumbnail-handler"));
    if (id != 0) {
        g_signal_handler_disconnect(file, id);
    }
    id = GPOINTER_TO_SIZE(g_object_steal_data(G_OBJECT(root), "changed-handler"));
    if (id != 0) {
        g_signal_handler_disconnect(file, id);
    }

    if (g_hash_table_lookup(cf->win->item_widgets, file) == root) {
        g_hash_table_remove(cf->win->item_widgets, file);
    }
    howl_thumbs_cancel(file);
    howl_file_release_thumbnails(file);
}

static void
cell_factory_free(gpointer data, GClosure *closure) {
    g_free(data);
}

static GtkListItemFactory *
cell_factory_new(HowlWindow *win, CellKind kind) {
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    CellFactory *cf = g_new0(CellFactory, 1);

    cf->win = win;
    cf->kind = kind;
    g_signal_connect_data(factory, "setup", G_CALLBACK(cell_setup), cf, cell_factory_free, 0);
    g_signal_connect(factory, "bind", G_CALLBACK(cell_bind), cf);
    g_signal_connect(factory, "unbind", G_CALLBACK(cell_unbind), cf);
    return factory;
}

void howl_window_refresh_cut(HowlWindow *win) {
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, win->item_widgets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gtk_widget_set_opacity(value, howl_ops_is_cut(howl_file_get_file(key)) ? 0.5 : 1.0);
    }
}

static void
apply_zoom(HowlWindow *win) {
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, win->item_widgets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        CellKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(value), "kind"));

        configure_cell_sizes(win, value, kind);
        refresh_image(win, value, key);
    }
}

static void
zoom(HowlWindow *win, int delta) {
    int *size = win->grid ? &win->grid_size : &win->list_size;
    int next = *size;

    if (delta == 0) {
        next = win->grid ? howl_opts.grid_size : howl_opts.list_size;
    } else if (delta > 0) {
        for (gsize i = 0; i < G_N_ELEMENTS(zoom_steps); i++) {
            if (zoom_steps[i] > *size) {
                next = zoom_steps[i];
                break;
            }
        }
    } else {
        for (gsize i = G_N_ELEMENTS(zoom_steps); i > 0; i--) {
            if (zoom_steps[i - 1] < *size) {
                next = zoom_steps[i - 1];
                break;
            }
        }
    }
    if (next == *size) {
        return;
    }
    *size = next;
    if (win->grid) {
        howl_opts.grid_size = next;
    } else {
        howl_opts.list_size = next;
    }
    howl_settings_save();
    update_max_columns(win);
    apply_zoom(win);
}

static void
view_activate_cb(GtkWidget *view, guint position, gpointer data) {
    HowlWindow *win = data;
    HowlFile *file = g_list_model_get_item(G_LIST_MODEL(win->selection), position);
    GList *list;

    if (file == NULL) {
        return;
    }
    list = g_list_append(NULL, file);
    howl_ops_open(win, list);
    g_list_free(list);
    g_object_unref(file);
}

static gboolean
widget_is_in_item(GtkWidget *view, GtkWidget *widget) {
    for (; widget != NULL && widget != view; widget = gtk_widget_get_parent(widget)) {
        if (g_object_get_data(G_OBJECT(widget), "list-item") != NULL) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
background_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    HowlWindow *win = data;
    GtkWidget *view = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    GtkWidget *picked = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT);
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    if (widget_is_in_item(view, picked)) {
        return;
    }

    if (button == GDK_BUTTON_SECONDARY) {
        graphene_point_t p = GRAPHENE_POINT_INIT((float)x, (float)y), out;

        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(win->selection));
        if (!gtk_widget_compute_point(view, win->content_box, &p, &out)) {
            out = p;
        }
        show_context_menu(win, FALSE, out.x, out.y);
    } else if (button == GDK_BUTTON_PRIMARY && (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == 0) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(win->selection));
    }
}

static gboolean
scroll_zoom_cb(GtkEventControllerScroll *controller, double dx, double dy, gpointer data) {
    HowlWindow *win = data;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));

    if ((state & GDK_CONTROL_MASK) == 0 || dy == 0) {
        return GDK_EVENT_PROPAGATE;
    }
    zoom(win, dy < 0 ? 1 : -1);
    return GDK_EVENT_STOP;
}

static void
add_view_controllers(HowlWindow *win, GtkWidget *view) {
    GtkGesture *gesture = gtk_gesture_click_new();
    GtkEventController *scroll;

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
    g_signal_connect(gesture, "pressed", G_CALLBACK(background_pressed), win);
    gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(gesture));

    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                             GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_zoom_cb), win);
    gtk_widget_add_controller(view, scroll);
}

static int
compare_dirs_first(gconstpointer a, gconstpointer b, gpointer data) {
    gboolean da = howl_file_is_directory((HowlFile *)a);
    gboolean db = howl_file_is_directory((HowlFile *)b);

    return da == db ? 0 : (da ? -1 : 1);
}

static int
compare_type(gconstpointer a, gconstpointer b, gpointer data) {
    int r = g_utf8_collate(howl_file_get_type_description((HowlFile *)a),
                           howl_file_get_type_description((HowlFile *)b));

    if (r == 0) {
        r = g_utf8_collate(howl_file_get_display_name((HowlFile *)a),
                           howl_file_get_display_name((HowlFile *)b));
    }
    return r;
}

static gboolean
filter_hidden(gpointer item, gpointer data) {
    HowlWindow *win = data;

    return win->show_hidden || !howl_file_is_hidden(item);
}

static GtkColumnViewColumn *
add_column(HowlWindow *win, const char *title, CellKind kind, GtkSorter *sorter, gboolean expand) {
    GtkColumnViewColumn *column = gtk_column_view_column_new(title, cell_factory_new(win, kind));

    gtk_column_view_column_set_sorter(column, sorter);
    gtk_column_view_column_set_expand(column, expand);
    gtk_column_view_column_set_resizable(column, TRUE);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(win->column_view), column);
    g_clear_object(&sorter);
    return column;
}

static void
sync_sort_actions(GtkSorter *sorter, GtkSorterChange change, gpointer data) {
    HowlWindow *win = data;

    if (win->closing) {
        return;
    }
    GtkColumnViewSorter *cvs = GTK_COLUMN_VIEW_SORTER(sorter);
    GtkColumnViewColumn *column = gtk_column_view_sorter_get_primary_sort_column(cvs);
    GtkSortType order = gtk_column_view_sorter_get_primary_sort_order(cvs);
    const char *name = "name";
    GAction *action;

    if (column == win->col_size) {
        name = "size";
    } else if (column == win->col_type) {
        name = "type";
    } else if (column == win->col_modified) {
        name = "modified";
    }

    action = g_action_map_lookup_action(G_ACTION_MAP(win->actions), "sort-by");
    g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_string(name));
    action = g_action_map_lookup_action(G_ACTION_MAP(win->actions), "sort-reverse");
    g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(order == GTK_SORT_DESCENDING));
    howl_opts.sort_by = howl_settings_canonical_sort(name);
    howl_opts.sort_reverse = order == GTK_SORT_DESCENDING;
    howl_settings_save();
}

static void
build_models_and_views(HowlWindow *win) {
    GtkExpression *expr;
    GtkStringSorter *name_sorter;
    GtkEveryFilter *every;
    GtkFilterListModel *filter_model;
    GtkSortListModel *sort_model;
    GtkMultiSorter *multi;
    GtkCustomSorter *dirs_first;

    win->dir = howl_dir_new(dir_changed_cb, win);

    win->hidden_filter = gtk_custom_filter_new(filter_hidden, win, NULL);
    expr = gtk_property_expression_new(HOWL_TYPE_FILE, NULL, "display-name");
    win->search_filter = gtk_string_filter_new(expr);
    gtk_string_filter_set_match_mode(win->search_filter, GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
    every = gtk_every_filter_new();
    gtk_multi_filter_append(GTK_MULTI_FILTER(every), GTK_FILTER(g_object_ref(win->hidden_filter)));
    gtk_multi_filter_append(GTK_MULTI_FILTER(every), GTK_FILTER(g_object_ref(win->search_filter)));
    filter_model = gtk_filter_list_model_new(g_object_ref(howl_dir_get_model(win->dir)), GTK_FILTER(every));

    win->column_view = gtk_column_view_new(NULL);
    gtk_column_view_set_enable_rubberband(GTK_COLUMN_VIEW(win->column_view), TRUE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(win->column_view), FALSE);
    gtk_widget_add_css_class(win->column_view, "howl-list");

    expr = gtk_property_expression_new(HOWL_TYPE_FILE, NULL, "display-name");
    name_sorter = gtk_string_sorter_new(expr);
    gtk_string_sorter_set_collation(name_sorter, GTK_COLLATION_FILENAME);
    gtk_string_sorter_set_ignore_case(name_sorter, TRUE);
    win->col_name = add_column(win, "Name", CELL_NAME, GTK_SORTER(name_sorter), TRUE);
    win->col_size = add_column(win, "Size", CELL_SIZE,
                               GTK_SORTER(gtk_numeric_sorter_new(gtk_property_expression_new(HOWL_TYPE_FILE, NULL, "size"))),
                               FALSE);
    win->col_type = add_column(win, "Type", CELL_TYPE,
                               GTK_SORTER(gtk_custom_sorter_new(compare_type, NULL, NULL)), FALSE);
    win->col_modified = add_column(win, "Modified", CELL_MODIFIED,
                                   GTK_SORTER(gtk_numeric_sorter_new(gtk_property_expression_new(HOWL_TYPE_FILE, NULL, "mtime"))),
                                   FALSE);

    dirs_first = gtk_custom_sorter_new(compare_dirs_first, NULL, NULL);
    multi = gtk_multi_sorter_new();
    gtk_multi_sorter_append(multi, GTK_SORTER(dirs_first));
    gtk_multi_sorter_append(multi, g_object_ref(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(win->column_view))));
    sort_model = gtk_sort_list_model_new(G_LIST_MODEL(filter_model), GTK_SORTER(multi));
    g_signal_connect(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(win->column_view)), "changed",
                     G_CALLBACK(sync_sort_actions), win);

    win->selection = gtk_multi_selection_new(G_LIST_MODEL(sort_model));
    g_signal_connect(win->selection, "items-changed", G_CALLBACK(items_changed_cb), win);
    g_signal_connect(win->selection, "selection-changed", G_CALLBACK(selection_changed_cb), win);

    win->grid_view = gtk_grid_view_new(NULL, cell_factory_new(win, CELL_GRID));
    gtk_grid_view_set_enable_rubberband(GTK_GRID_VIEW(win->grid_view), TRUE);
    gtk_widget_add_css_class(win->grid_view, "howl-grid");

    g_signal_connect(win->grid_view, "activate", G_CALLBACK(view_activate_cb), win);
    g_signal_connect(win->column_view, "activate", G_CALLBACK(view_activate_cb), win);
    add_view_controllers(win, win->grid_view);
    add_view_controllers(win, win->column_view);
}

static void
set_view(HowlWindow *win, gboolean grid) {
    GAction *action;

    win->grid = grid;
    if (grid) {
        gtk_column_view_set_model(GTK_COLUMN_VIEW(win->column_view), NULL);
        gtk_grid_view_set_model(GTK_GRID_VIEW(win->grid_view), GTK_SELECTION_MODEL(win->selection));
    } else {
        gtk_grid_view_set_model(GTK_GRID_VIEW(win->grid_view), NULL);
        gtk_column_view_set_model(GTK_COLUMN_VIEW(win->column_view), GTK_SELECTION_MODEL(win->selection));
    }

    action = g_action_map_lookup_action(G_ACTION_MAP(win->actions), "view");
    g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_string(grid ? "grid" : "list"));
    howl_opts.list_view = !grid;
    howl_settings_save();
    update_page(win);
}

static void
menu_add(GMenu *menu, const char *label, const char *action, const char *accel) {
    GMenuItem *item = g_menu_item_new(label, action);

    if (accel != NULL) {
        g_menu_item_set_attribute(item, "accel", "s", accel);
    }
    g_menu_append_item(menu, item);
    g_object_unref(item);
}

static void
menu_add_radio(GMenu *menu, const char *label, const char *action, const char *target, const char *accel) {
    GMenuItem *item = g_menu_item_new(label, NULL);

    g_menu_item_set_action_and_target(item, action, "s", target);
    if (accel != NULL) {
        g_menu_item_set_attribute(item, "accel", "s", accel);
    }
    g_menu_append_item(menu, item);
    g_object_unref(item);
}

static void
menu_add_section(GMenu *menu, GMenu *section) {
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
}

static GMenuModel *
build_main_menu(void) {
    GMenu *menu = g_menu_new();
    GMenu *section, *sort;

    section = g_menu_new();
    menu_add(section, "New Window", "win.new-window", "<Control>n");
    menu_add(section, "Search", "win.search", "<Control>f");
    menu_add_section(menu, section);

    section = g_menu_new();
    menu_add_radio(section, "Grid", "win.view", "grid", "<Control>1");
    menu_add_radio(section, "List", "win.view", "list", "<Control>2");
    menu_add_section(menu, section);

    sort = g_menu_new();
    section = g_menu_new();
    menu_add_radio(section, "Name", "win.sort-by", "name", NULL);
    menu_add_radio(section, "Size", "win.sort-by", "size", NULL);
    menu_add_radio(section, "Type", "win.sort-by", "type", NULL);
    menu_add_radio(section, "Modified", "win.sort-by", "modified", NULL);
    menu_add_section(sort, section);
    section = g_menu_new();
    menu_add(section, "Reverse Order", "win.sort-reverse", NULL);
    menu_add_section(sort, section);
    section = g_menu_new();
    g_menu_append_submenu(section, "Sort", G_MENU_MODEL(sort));
    g_object_unref(sort);
    menu_add(section, "Show Hidden Files", "win.show-hidden", "<Control>h");
    menu_add(section, "Show Sidebar", "win.show-sidebar", "F9");
    menu_add_section(menu, section);

    section = g_menu_new();
    menu_add(section, "Zoom In", "win.zoom-in", "<Control>plus");
    menu_add(section, "Zoom Out", "win.zoom-out", "<Control>minus");
    menu_add(section, "Reset Zoom", "win.zoom-reset", "<Control>0");
    menu_add_section(menu, section);

    section = g_menu_new();
    menu_add(section, "About Howl", "win.about", NULL);
    menu_add_section(menu, section);

    return G_MENU_MODEL(menu);
}

static GMenu *
build_context_menu(HowlWindow *win, gboolean on_item) {
    GMenu *menu = g_menu_new();
    GMenu *section;

    if (on_item) {
        GList *sel = howl_window_get_selection(win);
        guint n = g_list_length(sel);
        HowlFile *one = n == 1 ? sel->data : NULL;
        gboolean any_file = FALSE;

        for (GList *l = sel; l != NULL; l = l->next) {
            if (!howl_file_is_directory(l->data)) {
                any_file = TRUE;
            }
        }

        section = g_menu_new();
        menu_add(section, "Open", "win.open", NULL);
        if (one != NULL && howl_file_is_directory(one)) {
            menu_add(section, "Open in New Window", "win.open-new-window", NULL);
        }
        if (any_file) {
            GList *files = selected_regular_files(win);
            GMenuModel *open_with = howl_ops_open_with_menu(files);

            g_menu_append_submenu(section, "Open With", open_with);
            g_object_unref(open_with);
            g_list_free_full(files, g_object_unref);
        }
        if (one != NULL && howl_file_is_executable(one)) {
            menu_add(section, "Run", "win.run", NULL);
        }
        if (one != NULL && howl_file_is_directory(one) && howl_ops_have_terminal()) {
            menu_add(section, "Open in Terminal", "win.open-terminal", NULL);
        }
        menu_add_section(menu, section);

        section = g_menu_new();
        menu_add(section, "Cut", "win.cut", NULL);
        menu_add(section, "Copy", "win.copy", NULL);
        menu_add_section(menu, section);

        section = g_menu_new();
        if (one != NULL) {
            menu_add(section, "Rename\342\200\246", "win.rename", NULL);
        }
        menu_add(section, "Move to Trash", "win.trash", NULL);
        menu_add(section, "Delete Permanently", "win.delete", NULL);
        menu_add_section(menu, section);

        g_list_free_full(sel, g_object_unref);
    } else {
        section = g_menu_new();
        menu_add(section, "New Folder\342\200\246", "win.new-folder", NULL);
        menu_add(section, "Paste", "win.paste", NULL);
        if (howl_ops_have_terminal()) {
            menu_add(section, "Open in Terminal", "win.open-terminal", NULL);
        }
        menu_add_section(menu, section);

        section = g_menu_new();
        menu_add(section, "Select All", "win.select-all", NULL);
        menu_add(section, "Show Hidden Files", "win.show-hidden", NULL);
        menu_add(section, "Reload", "win.reload", NULL);
        menu_add_section(menu, section);
    }
    return menu;
}

static void
show_context_menu(HowlWindow *win, gboolean on_item, double x, double y) {
    GMenu *menu = build_context_menu(win, on_item);
    GdkRectangle rect = {(int)x, (int)y, 1, 1};

    win->menu_x = x;
    win->menu_y = y;
    win->menu_time = g_get_monotonic_time();
    update_actions(win);
    gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(win->context_menu), G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_popover_set_pointing_to(GTK_POPOVER(win->context_menu), &rect);
    gtk_popover_popup(GTK_POPOVER(win->context_menu));
}

static void
selection_anchor(HowlWindow *win, GdkRectangle *rect) {
    GList *sel = howl_window_get_selection(win);
    GtkWidget *root = sel != NULL ? g_hash_table_lookup(win->item_widgets, sel->data) : NULL;
    graphene_rect_t bounds;

    if (root != NULL && gtk_widget_compute_bounds(root, win->content_box, &bounds)) {
        rect->x = (int)bounds.origin.x;
        rect->y = (int)bounds.origin.y;
        rect->width = (int)bounds.size.width;
        rect->height = (int)bounds.size.height;
    } else {
        rect->x = gtk_widget_get_width(win->content_box) / 2;
        rect->y = gtk_widget_get_height(win->content_box) / 2;
        rect->width = 1;
        rect->height = 1;
    }
    g_list_free_full(sel, g_object_unref);
}

static void
set_enabled(HowlWindow *win, const char *name, gboolean enabled) {
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(win->actions), name);

    if (action != NULL) {
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
    }
}

static void
update_actions(HowlWindow *win) {
    GList *sel;
    guint n;
    HowlFile *one;
    gboolean any_file = FALSE;
    GFile *parent;

    if (win->closing) {
        return;
    }
    sel = howl_window_get_selection(win);
    n = g_list_length(sel);
    one = n == 1 ? sel->data : NULL;
    parent = win->location != NULL ? g_file_get_parent(win->location) : NULL;
    for (GList *l = sel; l != NULL; l = l->next) {
        if (!howl_file_is_directory(l->data)) {
            any_file = TRUE;
        }
    }

    set_enabled(win, "back", win->back != NULL);
    set_enabled(win, "forward", win->forward != NULL);
    set_enabled(win, "up", parent != NULL);
    set_enabled(win, "open", n > 0);
    set_enabled(win, "open-with", any_file);
    set_enabled(win, "open-with-dialog", any_file);
    set_enabled(win, "open-new-window", one != NULL && howl_file_is_directory(one));
    set_enabled(win, "run", one != NULL && howl_file_is_executable(one));
    set_enabled(win, "open-terminal", howl_ops_have_terminal());
    set_enabled(win, "rename", one != NULL);
    set_enabled(win, "trash", n > 0);
    set_enabled(win, "delete", n > 0);
    set_enabled(win, "copy", n > 0);
    set_enabled(win, "cut", n > 0);

    g_clear_object(&parent);
    g_list_free_full(sel, g_object_unref);
}

static void
act_back(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GFile *target;

    if (win->back == NULL) {
        return;
    }
    target = win->back->data;
    win->back = g_list_delete_link(win->back, win->back);
    win->forward = g_list_prepend(win->forward, g_object_ref(win->location));
    go(win, target, FALSE);
    g_object_unref(target);
}

static void
act_forward(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GFile *target;

    if (win->forward == NULL) {
        return;
    }
    target = win->forward->data;
    win->forward = g_list_delete_link(win->forward, win->forward);
    win->back = g_list_prepend(win->back, g_object_ref(win->location));
    go(win, target, FALSE);
    g_object_unref(target);
}

static void
act_up(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GFile *parent = g_file_get_parent(win->location);
    char *name;

    if (parent == NULL) {
        return;
    }
    name = g_file_get_basename(win->location);
    go(win, parent, TRUE);
    howl_window_select_name(win, name);
    g_free(name);
    g_object_unref(parent);
}

static void
act_home(GSimpleAction *action, GVariant *param, gpointer data) {
    GFile *home = g_file_new_for_path(g_get_home_dir());

    howl_window_navigate(data, home);
    g_object_unref(home);
}

static void
act_reload(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;

    howl_dir_reload(win->dir);
}

static void
show_location_entry(HowlWindow *win) {
    char *path = win->location != NULL ? g_file_get_path(win->location) : NULL;

    gtk_editable_set_text(GTK_EDITABLE(win->location_entry), path != NULL ? path : "");
    gtk_stack_set_visible_child_name(GTK_STACK(win->title_stack), "entry");
    gtk_widget_grab_focus(win->location_entry);
    gtk_editable_set_position(GTK_EDITABLE(win->location_entry), -1);
    g_free(path);
}

static void
hide_location_entry(HowlWindow *win) {
    gboolean had_focus;

    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(win->title_stack)), "path") == 0) {
        return;
    }
    had_focus = gtk_widget_has_focus(win->location_entry) ||
        (gtk_window_get_focus(win->window) != NULL &&
         gtk_widget_is_ancestor(gtk_window_get_focus(win->window), win->location_entry));
    gtk_stack_set_visible_child_name(GTK_STACK(win->title_stack), "path");
    if (had_focus) {
        gtk_widget_grab_focus(current_view(win));
    }
}

static void
act_location(GSimpleAction *action, GVariant *param, gpointer data) {
    show_location_entry(data);
}

static void
change_search(GSimpleAction *action, GVariant *value, gpointer data) {
    HowlWindow *win = data;
    gboolean on = g_variant_get_boolean(value);

    g_simple_action_set_state(action, value);
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(win->search_bar), on);
    if (on) {
        gtk_widget_grab_focus(win->search_entry);
    }
}

static void
change_show_hidden(GSimpleAction *action, GVariant *value, gpointer data) {
    HowlWindow *win = data;

    win->show_hidden = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);
    howl_opts.show_hidden = win->show_hidden;
    howl_settings_save();
    gtk_filter_changed(GTK_FILTER(win->hidden_filter),
                       win->show_hidden ? GTK_FILTER_CHANGE_LESS_STRICT : GTK_FILTER_CHANGE_MORE_STRICT);
    update_status(win);
}

static void
change_show_sidebar(GSimpleAction *action, GVariant *value, gpointer data) {
    HowlWindow *win = data;

    g_simple_action_set_state(action, value);
    gtk_widget_set_visible(win->places, g_variant_get_boolean(value));
    howl_opts.show_sidebar = g_variant_get_boolean(value);
    howl_settings_save();
}

static void
change_view(GSimpleAction *action, GVariant *value, gpointer data) {
    set_view(data, g_str_equal(g_variant_get_string(value, NULL), "grid"));
}

static GtkColumnViewColumn *
column_for_name(HowlWindow *win, const char *name) {
    if (g_str_equal(name, "size")) {
        return win->col_size;
    }
    if (g_str_equal(name, "type")) {
        return win->col_type;
    }
    if (g_str_equal(name, "modified")) {
        return win->col_modified;
    }
    return win->col_name;
}

static void
change_sort_by(GSimpleAction *action, GVariant *value, gpointer data) {
    HowlWindow *win = data;
    GtkColumnViewSorter *cvs = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(win->column_view)));

    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(win->column_view),
                                   column_for_name(win, g_variant_get_string(value, NULL)),
                                   gtk_column_view_sorter_get_primary_sort_order(cvs));
}

static void
change_sort_reverse(GSimpleAction *action, GVariant *value, gpointer data) {
    HowlWindow *win = data;
    GtkColumnViewSorter *cvs = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(win->column_view)));
    GtkColumnViewColumn *column = gtk_column_view_sorter_get_primary_sort_column(cvs);

    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(win->column_view),
                                   column != NULL ? column : win->col_name,
                                   g_variant_get_boolean(value) ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
}

static void
act_zoom_in(GSimpleAction *action, GVariant *param, gpointer data) {
    zoom(data, 1);
}

static void
act_zoom_out(GSimpleAction *action, GVariant *param, gpointer data) {
    zoom(data, -1);
}

static void
act_zoom_reset(GSimpleAction *action, GVariant *param, gpointer data) {
    zoom(data, 0);
}

static void
act_new_window(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;

    howl_window_new(win->location, NULL);
}

static void
act_close(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;

    gtk_window_destroy(win->window);
}

static void
act_quit(GSimpleAction *action, GVariant *param, gpointer data) {
    GListModel *toplevels = gtk_window_get_toplevels();
    GList *windows = NULL;
    guint n = g_list_model_get_n_items(toplevels);

    for (guint i = 0; i < n; i++) {
        windows = g_list_prepend(windows, g_list_model_get_item(toplevels, i));
    }
    for (GList *l = windows; l != NULL; l = l->next) {
        if (g_object_get_data(l->data, "howl-window") != NULL) {
            gtk_window_destroy(l->data);
        }
    }
    g_list_free_full(windows, g_object_unref);
}

static void
act_open(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);

    if (sel != NULL) {
        howl_ops_open(win, sel);
    }
    g_list_free_full(sel, g_object_unref);
}

static void
act_open_with(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *files = selected_regular_files(win);

    if (files != NULL) {
        howl_ops_open_with_id(win, files, g_variant_get_string(param, NULL));
    }
    g_list_free_full(files, g_object_unref);
}

static void
act_open_with_dialog(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *files = selected_regular_files(win);

    if (files != NULL) {
        howl_ops_open_with_dialog(win, files);
    }
    g_list_free_full(files, g_object_unref);
}

static void
act_open_new_window(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);

    for (GList *l = sel; l != NULL; l = l->next) {
        if (howl_file_is_directory(l->data)) {
            howl_window_new(howl_file_get_file(l->data), NULL);
        }
    }
    g_list_free_full(sel, g_object_unref);
}

static void
act_run(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);

    if (sel != NULL && sel->next == NULL && howl_file_is_executable(sel->data)) {
        howl_ops_run(win, sel->data);
    }
    g_list_free_full(sel, g_object_unref);
}

static void
act_open_terminal(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);
    GFile *dir = win->location;

    if (sel != NULL && sel->next == NULL && howl_file_is_directory(sel->data)) {
        dir = howl_file_get_file(sel->data);
    }
    if (dir != NULL) {
        howl_ops_open_terminal(win, dir);
    }
    g_list_free_full(sel, g_object_unref);
}

static void
act_new_folder(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GdkRectangle rect;

    if (win->location == NULL) {
        return;
    }
    if (g_get_monotonic_time() - win->menu_time < 2 * G_USEC_PER_SEC) {
        rect.x = (int)win->menu_x;
        rect.y = (int)win->menu_y;
        rect.width = rect.height = 1;
    } else {
        rect.x = gtk_widget_get_width(win->content_box) / 2;
        rect.y = gtk_widget_get_height(win->content_box) / 3;
        rect.width = rect.height = 1;
    }
    howl_ops_new_folder(win, win->location, win->content_box, &rect);
}

static void
act_rename(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);
    GdkRectangle rect;

    if (sel != NULL && sel->next == NULL) {
        selection_anchor(win, &rect);
        howl_ops_rename(win, sel->data, win->content_box, &rect);
    }
    g_list_free_full(sel, g_object_unref);
}

static void
act_trash(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);

    if (sel != NULL) {
        howl_ops_trash(win, sel);
    }
    g_list_free_full(sel, g_object_unref);
}

static void
act_delete(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);

    if (sel != NULL) {
        howl_ops_delete(win, sel);
    }
    g_list_free_full(sel, g_object_unref);
}

static void
act_copy(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);

    howl_ops_copy(win, sel, FALSE);
    g_list_free_full(sel, g_object_unref);
}

static void
act_cut(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GList *sel = howl_window_get_selection(win);

    howl_ops_copy(win, sel, TRUE);
    g_list_free_full(sel, g_object_unref);
}

static void
act_paste(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;

    if (win->location != NULL) {
        howl_ops_paste(win, win->location);
    }
}

static void
act_select_all(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;

    gtk_selection_model_select_all(GTK_SELECTION_MODEL(win->selection));
}

static void
act_context_menu(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;
    GdkRectangle rect;
    gboolean on_item = selection_count(win) > 0;

    selection_anchor(win, &rect);
    show_context_menu(win, on_item, rect.x + rect.width / 2, rect.y + rect.height);
}

static void
act_about(GSimpleAction *action, GVariant *param, gpointer data) {
    HowlWindow *win = data;

    gtk_show_about_dialog(win->window,
                          "program-name", "Howl",
                          "version", HOWL_VERSION,
                          "comments", "A small file manager.",
                          "website", "https://github.com/dancingmirrors/howl",
                          "license-type", GTK_LICENSE_MIT_X11,
                          "logo-icon-name", "system-file-manager",
                          NULL);
}

static const GActionEntry action_entries[] = {
    {"back", act_back, NULL, NULL, NULL, {0}},
    {"forward", act_forward, NULL, NULL, NULL, {0}},
    {"up", act_up, NULL, NULL, NULL, {0}},
    {"home", act_home, NULL, NULL, NULL, {0}},
    {"reload", act_reload, NULL, NULL, NULL, {0}},
    {"location", act_location, NULL, NULL, NULL, {0}},
    {"search", NULL, NULL, "false", change_search, {0}},
    {"show-hidden", NULL, NULL, "true", change_show_hidden, {0}},
    {"show-sidebar", NULL, NULL, "true", change_show_sidebar, {0}},
    {"view", NULL, "s", "'grid'", change_view, {0}},
    {"sort-by", NULL, "s", "'name'", change_sort_by, {0}},
    {"sort-reverse", NULL, NULL, "false", change_sort_reverse, {0}},
    {"zoom-in", act_zoom_in, NULL, NULL, NULL, {0}},
    {"zoom-out", act_zoom_out, NULL, NULL, NULL, {0}},
    {"zoom-reset", act_zoom_reset, NULL, NULL, NULL, {0}},
    {"new-window", act_new_window, NULL, NULL, NULL, {0}},
    {"close", act_close, NULL, NULL, NULL, {0}},
    {"quit", act_quit, NULL, NULL, NULL, {0}},
    {"open", act_open, NULL, NULL, NULL, {0}},
    {"open-with", act_open_with, "s", NULL, NULL, {0}},
    {"open-with-dialog", act_open_with_dialog, NULL, NULL, NULL, {0}},
    {"open-new-window", act_open_new_window, NULL, NULL, NULL, {0}},
    {"run", act_run, NULL, NULL, NULL, {0}},
    {"open-terminal", act_open_terminal, NULL, NULL, NULL, {0}},
    {"new-folder", act_new_folder, NULL, NULL, NULL, {0}},
    {"rename", act_rename, NULL, NULL, NULL, {0}},
    {"trash", act_trash, NULL, NULL, NULL, {0}},
    {"delete", act_delete, NULL, NULL, NULL, {0}},
    {"copy", act_copy, NULL, NULL, NULL, {0}},
    {"cut", act_cut, NULL, NULL, NULL, {0}},
    {"paste", act_paste, NULL, NULL, NULL, {0}},
    {"select-all", act_select_all, NULL, NULL, NULL, {0}},
    {"context-menu", act_context_menu, NULL, NULL, NULL, {0}},
    {"about", act_about, NULL, NULL, NULL, {0}},
};

static const struct {
    const char *accel;
    const char *action;
    const char *arg;
} shortcuts[] = {
    {"<Alt>Left", "win.back", NULL},
    {"<Alt>Right", "win.forward", NULL},
    {"BackSpace", "win.up", NULL},
    {"<Alt>Up", "win.up", NULL},
    {"<Alt>Home", "win.home", NULL},
    {"<Control>r", "win.reload", NULL},
    {"F5", "win.reload", NULL},
    {"<Control>l", "win.location", NULL},
    {"<Control>f", "win.search", NULL},
    {"<Control>h", "win.show-hidden", NULL},
    {"F9", "win.show-sidebar", NULL},
    {"<Control>1", "win.view", "grid"},
    {"<Control>2", "win.view", "list"},
    {"<Control>KP_Add", "win.zoom-in", NULL},
    {"<Control>equal", "win.zoom-in", NULL},
    {"<Control>plus", "win.zoom-in", NULL},
    {"<Control>KP_Subtract", "win.zoom-out", NULL},
    {"<Control>minus", "win.zoom-out", NULL},
    {"<Control>KP_0", "win.zoom-reset", NULL},
    {"<Control>0", "win.zoom-reset", NULL},
    {"<Control>n", "win.new-window", NULL},
    {"<Control>w", "win.close", NULL},
    {"<Control>q", "win.quit", NULL},
    {"<Control><Shift>n", "win.new-folder", NULL},
    {"F2", "win.rename", NULL},
    {"KP_Delete", "win.trash", NULL},
    {"Delete", "win.trash", NULL},
    {"<Shift>KP_Delete", "win.delete", NULL},
    {"<Shift>Delete", "win.delete", NULL},
    {"<Control>c", "win.copy", NULL},
    {"<Control>x", "win.cut", NULL},
    {"<Control>v", "win.paste", NULL},
    {"<Control>a", "win.select-all", NULL},
    {"<Control>o", "win.open", NULL},
    {"<Shift>F10", "win.context-menu", NULL},
    {"Menu", "win.context-menu", NULL},
};

static void
mouse_nav_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    HowlWindow *win = data;

    if (button == 8) {
        g_action_group_activate_action(G_ACTION_GROUP(win->actions), "back", NULL);
    } else if (button == 9) {
        g_action_group_activate_action(G_ACTION_GROUP(win->actions), "forward", NULL);
    }
}

static void
build_actions(HowlWindow *win) {
    GtkEventController *controller;
    GtkGesture *gesture;
    GAction *action;

    win->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(win->actions), action_entries,
                                    G_N_ELEMENTS(action_entries), win);
    gtk_widget_insert_action_group(GTK_WIDGET(win->window), "win", G_ACTION_GROUP(win->actions));

    action = g_action_map_lookup_action(G_ACTION_MAP(win->actions), "show-hidden");
    g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(win->show_hidden));

    controller = gtk_shortcut_controller_new();
    for (gsize i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        GtkShortcutTrigger *trigger = gtk_shortcut_trigger_parse_string(shortcuts[i].accel);
        GtkShortcutAction *sa = gtk_named_action_new(shortcuts[i].action);
        GtkShortcut *shortcut;

        if (shortcuts[i].arg != NULL) {
            shortcut = gtk_shortcut_new_with_arguments(trigger, sa, "s", shortcuts[i].arg);
        } else {
            shortcut = gtk_shortcut_new(trigger, sa);
        }
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut);
    }
    gtk_widget_add_controller(GTK_WIDGET(win->window), controller);

    gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
    g_signal_connect(gesture, "pressed", G_CALLBACK(mouse_nav_pressed), win);
    gtk_widget_add_controller(GTK_WIDGET(win->window), GTK_EVENT_CONTROLLER(gesture));
}

static void
pathbar_button_clicked(GtkButton *button, gpointer data) {
    HowlWindow *win = data;
    GFile *file = g_object_get_data(G_OBJECT(button), "file");

    howl_window_navigate(win, file);
}

static GFile *
pathbar_drop_locate(GtkWidget *widget, double x, double y, GtkWidget **highlight, gpointer data) {
    GFile *file = g_object_get_data(G_OBJECT(widget), "file");

    if (file == NULL) {
        return NULL;
    }
    *highlight = widget;
    return g_object_ref(file);
}

static void
pathbar_scroll_to_end(GtkAdjustment *adjustment, GParamSpec *pspec, gpointer data) {
    gtk_adjustment_set_value(adjustment,
                             gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
}

static void
update_pathbar(HowlWindow *win) {
    GtkWidget *child;
    GFile *home = g_file_new_for_path(g_get_home_dir());
    GSList *chain = NULL, *l;
    GFile *f;

    while ((child = gtk_widget_get_first_child(win->pathbar_box)) != NULL) {
        gtk_box_remove(GTK_BOX(win->pathbar_box), child);
    }

    for (f = g_object_ref(win->location); f != NULL;) {
        GFile *parent;

        chain = g_slist_prepend(chain, f);
        if (g_file_equal(f, home)) {
            break;
        }
        parent = g_file_get_parent(f);
        f = parent;
    }

    for (l = chain; l != NULL; l = l->next) {
        GFile *file = l->data;
        GtkWidget *button = gtk_button_new();
        GtkWidget *content;
        GFile *parent = g_file_get_parent(file);

        if (l != chain) {
            GtkWidget *sep = gtk_image_new_from_icon_name("pan-end-symbolic");

            gtk_widget_add_css_class(sep, "howl-pathbar-separator");
            gtk_box_append(GTK_BOX(win->pathbar_box), sep);
        }

        if (g_file_equal(file, home)) {
            content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name("user-home-symbolic"));
            gtk_box_append(GTK_BOX(content), gtk_label_new("Home"));
        } else if (parent == NULL) {
            content = gtk_image_new_from_icon_name("drive-harddisk-symbolic");
            gtk_widget_set_tooltip_text(button, "File System");
        } else {
            char *name = g_file_get_basename(file);
            char *display = g_filename_display_name(name);

            content = gtk_label_new(display);
            gtk_label_set_single_line_mode(GTK_LABEL(content), TRUE);
            g_free(display);
            g_free(name);
        }

        gtk_button_set_child(GTK_BUTTON(button), content);
        gtk_widget_add_css_class(button, "flat");
        if (l->next == NULL) {
            gtk_widget_add_css_class(button, "howl-pathbar-current");
        }
        g_object_set_data_full(G_OBJECT(button), "file", g_object_ref(file), g_object_unref);
        g_signal_connect(button, "clicked", G_CALLBACK(pathbar_button_clicked), win);
        howl_dnd_add_drop_site(win, button, pathbar_drop_locate, win);
        gtk_box_append(GTK_BOX(win->pathbar_box), button);
        g_clear_object(&parent);
    }

    g_slist_free_full(chain, g_object_unref);
    g_object_unref(home);
}

static GFile *
parse_location(HowlWindow *win, const char *text) {
    char *t = g_strstrip(g_strdup(text));
    GFile *file = NULL;

    if (*t == '\0') {
        g_free(t);
        return NULL;
    }
    if (t[0] == '~' && (t[1] == '\0' || t[1] == '/')) {
        char *path = g_build_filename(g_get_home_dir(), t + 1, NULL);

        file = g_file_new_for_path(path);
        g_free(path);
    } else {
        char *cwd = win->location != NULL ? g_file_get_path(win->location) : NULL;

        file = g_file_new_for_commandline_arg_and_cwd(t, cwd != NULL ? cwd : g_get_home_dir());
        g_free(cwd);
    }
    g_free(t);
    return file;
}

static void
location_activate_cb(GtkEntry *entry, gpointer data) {
    HowlWindow *win = data;
    GFile *file = parse_location(win, gtk_editable_get_text(GTK_EDITABLE(entry)));
    GFileType type;

    if (file == NULL) {
        return;
    }

    type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);
    if (type == G_FILE_TYPE_DIRECTORY) {
        hide_location_entry(win);
        howl_window_navigate(win, file);
    } else if (type == G_FILE_TYPE_UNKNOWN) {
        char *path = g_file_get_parse_name(file);
        char *detail = g_strdup_printf("\342\200\234%s\342\200\235 does not exist.", path);

        howl_window_show_error(win, "Location not found", detail);
        g_free(detail);
        g_free(path);
    } else {
        GFile *parent = g_file_get_parent(file);
        char *name = g_file_get_basename(file);

        hide_location_entry(win);
        if (parent != NULL) {
            howl_window_navigate(win, parent);
            howl_window_select_name(win, name);
            g_object_unref(parent);
        }
        g_free(name);
    }
    g_object_unref(file);
}

static gboolean
location_key_cb(GtkEventControllerKey *controller, guint keyval, guint keycode,
                GdkModifierType state, gpointer data) {
    if (keyval == GDK_KEY_Escape) {
        hide_location_entry(data);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void
location_focus_leave_cb(GtkEventControllerFocus *controller, gpointer data) {
    HowlWindow *win = data;

    if (gtk_window_is_active(win->window)) {
        hide_location_entry(win);
    }
}

static GtkWidget *
window_controls(GtkPackType side) {
    GtkWidget *controls = gtk_window_controls_new(side);

    g_object_bind_property(controls, "empty", controls, "visible",
                           G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);
    return controls;
}

static void
build_header(HowlWindow *win) {
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *button, *menu_button;
    GtkEventController *controller;
    GMenuModel *menu;

    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    gtk_widget_add_css_class(header, "howl-header");
    gtk_box_append(GTK_BOX(bar), window_controls(GTK_PACK_START));

    gtk_widget_add_css_class(nav, "linked");
    button = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_set_tooltip_text(button, "Back");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.back");
    gtk_box_append(GTK_BOX(nav), button);
    button = gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_set_tooltip_text(button, "Forward");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.forward");
    gtk_box_append(GTK_BOX(nav), button);
    gtk_box_append(GTK_BOX(bar), nav);

    menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
    gtk_widget_set_tooltip_text(menu_button, "Menu");
    menu = build_main_menu();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), menu);
    gtk_widget_set_halign(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu_button))), GTK_ALIGN_START);
    g_object_unref(menu);
    gtk_box_append(GTK_BOX(bar), menu_button);

    win->title_stack = gtk_stack_new();
    gtk_widget_set_hexpand(win->title_stack, TRUE);
    gtk_stack_set_hhomogeneous(GTK_STACK(win->title_stack), FALSE);

    win->pathbar_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(win->pathbar_scroller),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_widget_set_hexpand(win->pathbar_scroller, TRUE);
    win->pathbar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(win->pathbar_box, "howl-pathbar");
    gtk_widget_set_valign(win->pathbar_box, GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(win->pathbar_scroller), win->pathbar_box);
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(win->pathbar_scroller)),
                     "notify::upper", G_CALLBACK(pathbar_scroll_to_end), NULL);
    gtk_stack_add_named(GTK_STACK(win->title_stack), win->pathbar_scroller, "path");

    win->location_entry = gtk_entry_new();
    gtk_widget_set_hexpand(win->location_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->location_entry), "Location");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(win->location_entry), GTK_ENTRY_ICON_PRIMARY, "folder-symbolic");
    g_signal_connect(win->location_entry, "activate", G_CALLBACK(location_activate_cb), win);
    controller = gtk_event_controller_key_new();
    g_signal_connect(controller, "key-pressed", G_CALLBACK(location_key_cb), win);
    gtk_widget_add_controller(win->location_entry, controller);
    controller = gtk_event_controller_focus_new();
    g_signal_connect(controller, "leave", G_CALLBACK(location_focus_leave_cb), win);
    gtk_widget_add_controller(win->location_entry, controller);
    gtk_stack_add_named(GTK_STACK(win->title_stack), win->location_entry, "entry");
    gtk_box_append(GTK_BOX(bar), win->title_stack);

    gtk_box_append(GTK_BOX(bar), window_controls(GTK_PACK_END));
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), bar);
    gtk_window_set_titlebar(win->window, header);
}

static void
search_changed_cb(GtkSearchEntry *entry, gpointer data) {
    HowlWindow *win = data;

    if (win->closing) {
        return;
    }

    gtk_string_filter_set_search(win->search_filter, gtk_editable_get_text(GTK_EDITABLE(entry)));
    update_page(win);
    update_status(win);
}

static void
search_mode_cb(GObject *bar, GParamSpec *pspec, gpointer data) {
    HowlWindow *win = data;
    gboolean on = gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar));
    GAction *action;

    if (win->closing) {
        return;
    }

    action = g_action_map_lookup_action(G_ACTION_MAP(win->actions), "search");
    g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(on));
    if (!on) {
        gtk_editable_set_text(GTK_EDITABLE(win->search_entry), "");
        if (gtk_widget_get_mapped(GTK_WIDGET(win->window))) {
            gtk_widget_grab_focus(current_view(win));
        }
    }
}

static GtkWidget *
scrolled(GtkWidget *child) {
    GtkWidget *sw = gtk_scrolled_window_new();

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

static GtkWidget *
message_page(GtkWidget **label_out, const char *css_class) {
    GtkWidget *label = gtk_label_new(NULL);

    gtk_widget_add_css_class(label, css_class);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(label, 24);
    gtk_widget_set_margin_end(label, 24);
    *label_out = label;
    return label;
}

static GtkListItem *
find_list_item(GtkWidget *widget) {
    GtkListItem *item = g_object_get_data(G_OBJECT(widget), "list-item");

    for (GtkWidget *child = gtk_widget_get_first_child(widget); item == NULL && child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        item = find_list_item(child);
    }
    return item;
}

static GFile *
content_drop_locate(GtkWidget *widget, double x, double y, GtkWidget **highlight, gpointer data) {
    HowlWindow *win = data;
    GtkWidget *picked, *container = NULL;
    GtkListItem *item = NULL;

    if (win->closing || win->location == NULL || howl_dir_get_error(win->dir) != NULL) {
        return NULL;
    }

    picked = gtk_widget_pick(widget, x, y, GTK_PICK_DEFAULT);
    for (GtkWidget *w = picked; w != NULL && w != widget && container == NULL; w = gtk_widget_get_parent(w)) {
        const char *name = gtk_widget_get_css_name(w);

        if (g_strcmp0(name, "row") == 0 || g_strcmp0(name, "child") == 0) {
            container = w;
        }
    }
    if (container != NULL) {
        item = find_list_item(container);
    }
    if (item != NULL) {
        HowlFile *file = gtk_list_item_get_item(item);

        if (file != NULL && howl_file_is_directory(file)) {
            *highlight = container;
            return g_object_ref(howl_file_get_file(file));
        }
    }
    *highlight = win->stack;
    return g_object_ref(win->location);
}

static void
build_body(HowlWindow *win) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *overlay, *page;

    win->search_bar = gtk_search_bar_new();
    win->search_entry = gtk_search_entry_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(win->search_bar), win->search_entry);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(win->search_bar), GTK_EDITABLE(win->search_entry));
    gtk_search_bar_set_key_capture_widget(GTK_SEARCH_BAR(win->search_bar), GTK_WIDGET(win->window));
    gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(win->search_bar), TRUE);
    g_signal_connect(win->search_entry, "search-changed", G_CALLBACK(search_changed_cb), win);
    g_signal_connect(win->search_bar, "notify::search-mode-enabled", G_CALLBACK(search_mode_cb), win);
    gtk_box_append(GTK_BOX(vbox), win->search_bar);

    win->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(win->paned), 200);
    gtk_paned_set_shrink_start_child(GTK_PANED(win->paned), FALSE);
    gtk_paned_set_resize_start_child(GTK_PANED(win->paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(win->paned), FALSE);
    gtk_widget_set_vexpand(win->paned, TRUE);

    win->places = howl_places_new(win);
    gtk_paned_set_start_child(GTK_PANED(win->paned), win->places);

    win->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    win->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(win->stack), scrolled(win->grid_view), "grid");
    gtk_stack_add_named(GTK_STACK(win->stack), scrolled(win->column_view), "list");
    page = message_page(&win->empty_label, "howl-empty");
    gtk_stack_add_named(GTK_STACK(win->stack), page, "empty");
    page = message_page(&win->error_label, "howl-message");
    gtk_stack_add_named(GTK_STACK(win->stack), page, "error");

    overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), win->stack);
    win->status_label = gtk_label_new("");
    gtk_widget_add_css_class(win->status_label, "howl-floating-bar");
    gtk_widget_set_halign(win->status_label, GTK_ALIGN_END);
    gtk_widget_set_valign(win->status_label, GTK_ALIGN_END);
    gtk_widget_set_can_target(win->status_label, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), win->status_label);
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_box_append(GTK_BOX(win->content_box), overlay);
    howl_dnd_add_drop_site(win, win->content_box, content_drop_locate, win);

    win->context_menu = gtk_popover_menu_new_from_model(NULL);
    gtk_popover_set_has_arrow(GTK_POPOVER(win->context_menu), FALSE);
    gtk_widget_set_halign(win->context_menu, GTK_ALIGN_START);
    gtk_widget_set_parent(win->context_menu, win->content_box);

    gtk_paned_set_end_child(GTK_PANED(win->paned), win->content_box);
    gtk_box_append(GTK_BOX(vbox), win->paned);
    gtk_window_set_child(win->window, vbox);
}

static void
update_title(HowlWindow *win) {
    GFile *home = g_file_new_for_path(g_get_home_dir());
    char *title;

    if (g_file_equal(win->location, home)) {
        title = g_strdup("Home");
    } else {
        char *name = g_file_get_basename(win->location);
        title = g_filename_display_name(name);
        g_free(name);
    }
    gtk_window_set_title(win->window, title);
    g_free(title);
    g_object_unref(home);
}

static void
go(HowlWindow *win, GFile *location, gboolean record) {
    if (record && win->location != NULL) {
        win->back = g_list_prepend(win->back, g_object_ref(win->location));
        g_list_free_full(win->forward, g_object_unref);
        win->forward = NULL;
    }

    g_set_object(&win->location, location);
    g_clear_pointer(&win->pending_select, g_free);
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(win->search_bar), FALSE);
    hide_location_entry(win);

    howl_dir_load(win->dir, location);
    update_pathbar(win);
    update_title(win);
    update_actions(win);
    howl_places_set_location(win->places, location);
    if (gtk_widget_get_mapped(GTK_WIDGET(win->window))) {
        gtk_widget_grab_focus(current_view(win));
    }
}

void howl_window_navigate(HowlWindow *win, GFile *location) {
    if (win->location != NULL && g_file_equal(win->location, location)) {
        return;
    }
    go(win, location, TRUE);
}

static void
window_map_cb(GtkWidget *widget, gpointer data) {
    g_debug("window mapped %.0f ms after start",
            (g_get_monotonic_time() - howl_start_time) / 1000.0);
}

static void
window_destroy_cb(GtkWidget *widget, gpointer data) {
    HowlWindow *win = data;

    win->closing = TRUE;
    g_object_set_data(G_OBJECT(widget), "howl-window", NULL);
    howl_dir_dispose(win->dir);
    g_signal_handlers_disconnect_by_data(win->selection, win);
    howl_app_window_closed();
}

static void
window_finalized_cb(gpointer data, GObject *where_the_object_was) {
    HowlWindow *win = data;

    howl_dir_unref(win->dir);
    g_clear_object(&win->selection);
    g_clear_object(&win->hidden_filter);
    g_clear_object(&win->search_filter);
    g_clear_object(&win->actions);
    g_clear_object(&win->col_name);
    g_clear_object(&win->col_size);
    g_clear_object(&win->col_type);
    g_clear_object(&win->col_modified);
    g_clear_object(&win->location);
    g_list_free_full(win->back, g_object_unref);
    g_list_free_full(win->forward, g_object_unref);
    g_hash_table_unref(win->item_widgets);
    g_free(win->pending_select);
    g_free(win->status_override);
    g_free(win);
}

HowlWindow *
howl_window_new(GFile *location, const char *select_name) {
    HowlWindow *win = g_new0(HowlWindow, 1);

    win->show_hidden = howl_opts.show_hidden;
    win->grid = !howl_opts.list_view;
    win->grid_size = howl_opts.grid_size;
    win->list_size = howl_opts.list_size;
    win->item_widgets = g_hash_table_new(NULL, NULL);

    win->window = GTK_WINDOW(gtk_window_new());
    g_object_set_data(G_OBJECT(win->window), "howl-window", win);
    g_object_weak_ref(G_OBJECT(win->window), window_finalized_cb, win);
    gtk_window_set_default_size(win->window, howl_opts.window_width, howl_opts.window_height);
    if (howl_opts.maximized) {
        gtk_window_maximize(win->window);
    }
    gtk_window_set_icon_name(win->window, "system-file-manager");

    build_models_and_views(win);
    build_actions(win);
    build_body(win);
    build_header(win);
    set_view(win, win->grid);
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(win->column_view), column_for_name(win, howl_opts.sort_by), howl_opts.sort_reverse ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    if (!howl_opts.show_sidebar) {
        g_action_group_change_action_state(G_ACTION_GROUP(win->actions), "show-sidebar", g_variant_new_boolean(FALSE));
    }
    g_signal_connect(win->window, "notify::default-width", G_CALLBACK(window_size_cb), win);
    g_signal_connect(win->window, "notify::default-height", G_CALLBACK(window_size_cb), win);
    g_signal_connect(win->window, "notify::maximized", G_CALLBACK(window_size_cb), win);

    g_signal_connect(win->window, "realize", G_CALLBACK(window_realize_cb), win);
    g_signal_connect(win->window, "unrealize", G_CALLBACK(window_unrealize_cb), win);
    g_signal_connect(win->window, "map", G_CALLBACK(window_map_cb), NULL);
    g_signal_connect(win->window, "destroy", G_CALLBACK(window_destroy_cb), win);
    howl_app_window_opened();

    go(win, location, FALSE);
    if (select_name != NULL) {
        howl_window_select_name(win, select_name);
    }

    gtk_window_present(win->window);
    gtk_widget_grab_focus(current_view(win));
    return win;
}
