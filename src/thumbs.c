// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// clang-format off
#if defined(__linux__)
#include <sys/vfs.h>
#elif defined(__NetBSD__)
#include <sys/statvfs.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__APPLE__)
#include <sys/param.h>
#include <sys/mount.h>
#endif
// clang-format on

#define THUMBNAILER_GROUP "Thumbnailer Entry"
#define TMPFS_MAGIC 0x01021994
#define RAMFS_MAGIC 0x858458f6

typedef struct {
    char **argv;
} Thumbnailer;

typedef struct {
    char *key;
    GdkTexture *texture;
    gsize bytes;
    GList *link;
} CacheEntry;

typedef struct {
    HowlFile *file;
    char *path;
    char *uri;
    char *content_type;
    guint64 file_size;
    gint64 mtime;
    int size;
    int scale;
    guint seq;
    gint cancelled;
    gboolean transient;
    GdkPixbuf *result;
} Request;

static gboolean enabled;
static GHashTable *pixbuf_types;
static GHashTable *script_types;
static GPtrArray *scripts;
static char *tmpdir;
static GThreadPool *pool;
static guint next_seq;
static guint n_generated;

static GHashTable *cache;
static GQueue lru = G_QUEUE_INIT;
static gsize cache_bytes;

static GMutex children_lock;
static GArray *children;
static gint shutting_down;

static gboolean
dir_is_memory_backed(const char *path) {
#if defined(__linux__)
    struct statfs st;

    if (statfs(path, &st) != 0) {
        return FALSE;
    }
    return st.f_type == TMPFS_MAGIC || st.f_type == RAMFS_MAGIC;
#elif defined(__NetBSD__)
    struct statvfs st;

    if (statvfs(path, &st) != 0) {
        return FALSE;
    }
    return strcmp(st.f_fstypename, "tmpfs") == 0 || strcmp(st.f_fstypename, "mfs") == 0;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__APPLE__)
    struct statfs st;

    if (statfs(path, &st) != 0) {
        return FALSE;
    }
    return strcmp(st.f_fstypename, "tmpfs") == 0 || strcmp(st.f_fstypename, "mfs") == 0;
#else
    (void)path;
    return FALSE;
#endif
}

static char *
pick_tmpdir(void) {
    const char *override = g_getenv("HOWL_TMPDIR");
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    const char *candidates[3];
    guint n = 0, i;

    if (override != NULL && *override != '\0') {
        candidates[n++] = override;
    }
    if (runtime != NULL && *runtime != '\0') {
        candidates[n++] = runtime;
    }
    candidates[n++] = "/dev/shm";

    for (i = 0; i < n; i++) {
        char *template;

        if (!g_file_test(candidates[i], G_FILE_TEST_IS_DIR)) {
            continue;
        }
        if (candidates[i] != override && !dir_is_memory_backed(candidates[i])) {
            continue;
        }

        template = g_build_filename(candidates[i], "howl-XXXXXX", NULL);
        if (g_mkdtemp_full(template, 0700) != NULL) {
            return template;
        }
        g_free(template);
    }
    return NULL;
}

static void
thumbnailer_free(gpointer data) {
    Thumbnailer *t = data;

    g_strfreev(t->argv);
    g_free(t);
}

static void
load_thumbnailer_file(const char *path) {
    GKeyFile *kf = g_key_file_new();
    char *try_exec = NULL, *exec = NULL, *program = NULL;
    char **mimes = NULL, **argv = NULL, **m;
    Thumbnailer *t;

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        goto out;
    }

    try_exec = g_key_file_get_string(kf, THUMBNAILER_GROUP, "TryExec", NULL);
    exec = g_key_file_get_string(kf, THUMBNAILER_GROUP, "Exec", NULL);
    mimes = g_key_file_get_string_list(kf, THUMBNAILER_GROUP, "MimeType", NULL, NULL);
    if (exec == NULL || mimes == NULL) {
        goto out;
    }

    if (!g_shell_parse_argv(exec, NULL, &argv, NULL) || argv[0] == NULL) {
        goto out;
    }

    program = g_find_program_in_path(try_exec != NULL && *try_exec != '\0' ? try_exec : argv[0]);
    if (program == NULL) {
        goto out;
    }

    t = g_new0(Thumbnailer, 1);
    t->argv = g_steal_pointer(&argv);
    g_ptr_array_add(scripts, t);

    for (m = mimes; *m != NULL; m++) {
        g_strstrip(*m);
        if (**m != '\0' && !g_hash_table_contains(script_types, *m)) {
            g_hash_table_insert(script_types, g_strdup(*m), t);
        }
    }

