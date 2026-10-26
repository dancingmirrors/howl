// Copyright © 2026 dancingmirrors
// SPDX-License-Identifier: MIT

#include "howl.h"

#include <sys/stat.h>

#define THUMB_SLOTS 4

typedef struct {
    int size;
    HowlThumbState state;
    GdkPaintable *paintable;
    guint stamp;
} ThumbSlot;

struct _HowlFile {
    GObject parent_instance;

    GFile *file;
    GFileInfo *info;
    char *content_type;
    char *type_description;
    guint64 size;
    gint64 mtime;
    gboolean is_dir;
    gboolean is_hidden;
    gboolean is_exec;

    ThumbSlot thumbs[THUMB_SLOTS];
    guint thumb_stamp;
    gboolean thumb_failed;
};

enum { PROP_0,
       PROP_DISPLAY_NAME,
       PROP_SIZE,
       PROP_MTIME,
       PROP_THUMBNAIL,
       N_PROPS };

enum { SIGNAL_CHANGED,
       N_SIGNALS };

static GParamSpec *props[N_PROPS];
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(HowlFile, howl_file, G_TYPE_OBJECT)

static void
howl_file_apply_info(HowlFile *self, GFileInfo *info) {
    const char *ct;

    g_set_object(&self->info, info);

    self->is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;

    g_free(self->content_type);
    ct = g_file_info_get_content_type(info);
    if (self->is_dir) {
        self->content_type = g_strdup("inode/directory");
    } else if (ct != NULL) {
        self->content_type = g_strdup(ct);
    } else {
        self->content_type = g_strdup("application/octet-stream");
    }

    g_clear_pointer(&self->type_description, g_free);

    self->size = self->is_dir ? 0 : (guint64)g_file_info_get_size(info);
    self->mtime = 0;
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
        self->mtime = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC;
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)) {
            self->mtime += g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
        }
    }

    self->is_hidden = g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info);
    self->is_exec = !self->is_dir && g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE);
}

static void
howl_file_clear_thumbnails(HowlFile *self) {
    for (guint i = 0; i < THUMB_SLOTS; i++) {
        g_clear_object(&self->thumbs[i].paintable);
        self->thumbs[i].size = 0;
        self->thumbs[i].state = HOWL_THUMB_NONE;
    }
    self->thumb_failed = FALSE;
}

static ThumbSlot *
howl_file_find_slot(HowlFile *self, int size, gboolean create) {
    ThumbSlot *victim = NULL;

    for (guint i = 0; i < THUMB_SLOTS; i++) {
        if (self->thumbs[i].size == size) {
            return &self->thumbs[i];
        }
    }
    if (!create) {
        return NULL;
    }

    for (guint i = 0; i < THUMB_SLOTS; i++) {
        ThumbSlot *slot = &self->thumbs[i];

        if (slot->size == 0) {
            victim = slot;
            break;
        }
        if (slot->state != HOWL_THUMB_PENDING && (victim == NULL || slot->stamp < victim->stamp)) {
            victim = slot;
        }
    }
    if (victim == NULL) {
        victim = &self->thumbs[0];
    }
    g_clear_object(&victim->paintable);
    victim->size = size;
    victim->state = HOWL_THUMB_NONE;
    victim->stamp = ++self->thumb_stamp;
    return victim;
}

HowlFile *
howl_file_new(GFile *parent, GFileInfo *info) {
    HowlFile *self = g_object_new(HOWL_TYPE_FILE, NULL);

    self->file = g_file_get_child(parent, g_file_info_get_name(info));
    howl_file_apply_info(self, info);
    return self;
}

