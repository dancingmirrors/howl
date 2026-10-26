// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

// clang-format off
static const char attributes[] = G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                 G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
                                 G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                 G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                 G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
                                 G_FILE_ATTRIBUTE_STANDARD_ICON ","
                                 G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
                                 G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP ","
                                 G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","
                                 G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                 G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC ","
                                 G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE ","
                                 G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET ","
                                 G_FILE_ATTRIBUTE_UNIX_MODE ","
                                 G_FILE_ATTRIBUTE_OWNER_USER ","
                                 G_FILE_ATTRIBUTE_OWNER_GROUP;
// clang-format on

struct _HowlDir {
    int ref_count;
    gboolean disposed;

    GListStore *store;
    GFile *location;
    GFileMonitor *monitor;
    GCancellable *cancellable;
    guint generation;

    gboolean loading;
    gboolean merging;
    gboolean reload_pending;
    GPtrArray *incoming;
    GError *error;
    guint reload_source;

    HowlDirFunc changed;
    gpointer user_data;
};

typedef struct {
    HowlDir *dir;
    guint generation;
    GFileEnumerator *enumerator;
} Load;

static void start_load(HowlDir *dir, gboolean merging);

static void
emit_changed(HowlDir *dir) {
    if (!dir->disposed && dir->changed != NULL) {
        dir->changed(dir, dir->user_data);
    }
}

HowlDir *
howl_dir_new(HowlDirFunc changed, gpointer user_data) {
    HowlDir *dir = g_new0(HowlDir, 1);

    dir->ref_count = 1;
    dir->store = g_list_store_new(HOWL_TYPE_FILE);
    dir->changed = changed;
    dir->user_data = user_data;
    return dir;
}

HowlDir *
howl_dir_ref(HowlDir *dir) {
    dir->ref_count++;
    return dir;
}

void howl_dir_unref(HowlDir *dir) {
    if (--dir->ref_count > 0) {
        return;
    }

    howl_dir_dispose(dir);
    g_clear_object(&dir->store);
    g_clear_object(&dir->location);
    g_clear_object(&dir->cancellable);
    g_clear_error(&dir->error);
    g_clear_pointer(&dir->incoming, g_ptr_array_unref);
    g_free(dir);
}

static void
stop_monitor(HowlDir *dir) {
    if (dir->monitor != NULL) {
        g_signal_handlers_disconnect_by_data(dir->monitor, dir);
        g_file_monitor_cancel(dir->monitor);
        g_clear_object(&dir->monitor);
    }
    if (dir->reload_source != 0) {
        g_source_remove(dir->reload_source);
        dir->reload_source = 0;
    }
    dir->reload_pending = FALSE;
}

void howl_dir_dispose(HowlDir *dir) {
    if (dir->disposed) {
        return;
    }
    dir->disposed = TRUE;
    dir->changed = NULL;
    stop_monitor(dir);
    if (dir->cancellable != NULL) {
        g_cancellable_cancel(dir->cancellable);
    }
}

GListModel *
howl_dir_get_model(HowlDir *dir) {
    return G_LIST_MODEL(dir->store);
}

GFile *
howl_dir_get_location(HowlDir *dir) {
    return dir->location;
}

gboolean
howl_dir_is_loading(HowlDir *dir) {
    return dir->loading;
}

const GError *
howl_dir_get_error(HowlDir *dir) {
    return dir->error;
}

static void
load_free(Load *load) {
    g_clear_object(&load->enumerator);
    howl_dir_unref(load->dir);
    g_free(load);
}

static gboolean
load_is_stale(Load *load) {
    return load->dir->disposed || load->generation != load->dir->generation;
}

static void
merge_incoming(HowlDir *dir) {
    GHashTable *fresh = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *additions = g_ptr_array_new();
    guint i, n;

    for (i = 0; i < dir->incoming->len; i++) {
        HowlFile *file = g_ptr_array_index(dir->incoming, i);
        g_hash_table_insert(fresh, (gpointer)howl_file_get_name(file), file);
    }

    n = g_list_model_get_n_items(G_LIST_MODEL(dir->store));
    for (i = n; i > 0; i--) {
        HowlFile *old = g_list_model_get_item(G_LIST_MODEL(dir->store), i - 1);
        HowlFile *new = g_hash_table_lookup(fresh, howl_file_get_name(old));

        if (new != NULL) {
            howl_file_update(old, howl_file_get_info(new));
            g_hash_table_remove(fresh, howl_file_get_name(old));
        } else {
            g_list_store_remove(dir->store, i - 1);
        }
        g_object_unref(old);
    }

    for (i = 0; i < dir->incoming->len; i++) {
        HowlFile *file = g_ptr_array_index(dir->incoming, i);
        if (g_hash_table_contains(fresh, howl_file_get_name(file))) {
            g_ptr_array_add(additions, file);
        }
    }

    if (additions->len > 0) {
        g_list_store_splice(dir->store,
                            g_list_model_get_n_items(G_LIST_MODEL(dir->store)), 0,
                            additions->pdata, additions->len);
    }

    g_ptr_array_unref(additions);
    g_hash_table_unref(fresh);
}