out:
    g_strfreev(argv);
    g_strfreev(mimes);
    g_free(program);
    g_free(exec);
    g_free(try_exec);
    g_key_file_unref(kf);
}

static void
load_thumbnailers(void) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char *const *system_dirs = g_get_system_data_dirs();
    GPtrArray *dirs = g_ptr_array_new();
    guint i;

    g_ptr_array_add(dirs, (gpointer)g_get_user_data_dir());
    for (i = 0; system_dirs[i] != NULL; i++) {
        g_ptr_array_add(dirs, (gpointer)system_dirs[i]);
    }

    for (i = 0; i < dirs->len; i++) {
        char *path = g_build_filename(g_ptr_array_index(dirs, i), "thumbnailers", NULL);
        GDir *d = g_dir_open(path, 0, NULL);
        const char *name;

        while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
            char *file;

            if (!g_str_has_suffix(name, ".thumbnailer") || g_hash_table_contains(seen, name)) {
                continue;
            }
            g_hash_table_add(seen, g_strdup(name));
            file = g_build_filename(path, name, NULL);
            load_thumbnailer_file(file);
            g_free(file);
        }
        if (d != NULL) {
            g_dir_close(d);
        }
        g_free(path);
    }

    g_ptr_array_unref(dirs);
    g_hash_table_unref(seen);
}

static void worker(gpointer data, gpointer user_data);

void howl_thumbs_init(void) {
    GSList *formats, *l;

    if (!howl_opts.thumbnails) {
        return;
    }

    pixbuf_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    formats = gdk_pixbuf_get_formats();
    for (l = formats; l != NULL; l = l->next) {
        char **mimes, **m;

        if (gdk_pixbuf_format_is_disabled(l->data)) {
            continue;
        }
        mimes = gdk_pixbuf_format_get_mime_types(l->data);
        for (m = mimes; m != NULL && *m != NULL; m++) {
            g_hash_table_add(pixbuf_types, g_strdup(*m));
        }
        g_strfreev(mimes);
    }
    g_slist_free(formats);

    scripts = g_ptr_array_new_with_free_func(thumbnailer_free);
    script_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    load_thumbnailers();
    if (scripts->len > 0) {
        tmpdir = pick_tmpdir();
        if (tmpdir == NULL) {
            g_debug("no memory-backed directory available; external thumbnailers disabled");
            g_hash_table_remove_all(script_types);
        }
    }

    children = g_array_new(FALSE, FALSE, sizeof(GPid));
    cache = g_hash_table_new(g_str_hash, g_str_equal);
    pool = g_thread_pool_new(worker, NULL, HOWL_THUMB_WORKERS, FALSE, NULL);
    enabled = pool != NULL;
}

void howl_thumbs_shutdown(void) {
    if (!enabled) {
        return;
    }

    g_debug("thumbnails: %u requested, %u generated, %u cached (%zu KiB)",
            next_seq, (guint)g_atomic_int_get(&n_generated), g_hash_table_size(cache), cache_bytes / 1024);

    g_mutex_lock(&children_lock);
    g_atomic_int_set(&shutting_down, 1);
    for (guint i = 0; i < children->len; i++) {
        kill(-g_array_index(children, GPid, i), SIGKILL);
    }
    g_mutex_unlock(&children_lock);

    if (tmpdir != NULL) {
        GDir *d = g_dir_open(tmpdir, 0, NULL);
        const char *name;

        while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
            char *path = g_build_filename(tmpdir, name, NULL);
            unlink(path);
            g_free(path);
        }
        if (d != NULL) {
            g_dir_close(d);
        }
        rmdir(tmpdir);
    }
}

gboolean
howl_thumbs_supported(const char *content_type) {
    if (!enabled || content_type == NULL) {
        return FALSE;
    }
    return g_hash_table_contains(pixbuf_types, content_type) ||
        g_hash_table_contains(script_types, content_type);
}