void howl_file_update(HowlFile *self, GFileInfo *info) {
    gboolean content_changed, name_changed, other_changed;
    guint64 old_size = self->size;
    gint64 old_mtime = self->mtime;
    gboolean old_dir = self->is_dir;
    gboolean old_hidden = self->is_hidden;
    gboolean old_exec = self->is_exec;
    char *old_content_type = g_strdup(self->content_type);

    name_changed = g_strcmp0(g_file_info_get_display_name(self->info), g_file_info_get_display_name(info)) != 0;

    howl_file_apply_info(self, info);

    content_changed = old_size != self->size || old_mtime != self->mtime || old_dir != self->is_dir;
    other_changed = old_hidden != self->is_hidden || old_exec != self->is_exec || g_strcmp0(old_content_type, self->content_type) != 0;
    g_free(old_content_type);

    if (!content_changed && !name_changed && !other_changed) {
        return;
    }

    g_object_freeze_notify(G_OBJECT(self));
    if (content_changed) {
        howl_file_clear_thumbnails(self);
        g_object_notify_by_pspec(G_OBJECT(self), props[PROP_THUMBNAIL]);
        if (old_size != self->size) {
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SIZE]);
        }
        if (old_mtime != self->mtime) {
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MTIME]);
        }
    }
    if (name_changed) {
        g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DISPLAY_NAME]);
    }
    g_object_thaw_notify(G_OBJECT(self));

    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

GFile *
howl_file_get_file(HowlFile *self) {
    return self->file;
}

GFileInfo *
howl_file_get_info(HowlFile *self) {
    return self->info;
}

const char *
howl_file_get_name(HowlFile *self) {
    return g_file_info_get_name(self->info);
}

const char *
howl_file_get_display_name(HowlFile *self) {
    const char *name = g_file_info_get_display_name(self->info);

    return name != NULL ? name : g_file_info_get_name(self->info);
}

const char *
howl_file_get_content_type(HowlFile *self) {
    return self->content_type;
}

const char *
howl_file_get_type_description(HowlFile *self) {
    if (self->type_description == NULL) {
        if (self->is_dir) {
            self->type_description = g_strdup("Folder");
        } else {
            self->type_description = g_content_type_get_description(self->content_type);
        }
        if (self->type_description == NULL) {
            self->type_description = g_strdup(self->content_type);
        }
    }
    return self->type_description;
}

GIcon *
howl_file_get_icon(HowlFile *self) {
    return g_file_info_get_icon(self->info);
}

guint64 howl_file_get_size(HowlFile *self) {
    return self->size;
}

gint64 howl_file_get_mtime(HowlFile *self) {
    return self->mtime;
}

gboolean howl_file_is_directory(HowlFile *self) {
    return self->is_dir;
}

gboolean howl_file_is_hidden(HowlFile *self) {
    return self->is_hidden;
}

gboolean howl_file_is_executable(HowlFile *self) {
    return self->is_exec;
}

char *
howl_file_format_size(HowlFile *self) {
    if (self->is_dir) {
        return g_strdup("\342\200\224");
    }
    return g_format_size(self->size);
}

char *
howl_file_format_mtime(HowlFile *self) {
    GDateTime *then, *now;
    int y1, m1, d1, y2, m2, d2;
    char *result;

    if (self->mtime == 0) {
        return g_strdup("");
    }

    then = g_date_time_new_from_unix_local(self->mtime / G_USEC_PER_SEC);
    now = g_date_time_new_now_local();
    if (then == NULL || now == NULL) {
        g_clear_pointer(&then, g_date_time_unref);
        g_clear_pointer(&now, g_date_time_unref);
        return g_strdup("");
    }

    g_date_time_get_ymd(then, &y1, &m1, &d1);
    g_date_time_get_ymd(now, &y2, &m2, &d2);

    if (y1 == y2 && m1 == m2 && d1 == d2) {
        result = g_date_time_format(then, "%H:%M");
    } else if (y1 == y2) {
        result = g_date_time_format(then, "%-e %b, %H:%M");
    } else {
        result = g_date_time_format(then, "%-e %b %Y");
    }

    g_date_time_unref(then);
    g_date_time_unref(now);
    return result != NULL ? result : g_strdup("");
}

