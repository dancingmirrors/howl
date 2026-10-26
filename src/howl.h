// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#ifndef HOWL_H
#define HOWL_H

#define G_LOG_DOMAIN "howl"

#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>

#define HOWL_VERSION "0.1"

#define HOWL_THUMB_CACHE_BYTES (128u * 1024 * 1024)
#define HOWL_THUMB_IMAGE_LIMIT (64u * 1024 * 1024)
#define HOWL_THUMB_PIXEL_LIMIT (50 * 1000 * 1000)
#define HOWL_THUMB_WORKERS 4
#define HOWL_THUMBNAILER_TIMEOUT 20
#define HOWL_LOAD_BATCH 250
#define HOWL_RELOAD_DELAY_MS 300

typedef struct {
    gboolean show_hidden;
    gboolean thumbnails;
    gboolean list_view;
    gboolean show_sidebar;
    int grid_size;
    int list_size;
    const char *sort_by;
    gboolean sort_reverse;
    int window_width;
    int window_height;
    gboolean maximized;
} HowlOptions;

extern HowlOptions howl_opts;
extern gint64 howl_start_time;

void howl_app_window_opened(void);
void howl_app_window_closed(void);
void howl_app_hold(void);
void howl_app_release(void);

#define HOWL_TYPE_FILE (howl_file_get_type())
G_DECLARE_FINAL_TYPE(HowlFile, howl_file, HOWL, FILE, GObject)

typedef enum {
    HOWL_THUMB_NONE,
    HOWL_THUMB_PENDING,
    HOWL_THUMB_DONE,
    HOWL_THUMB_FAILED
} HowlThumbState;

HowlFile *howl_file_new(GFile *parent, GFileInfo *info);
void howl_file_update(HowlFile *self, GFileInfo *info);
GFile *howl_file_get_file(HowlFile *self);
GFileInfo *howl_file_get_info(HowlFile *self);
const char *howl_file_get_name(HowlFile *self);
const char *howl_file_get_display_name(HowlFile *self);
const char *howl_file_get_content_type(HowlFile *self);
const char *howl_file_get_type_description(HowlFile *self);
GIcon *howl_file_get_icon(HowlFile *self);
guint64 howl_file_get_size(HowlFile *self);
gint64 howl_file_get_mtime(HowlFile *self);
gboolean howl_file_is_directory(HowlFile *self);
gboolean howl_file_is_hidden(HowlFile *self);
gboolean howl_file_is_executable(HowlFile *self);
char *howl_file_format_size(HowlFile *self);
char *howl_file_format_mtime(HowlFile *self);
char *howl_file_describe(HowlFile *self);
GdkPaintable *howl_file_get_thumbnail(HowlFile *self, int size);
void howl_file_set_thumbnail(HowlFile *self, int size, GdkPaintable *paintable);
HowlThumbState howl_file_get_thumb_state(HowlFile *self, int size);
void howl_file_set_thumb_state(HowlFile *self, int size, HowlThumbState state);
void howl_file_release_thumbnails(HowlFile *self);

typedef struct _HowlDir HowlDir;
typedef void (*HowlDirFunc)(HowlDir *dir, gpointer user_data);

HowlDir *howl_dir_new(HowlDirFunc changed, gpointer user_data);
HowlDir *howl_dir_ref(HowlDir *dir);
void howl_dir_unref(HowlDir *dir);
void howl_dir_dispose(HowlDir *dir);
GListModel *howl_dir_get_model(HowlDir *dir);
void howl_dir_load(HowlDir *dir, GFile *location);
void howl_dir_reload(HowlDir *dir);
GFile *howl_dir_get_location(HowlDir *dir);
gboolean howl_dir_is_loading(HowlDir *dir);
const GError *howl_dir_get_error(HowlDir *dir);

void howl_thumbs_init(void);
void howl_thumbs_shutdown(void);
gboolean howl_thumbs_supported(const char *content_type);
void howl_thumbs_request(HowlFile *file, int size, int scale);
void howl_thumbs_cancel(HowlFile *file);

typedef struct _HowlWindow HowlWindow;

HowlWindow *howl_window_new(GFile *location, const char *select_name);
GtkWindow *howl_window_get_gtk(HowlWindow *win);
GFile *howl_window_get_location(HowlWindow *win);
void howl_window_navigate(HowlWindow *win, GFile *location);
GList *howl_window_get_selection(HowlWindow *win);
void howl_window_select_name(HowlWindow *win, const char *name);
void howl_window_set_status(HowlWindow *win, const char *text);
void howl_window_show_error(HowlWindow *win, const char *message, const char *detail);
void howl_window_refresh_cut(HowlWindow *win);

void howl_settings_load(void);
void howl_settings_baseline(void);
void howl_settings_save(void);
void howl_settings_flush(void);
const char *howl_settings_canonical_sort(const char *name);

GtkWidget *howl_places_new(HowlWindow *win);
void howl_places_set_location(GtkWidget *places, GFile *location);

void howl_ops_open(HowlWindow *win, GList *files);
void howl_ops_open_with_id(HowlWindow *win, GList *files, const char *desktop_id);
void howl_ops_open_with_dialog(HowlWindow *win, GList *files);
GMenuModel *howl_ops_open_with_menu(GList *files);
void howl_ops_run(HowlWindow *win, HowlFile *file);
gboolean howl_ops_have_terminal(void);
void howl_ops_open_terminal(HowlWindow *win, GFile *dir);
void howl_ops_new_folder(HowlWindow *win, GFile *parent, GtkWidget *anchor, const GdkRectangle *rect);
void howl_ops_rename(HowlWindow *win, HowlFile *file, GtkWidget *anchor, const GdkRectangle *rect);
void howl_ops_trash(HowlWindow *win, GList *files);
void howl_ops_trash_gfiles(HowlWindow *win, GList *gfiles);
void howl_ops_delete(HowlWindow *win, GList *files);
void howl_ops_copy(HowlWindow *win, GList *files, gboolean cut);
void howl_ops_paste(HowlWindow *win, GFile *target_dir);
void howl_ops_transfer(HowlWindow *win, GList *gfiles, GFile *target_dir, gboolean move);
void howl_ops_link(HowlWindow *win, GList *gfiles, GFile *target_dir);
gboolean howl_ops_is_cut(GFile *file);

GList *howl_files_to_gfiles(GList *files);

typedef GFile *(*HowlDropLocate)(GtkWidget *widget, double x, double y, GtkWidget **highlight, gpointer user_data);

void howl_dnd_add_drop_site(HowlWindow *win, GtkWidget *widget, HowlDropLocate locate, gpointer user_data);
GdkContentProvider *howl_dnd_content_for_files(GList *gfiles);
GtkWidget *howl_dnd_icon_new(GdkPaintable *paintable, GIcon *gicon, const char *name, guint count, int size);

#endif // HOWL_H