static char *
make_key(const char *path, gint64 mtime, guint64 size, int px) {
    return g_strdup_printf("%d\n%" G_GINT64_FORMAT "\n%" G_GUINT64_FORMAT "\n%s", px, mtime, size, path);
}

static void
cache_entry_free(CacheEntry *entry) {
    g_object_unref(entry->texture);
    g_free(entry->key);
    g_free(entry);
}

static void
cache_touch(CacheEntry *entry) {
    g_queue_unlink(&lru, entry->link);
    g_queue_push_head_link(&lru, entry->link);
}

static void
cache_insert(char *key, GdkTexture *texture) {
    CacheEntry *entry = g_hash_table_lookup(cache, key);

    if (entry != NULL) {
        cache_touch(entry);
        g_free(key);
        return;
    }

    entry = g_new0(CacheEntry, 1);
    entry->key = key;
    entry->texture = g_object_ref(texture);
    entry->bytes = (gsize)gdk_texture_get_width(texture) * gdk_texture_get_height(texture) * 4;
    g_hash_table_insert(cache, entry->key, entry);
    g_queue_push_head(&lru, entry);
    entry->link = lru.head;
    cache_bytes += entry->bytes;

    while (cache_bytes > HOWL_THUMB_CACHE_BYTES && lru.length > 1) {
        CacheEntry *old = g_queue_pop_tail(&lru);

        g_hash_table_remove(cache, old->key);
        cache_bytes -= old->bytes;
        cache_entry_free(old);
    }
}

static GdkPixbuf *
load_image(Request *r) {
    GdkPixbuf *pixbuf, *oriented;
    GdkPixbufFormat *format = NULL;
    int px = r->size * r->scale;
    int w = 0, h = 0;
    gboolean svg = g_str_equal(r->content_type, "image/svg+xml");
    gboolean known;

    if (r->file_size > HOWL_THUMB_IMAGE_LIMIT) {
        return NULL;
    }

    known = !svg && (format = gdk_pixbuf_get_file_info(r->path, &w, &h)) != NULL;
    if (known) {
        char *name = gdk_pixbuf_format_get_name(format);
        gint64 limit = g_strcmp0(name, "jpeg") == 0 ? 10 * (gint64)HOWL_THUMB_PIXEL_LIMIT : HOWL_THUMB_PIXEL_LIMIT;

        g_free(name);
        if ((gint64)w * h > limit) {
            return NULL;
        }
    }

    if (svg || !known || w > px || h > px) {
        pixbuf = gdk_pixbuf_new_from_file_at_scale(r->path, px, px, TRUE, NULL);
    } else {
        pixbuf = gdk_pixbuf_new_from_file(r->path, NULL);
    }

    if (pixbuf == NULL) {
        return NULL;
    }

    oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
    g_object_unref(pixbuf);
    return oriented;
}

static char *
expand_exec_token(const char *s, Request *r, const char *out, const char *size) {
    GString *g = g_string_new(NULL);

    while (*s != '\0') {
        if (*s != '%') {
            g_string_append_c(g, *s++);
            continue;
        }
        s++;
        switch (*s) {
        case 'i':
            g_string_append(g, r->path);
            break;
        case 'u':
            g_string_append(g, r->uri);
            break;
        case 'o':
            g_string_append(g, out);
            break;
        case 's':
            g_string_append(g, size);
            break;
        case '%':
            g_string_append_c(g, '%');
            break;
        case '\0':
            continue;
        default:
            break;
        }
        s++;
    }
    return g_string_free(g, FALSE);
}

static void
child_setup(gpointer data) {
    setpgid(0, 0);
}

static pid_t
reap_child(GPid pid, int *status, int options) {
    pid_t p;
    gboolean gone;

    g_mutex_lock(&children_lock);
    p = waitpid(pid, status, options);
    gone = p == pid || (p < 0 && errno != EINTR);
    if (gone) {
        for (guint i = 0; i < children->len; i++) {
            if (g_array_index(children, GPid, i) == pid) {
                g_array_remove_index_fast(children, i);
                break;
            }
        }
    }
    g_mutex_unlock(&children_lock);
    return gone ? pid : 0;
}