static void
mode_string(guint32 mode, gboolean is_symlink, char out[11]) {
    static const char rwx[] = "rwxrwxrwx";
    char type;

    if (is_symlink) {
        type = 'l';
        mode = (mode & ~0777u) | 0777u;
    } else if (S_ISDIR(mode)) {
        type = 'd';
    } else if (S_ISREG(mode)) {
        type = '.';
    } else if (S_ISLNK(mode)) {
        type = 'l';
    } else if (S_ISFIFO(mode)) {
        type = 'p';
    } else if (S_ISSOCK(mode)) {
        type = 's';
    } else if (S_ISCHR(mode)) {
        type = 'c';
    } else if (S_ISBLK(mode)) {
        type = 'b';
    } else {
        type = '?';
    }

    out[0] = type;
    for (int i = 0; i < 9; i++) {
        out[i + 1] = (mode & (0400u >> i)) ? rwx[i] : '-';
    }
    if (mode & S_ISUID) {
        out[3] = (mode & 0100u) ? 's' : 'S';
    }
    if (mode & S_ISGID) {
        out[6] = (mode & 0010u) ? 's' : 'S';
    }
    if (mode & S_ISVTX) {
        out[9] = (mode & 0001u) ? 't' : 'T';
    }
    out[10] = '\0';
}

static char *
eza_size(HowlFile *self) {
    static const char units[] = "kMGTPE";
    double value;
    gsize unit = 0;

    if (self->is_dir) {
        return g_strdup("-");
    }
    if (self->size < 1000) {
        return g_strdup_printf("%" G_GUINT64_FORMAT, self->size);
    }
    value = (double)self->size / 1000.0;
    while (value >= 1000.0 && unit + 2 < sizeof(units)) {
        value /= 1000.0;
        unit++;
    }
    return g_strdup_printf(value < 10.0 ? "%.1f%c" : "%.0f%c", value, units[unit]);
}

char *
howl_file_describe(HowlFile *self) {
    GString *line = g_string_new("<tt>");
    char perms[11];
    char *size, *when, *escaped;
    const char *user, *group, *target;
    GDateTime *then, *now;
    int y1, m1, d1, y2, m2, d2;

    mode_string(g_file_info_get_attribute_uint32(self->info, G_FILE_ATTRIBUTE_UNIX_MODE),
                g_file_info_get_is_symlink(self->info), perms);
    g_string_append(line, perms);

    size = eza_size(self);
    g_string_append_printf(line, " %s", size);
    g_free(size);

    user = g_file_info_get_attribute_string(self->info, G_FILE_ATTRIBUTE_OWNER_USER);
    group = g_file_info_get_attribute_string(self->info, G_FILE_ATTRIBUTE_OWNER_GROUP);
    if (user != NULL) {
        escaped = g_markup_escape_text(user, -1);
        g_string_append_printf(line, " %s", escaped);
        g_free(escaped);
    }
    if (group != NULL) {
        escaped = g_markup_escape_text(group, -1);
        g_string_append_printf(line, " %s", escaped);
        g_free(escaped);
    }

    then = g_date_time_new_from_unix_local(self->mtime / G_USEC_PER_SEC);
    now = g_date_time_new_now_local();
    if (then != NULL && now != NULL) {
        g_date_time_get_ymd(then, &y1, &m1, &d1);
        g_date_time_get_ymd(now, &y2, &m2, &d2);
        when = g_date_time_format(then, y1 == y2 ? "%-e %b %H:%M" : "%-e %b  %Y");
        if (when != NULL) {
            g_string_append_printf(line, " %s", when);
            g_free(when);
        }
    }
    g_clear_pointer(&then, g_date_time_unref);
    g_clear_pointer(&now, g_date_time_unref);

    escaped = g_markup_escape_text(howl_file_get_display_name(self), -1);
    g_string_append_printf(line, " %s%s", escaped,
                           g_file_info_get_is_symlink(self->info) ? "@" : self->is_dir ? "/"
                               : self->is_exec                                         ? "*"
                                                                                       : "");
    g_free(escaped);

    target = g_file_info_has_attribute(self->info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)
        ? g_file_info_get_symlink_target(self->info)
        : NULL;
    if (target != NULL) {
        char *display = g_filename_display_name(target);

        escaped = g_markup_escape_text(display, -1);
        g_string_append_printf(line, " -&gt; %s", escaped);
        g_free(escaped);
        g_free(display);
    }

    g_string_append(line, "</tt>");
    return g_string_free(line, FALSE);
}