static gboolean
reload_timeout(gpointer data) {
    HowlDir *dir = data;

    dir->reload_source = 0;
    if (dir->disposed || dir->location == NULL) {
        return G_SOURCE_REMOVE;
    }
    if (dir->loading) {
        dir->reload_pending = TRUE;
    } else {
        start_load(dir, TRUE);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_reload(HowlDir *dir) {
    if (dir->disposed) {
        return;
    }
    if (dir->loading) {
        dir->reload_pending = TRUE;
        return;
    }
    if (dir->reload_source == 0) {
        dir->reload_source = g_timeout_add(HOWL_RELOAD_DELAY_MS, reload_timeout, dir);
    }
}

static void
finish_load(HowlDir *dir, GError *error) {
    dir->loading = FALSE;

    if (error != NULL) {
        g_clear_error(&dir->error);
        dir->error = error;
        if (dir->merging) {
            g_list_store_remove_all(dir->store);
        }
    } else if (dir->merging) {
        merge_incoming(dir);
    }

    dir->merging = FALSE;
    g_clear_pointer(&dir->incoming, g_ptr_array_unref);
    emit_changed(dir);

    if (dir->reload_pending) {
        dir->reload_pending = FALSE;
        schedule_reload(dir);
    }
}

static void
next_files_cb(GObject *source, GAsyncResult *result, gpointer data) {
    Load *load = data;
    HowlDir *dir = load->dir;
    GError *error = NULL;
    GList *infos, *l;
    GPtrArray *files;

    infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(source), result, &error);

    if (load_is_stale(load)) {
        g_list_free_full(infos, g_object_unref);
        g_clear_error(&error);
        load_free(load);
        return;
    }

    if (error != NULL) {
        g_file_enumerator_close_async(load->enumerator, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        finish_load(dir, error);
        load_free(load);
        return;
    }

    if (infos == NULL) {
        g_file_enumerator_close_async(load->enumerator, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        finish_load(dir, NULL);
        load_free(load);
        return;
    }

    files = g_ptr_array_new_with_free_func(g_object_unref);
    for (l = infos; l != NULL; l = l->next) {
        HowlFile *file = howl_file_new(dir->location, l->data);

        if (dir->merging) {
            g_ptr_array_add(dir->incoming, file);
        } else {
            g_ptr_array_add(files, file);
        }
    }
    g_list_free_full(infos, g_object_unref);

    if (files->len > 0) {
        g_list_store_splice(dir->store,
                            g_list_model_get_n_items(G_LIST_MODEL(dir->store)), 0,
                            files->pdata, files->len);
        emit_changed(dir);
    }
    g_ptr_array_unref(files);

    g_file_enumerator_next_files_async(load->enumerator, HOWL_LOAD_BATCH, G_PRIORITY_DEFAULT,
                                       dir->cancellable, next_files_cb, load);
}

static void
enumerate_cb(GObject *source, GAsyncResult *result, gpointer data) {
    Load *load = data;
    GError *error = NULL;
    GFileEnumerator *enumerator;

    enumerator = g_file_enumerate_children_finish(G_FILE(source), result, &error);

    if (load_is_stale(load)) {
        g_clear_object(&enumerator);
        g_clear_error(&error);
        load_free(load);
        return;
    }

    if (enumerator == NULL) {
        finish_load(load->dir, error);
        load_free(load);
        return;
    }

    load->enumerator = enumerator;
    g_file_enumerator_next_files_async(enumerator, HOWL_LOAD_BATCH, G_PRIORITY_DEFAULT,
                                       load->dir->cancellable, next_files_cb, load);
}

static void
start_load(HowlDir *dir, gboolean merging) {
    Load *load;

    if (dir->cancellable != NULL) {
        g_cancellable_cancel(dir->cancellable);
        g_object_unref(dir->cancellable);
    }
    dir->cancellable = g_cancellable_new();
    dir->generation++;
    dir->loading = TRUE;
    dir->merging = merging;
    g_clear_error(&dir->error);
    g_clear_pointer(&dir->incoming, g_ptr_array_unref);

    if (merging) {
        dir->incoming = g_ptr_array_new_with_free_func(g_object_unref);
    } else {
        g_list_store_remove_all(dir->store);
    }

    emit_changed(dir);

    load = g_new0(Load, 1);
    load->dir = howl_dir_ref(dir);
    load->generation = dir->generation;

    g_file_enumerate_children_async(dir->location, attributes, G_FILE_QUERY_INFO_NONE,
                                    G_PRIORITY_DEFAULT, dir->cancellable, enumerate_cb, load);
}

static void
monitor_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                GFileMonitorEvent event, gpointer data) {
    HowlDir *dir = data;

    if (event == G_FILE_MONITOR_EVENT_CHANGED) {
        return;
    }

    schedule_reload(dir);
}

void howl_dir_load(HowlDir *dir, GFile *location) {
    stop_monitor(dir);
    g_set_object(&dir->location, location);

    dir->monitor = g_file_monitor_directory(location, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (dir->monitor != NULL) {
        g_signal_connect(dir->monitor, "changed", G_CALLBACK(monitor_changed), dir);
    }

    start_load(dir, FALSE);
}

void howl_dir_reload(HowlDir *dir) {
    if (dir->location == NULL || dir->disposed) {
        return;
    }
    if (dir->loading) {
        dir->reload_pending = TRUE;
        return;
    }
    start_load(dir, TRUE);
}