static gboolean
wait_child(GPid pid, int *status, int timeout_ms) {
    int waited = 0;

    for (;;) {
        if (reap_child(pid, status, WNOHANG) == pid) {
            return TRUE;
        }
        if (waited >= timeout_ms) {
            return FALSE;
        }
        g_usleep(20 * 1000);
        waited += 20;
    }
}

static GdkPixbuf *
run_thumbnailer(Request *r, Thumbnailer *t) {
    GdkPixbuf *pixbuf = NULL;
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    char *out = g_strdup_printf("%s/%u.png", tmpdir, r->seq);
    char size[16];
    GPid pid;
    gboolean spawned;
    int status = -1;

    g_snprintf(size, sizeof size, "%d", r->size * r->scale);
    for (guint i = 0; t->argv[i] != NULL; i++) {
        g_ptr_array_add(argv, expand_exec_token(t->argv[i], r, out, size));
    }
    g_ptr_array_add(argv, NULL);

    g_mutex_lock(&children_lock);
    if (g_atomic_int_get(&shutting_down)) {
        g_mutex_unlock(&children_lock);
        goto out;
    }
    spawned = g_spawn_async(NULL, (char **)argv->pdata, NULL,
                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                            child_setup, NULL, &pid, NULL);
    if (spawned) {
        g_array_append_val(children, pid);
    }
    g_mutex_unlock(&children_lock);

    if (!spawned) {
        r->transient = TRUE;
        goto out;
    }

    if (!wait_child(pid, &status, HOWL_THUMBNAILER_TIMEOUT * 1000)) {
        kill(-pid, SIGKILL);
        reap_child(pid, &status, 0);
        status = -1;
    }
    g_spawn_close_pid(pid);

    if (status == 0) {
        pixbuf = gdk_pixbuf_new_from_file_at_scale(out, r->size * r->scale, r->size * r->scale, TRUE, NULL);
    }

out:
    unlink(out);
    g_free(out);
    g_ptr_array_unref(argv);
    return pixbuf;
}

static gboolean deliver(gpointer data);

static void
worker(gpointer data, gpointer user_data) {
    Request *r = data;

    if (!g_atomic_int_get(&r->cancelled) && !g_atomic_int_get(&shutting_down)) {
        Thumbnailer *t;

        if (g_hash_table_contains(pixbuf_types, r->content_type)) {
            r->result = load_image(r);
        } else if (tmpdir != NULL && (t = g_hash_table_lookup(script_types, r->content_type)) != NULL) {
            r->result = run_thumbnailer(r, t);
        }
    }

    if (r->result != NULL) {
        g_atomic_int_inc(&n_generated);
    }
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, deliver, r, NULL);
}

static void
request_free(Request *r) {
    g_clear_object(&r->result);
    g_object_unref(r->file);
    g_free(r->path);
    g_free(r->uri);
    g_free(r->content_type);
    g_free(r);
}

static GdkPaintable *
texture_to_paintable(GdkTexture *texture, int size, int scale) {
    GtkSnapshot *snapshot = gtk_snapshot_new();
    double w = (double)gdk_texture_get_width(texture) / scale;
    double h = (double)gdk_texture_get_height(texture) / scale;

    if (w > size || h > size) {
        double ratio = MIN(size / w, size / h);
        w *= ratio;
        h *= ratio;
    }
    gtk_snapshot_append_scaled_texture(snapshot, texture, GSK_SCALING_FILTER_TRILINEAR,
                                       &GRAPHENE_RECT_INIT((float)((size - w) / 2), (float)((size - h) / 2),
                                                           (float)w, (float)h));
    return gtk_snapshot_free_to_paintable(snapshot, &GRAPHENE_SIZE_INIT(size, size));
}

#define REQUESTS_KEY "howl-thumb-requests"

static void
register_request(HowlFile *file, Request *r) {
    GSList *list = g_object_steal_data(G_OBJECT(file), REQUESTS_KEY);

    g_object_set_data(G_OBJECT(file), REQUESTS_KEY, g_slist_prepend(list, r));
}

static gboolean
unregister_request(HowlFile *file, Request *r) {
    GSList *list = g_object_steal_data(G_OBJECT(file), REQUESTS_KEY);
    gboolean found = g_slist_find(list, r) != NULL;

    g_object_set_data(G_OBJECT(file), REQUESTS_KEY, g_slist_remove(list, r));
    return found;
}