GdkPaintable *
howl_file_get_thumbnail(HowlFile *self, int size) {
    ThumbSlot *slot = howl_file_find_slot(self, size, FALSE);

    return slot != NULL ? slot->paintable : NULL;
}

HowlThumbState howl_file_get_thumb_state(HowlFile *self, int size) {
    ThumbSlot *slot;

    if (self->thumb_failed) {
        return HOWL_THUMB_FAILED;
    }
    slot = howl_file_find_slot(self, size, FALSE);
    return slot != NULL ? slot->state : HOWL_THUMB_NONE;
}

void howl_file_set_thumb_state(HowlFile *self, int size, HowlThumbState state) {
    ThumbSlot *slot;

    if (state == HOWL_THUMB_FAILED) {
        self->thumb_failed = TRUE;
        return;
    }
    slot = howl_file_find_slot(self, size, TRUE);
    slot->state = state;
    slot->stamp = ++self->thumb_stamp;
}

void howl_file_release_thumbnails(HowlFile *self) {
    for (guint i = 0; i < THUMB_SLOTS; i++) {
        ThumbSlot *slot = &self->thumbs[i];

        if (slot->state == HOWL_THUMB_DONE) {
            g_clear_object(&slot->paintable);
            slot->state = HOWL_THUMB_NONE;
            slot->size = 0;
        }
    }
}

void howl_file_set_thumbnail(HowlFile *self, int size, GdkPaintable *paintable) {
    ThumbSlot *slot;

    if (paintable == NULL) {
        self->thumb_failed = TRUE;
        slot = howl_file_find_slot(self, size, FALSE);
        if (slot != NULL) {
            slot->state = HOWL_THUMB_NONE;
        }
    } else {
        slot = howl_file_find_slot(self, size, TRUE);
        g_set_object(&slot->paintable, paintable);
        slot->state = HOWL_THUMB_DONE;
        slot->stamp = ++self->thumb_stamp;
    }
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_THUMBNAIL]);
}

static void
howl_file_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    HowlFile *self = HOWL_FILE(object);
    ThumbSlot *newest = NULL;

    switch (prop_id) {
    case PROP_DISPLAY_NAME:
        g_value_set_string(value, howl_file_get_display_name(self));
        break;
    case PROP_SIZE:
        g_value_set_uint64(value, self->size);
        break;
    case PROP_MTIME:
        g_value_set_int64(value, self->mtime);
        break;
    case PROP_THUMBNAIL:
        for (guint i = 0; i < THUMB_SLOTS; i++) {
            if (self->thumbs[i].paintable != NULL && (newest == NULL || self->thumbs[i].stamp > newest->stamp)) {
                newest = &self->thumbs[i];
            }
        }
        g_value_set_object(value, newest != NULL ? newest->paintable : NULL);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
howl_file_dispose(GObject *object) {
    HowlFile *self = HOWL_FILE(object);

    g_clear_object(&self->file);
    g_clear_object(&self->info);
    howl_file_clear_thumbnails(self);
    g_clear_pointer(&self->content_type, g_free);
    g_clear_pointer(&self->type_description, g_free);

    G_OBJECT_CLASS(howl_file_parent_class)->dispose(object);
}

static void
howl_file_class_init(HowlFileClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = howl_file_get_property;
    object_class->dispose = howl_file_dispose;

    props[PROP_DISPLAY_NAME] = g_param_spec_string("display-name", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    props[PROP_SIZE] = g_param_spec_uint64("size", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    props[PROP_MTIME] = g_param_spec_int64("mtime", NULL, NULL, G_MININT64, G_MAXINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    props[PROP_THUMBNAIL] = g_param_spec_object("thumbnail", NULL, NULL, GDK_TYPE_PAINTABLE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class, N_PROPS, props);

    signals[SIGNAL_CHANGED] = g_signal_new("changed", HOWL_TYPE_FILE, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
howl_file_init(HowlFile *self) {
}