static GdkTexture *
texture_from_pixbuf(GdkPixbuf *pixbuf) {
    GBytes *bytes = g_bytes_new_with_free_func(gdk_pixbuf_read_pixels(pixbuf), gdk_pixbuf_get_byte_length(pixbuf), g_object_unref, g_object_ref(pixbuf));
    GdkMemoryFormat format = gdk_pixbuf_get_n_channels(pixbuf) == 4 ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
    GdkTexture *texture = gdk_memory_texture_new(gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf), format, bytes, (gsize)gdk_pixbuf_get_rowstride(pixbuf));

    g_bytes_unref(bytes);
    return texture;
}

static gboolean
deliver(gpointer data) {
    Request *r = data;
    HowlFile *file = r->file;
    gboolean current;

    if (g_atomic_int_get(&shutting_down)) {
        request_free(r);
        return G_SOURCE_REMOVE;
    }

    current = unregister_request(file, r) && howl_file_get_mtime(file) == r->mtime && howl_file_get_size(file) == r->file_size && howl_file_get_thumb_state(file, r->size) == HOWL_THUMB_PENDING;

    if (r->result != NULL) {
        GdkTexture *texture = texture_from_pixbuf(r->result);

        cache_insert(make_key(r->path, r->mtime, r->file_size, r->size * r->scale), texture);
        if (current) {
            GdkPaintable *paintable = texture_to_paintable(texture, r->size, r->scale);

            howl_file_set_thumbnail(file, r->size, paintable);
            g_object_unref(paintable);
        }
        g_object_unref(texture);
    } else if (current) {
        if (r->transient) {
            howl_file_set_thumb_state(file, r->size, HOWL_THUMB_NONE);
        } else if (g_atomic_int_get(&r->cancelled)) {
            howl_file_set_thumb_state(file, r->size, HOWL_THUMB_NONE);
            g_object_notify(G_OBJECT(file), "thumbnail");
        } else {
            howl_file_set_thumbnail(file, r->size, NULL);
        }
    }

    request_free(r);
    return G_SOURCE_REMOVE;
}

void howl_thumbs_request(HowlFile *file, int size, int scale) {
    const char *content_type;
    GFile *gfile;
    CacheEntry *entry;
    Request *r;
    char *path, *key;

    if (!enabled || size <= 0 || howl_file_get_thumb_state(file, size) != HOWL_THUMB_NONE) {
        return;
    }

    content_type = howl_file_get_content_type(file);
    gfile = howl_file_get_file(file);
    path = g_file_get_path(gfile);
    if (howl_file_is_directory(file) || !howl_thumbs_supported(content_type) || path == NULL) {
        howl_file_set_thumb_state(file, size, HOWL_THUMB_FAILED);
        g_free(path);
        return;
    }

    if (scale < 1) {
        scale = 1;
    }

    key = make_key(path, howl_file_get_mtime(file), howl_file_get_size(file), size * scale);
    entry = g_hash_table_lookup(cache, key);
    g_free(key);
    if (entry != NULL) {
        GdkPaintable *paintable = texture_to_paintable(entry->texture, size, scale);

        cache_touch(entry);
        howl_file_set_thumbnail(file, size, paintable);
        g_object_unref(paintable);
        g_free(path);
        return;
    }

    r = g_new0(Request, 1);
    r->file = g_object_ref(file);
    r->path = path;
    r->uri = g_file_get_uri(gfile);
    r->content_type = g_strdup(content_type);
    r->file_size = howl_file_get_size(file);
    r->mtime = howl_file_get_mtime(file);
    r->size = size;
    r->scale = scale;
    r->seq = ++next_seq;

    howl_file_set_thumb_state(file, size, HOWL_THUMB_PENDING);
    register_request(file, r);
    g_thread_pool_push(pool, r, NULL);
}

void howl_thumbs_cancel(HowlFile *file) {
    for (GSList *l = g_object_get_data(G_OBJECT(file), REQUESTS_KEY); l != NULL; l = l->next) {
        g_atomic_int_set(&((Request *)l->data)->cancelled, 1);
    }
}
