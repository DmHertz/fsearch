/*
   FSearch - A fast file search utility
   Copyright © 2020 Christian Boxdörfer

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#define _GNU_SOURCE

#define G_LOG_DOMAIN "fsearch-database"

#include <assert.h>
#include <dirent.h>
#include <fnmatch.h>
#include <glib/gi18n.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "fsearch_database.h"
#include "fsearch_database_entry.h"
#include "fsearch_database_search.h"
#include "fsearch_exclude_path.h"
#include "fsearch_index.h"
#include "fsearch_memory_pool.h"
#include "fsearch_query.h"
#include "fsearch_task.h"
#include "fsearch_thread_pool.h"

#define NUM_DB_ENTRIES_FOR_POOL_BLOCK 10000

#define DATABASE_MAJOR_VERSION 0
#define DATABASE_MINOR_VERSION 6
#define DATABASE_MAGIC_NUMBER "FSDB"

struct FsearchDatabaseView {
    uint32_t id;

    FsearchDatabase *db;

    FsearchQuery *query;

    DynamicArray *files;
    DynamicArray *folders;
    GHashTable *selection;

    FsearchDatabaseIndexType sort_order;

    char *query_text;
    FsearchFilter *filter;
    FsearchQueryFlags query_flags;
    uint32_t query_id;

    FsearchTaskQueue *task_queue;

    FsearchDatabaseViewNotifyFunc view_changed_func;
    FsearchDatabaseViewNotifyFunc search_started_func;
    FsearchDatabaseViewNotifyFunc search_finished_func;
    FsearchDatabaseViewNotifyFunc sort_started_func;
    FsearchDatabaseViewNotifyFunc sort_finished_func;

    gpointer user_data;

    GMutex mutex;
};

struct FsearchDatabase {
    // DynamicArray *files;
    // DynamicArray *folders;

    DynamicArray *sorted_files[NUM_DATABASE_INDEX_TYPES];
    DynamicArray *sorted_folders[NUM_DATABASE_INDEX_TYPES];

    FsearchMemoryPool *file_pool;
    FsearchMemoryPool *folder_pool;

    GList *db_views;
    FsearchThreadPool *thread_pool;

    FsearchDatabaseIndexFlags index_flags;

    uint32_t num_entries;
    uint32_t num_folders;
    uint32_t num_files;

    GList *indexes;
    GList *excludes;
    char **exclude_files;

    bool exclude_hidden;
    time_t timestamp;

    int32_t ref_count;
    GMutex mutex;
};

enum {
    WALK_OK = 0,
    WALK_BADIO,
    WALK_CANCEL,
};

static void
db_view_update_entries(FsearchDatabaseView *view);

static void
db_view_update_sort(FsearchDatabaseView *view);

// Implementation

void
db_view_free(FsearchDatabaseView *view) {
    if (!view) {
        return;
    }

    g_mutex_lock(&view->mutex);

    if (view->filter) {
        fsearch_filter_unref(view->filter);
        view->filter = NULL;
    }

    if (view->query_text) {
        free(view->query_text);
        view->query_text = NULL;
    }

    if (view->task_queue) {
        fsearch_task_queue_free(view->task_queue);
        view->task_queue = NULL;
    }

    if (view->query) {
        fsearch_query_free(view->query);
        view->query = NULL;
    }

    db_view_unregister(view);

    if (view->selection) {
        g_hash_table_destroy(view->selection);
        view->selection = NULL;
    }

    g_mutex_unlock(&view->mutex);
    g_mutex_clear(&view->mutex);

    free(view);
    view = NULL;
}

void
db_view_unregister(FsearchDatabaseView *view) {
    assert(view != NULL);

    FsearchDatabase *db = view->db;
    if (db) {
    }

    if (view->selection) {
        g_hash_table_remove_all(view->selection);
    }

    if (view->files) {
        darray_unref(view->files);
        view->files = NULL;
    }
    if (view->folders) {
        darray_unref(view->folders);
        view->folders = NULL;
    }
    if (view->db) {
        view->db->db_views = g_list_remove(db->db_views, view);
        db_unref(view->db);
        view->db = NULL;
    }
}

void
db_view_register(FsearchDatabase *db, FsearchDatabaseView *view) {
    assert(view != NULL);
    assert(db != NULL);

    if (g_list_find(db->db_views, view)) {
        g_debug("[db_view] view is already registered for database");
        return;
    }
    db->db_views = g_list_append(db->db_views, view);

    view->db = db_ref(db);
    view->files = db_get_files(db);
    view->folders = db_get_folders(db);

    if (view->view_changed_func) {
        view->view_changed_func(view, view->user_data);
    }
    db_view_update_entries(view);
    db_view_update_sort(view);
}

FsearchDatabaseEntry *
db_view_get_entry(FsearchDatabaseView *view, uint32_t idx) {
    uint32_t num_entries = db_view_get_num_entries(view);
    if (idx >= num_entries) {
        return NULL;
    }
    uint32_t num_folders = db_view_get_num_folders(view);
    if (idx < num_folders) {
        return darray_get_item(view->folders, idx);
    }
    else {
        return darray_get_item(view->files, idx - num_folders);
    }
}

FsearchDatabaseView *
db_view_new(const char *query_text,
            FsearchQueryFlags flags,
            FsearchFilter *filter,
            FsearchDatabaseIndexType sort_order,
            FsearchDatabaseViewNotifyFunc view_changed_func,
            FsearchDatabaseViewNotifyFunc search_started_func,
            FsearchDatabaseViewNotifyFunc search_finished_func,
            FsearchDatabaseViewNotifyFunc sort_started_func,
            FsearchDatabaseViewNotifyFunc sort_finished_func,
            gpointer user_data) {
    FsearchDatabaseView *view = calloc(1, sizeof(struct FsearchDatabaseView));
    assert(view != NULL);

    view->task_queue = fsearch_task_queue_new("fsearch_db_task_queue");

    view->selection = g_hash_table_new(g_direct_hash, g_direct_equal);

    view->query_text = strdup(query_text ? query_text : "");
    view->query_flags = flags;
    view->filter = fsearch_filter_ref(filter);
    view->sort_order = sort_order;

    view->view_changed_func = view_changed_func;
    view->search_started_func = search_started_func;
    view->search_finished_func = search_finished_func;
    view->sort_started_func = sort_started_func;
    view->sort_finished_func = sort_finished_func;
    view->user_data = user_data;

    g_mutex_init(&view->mutex);

    return view;
}

static void
db_view_task_query_cancelled(FsearchTask *task, gpointer data) {
    FsearchQuery *query = data;
    FsearchDatabaseView *view = query->data;

    if (view->search_finished_func) {
        view->search_finished_func(view, view->user_data);
    }

    if (query) {
        fsearch_query_free(query);
        query = NULL;
    }

    fsearch_task_free(task);
    task = NULL;
}

static void
db_view_task_query_finished(FsearchTask *task, gpointer result, gpointer data) {
    FsearchQuery *query = data;
    FsearchDatabaseView *view = query->data;

    if (view->query) {
        fsearch_query_free(view->query);
    }
    view->query = query;

    if (result) {
        g_mutex_lock(&view->mutex);
        DatabaseSearchResult *res = result;

        if (view->selection) {
            g_hash_table_remove_all(view->selection);
        }

        if (view->files) {
            darray_unref(view->files);
        }
        view->files = res->files;

        if (view->folders) {
            darray_unref(view->folders);
        }
        view->folders = res->folders;

        g_mutex_unlock(&view->mutex);

        if (view->search_finished_func) {
            view->search_finished_func(view, view->user_data);
        }
        if (view->view_changed_func) {
            view->view_changed_func(view, view->user_data);
        }
    }

    fsearch_task_free(task);
    task = NULL;
}

static void
db_view_on_match_everything(FsearchDatabaseView *view) {
    darray_unref(view->files);
    darray_unref(view->folders);
    if (db_has_entries_sorted_by_type(view->db, view->sort_order)) {
        view->files = db_get_files_sorted(view->db, view->sort_order);
        view->folders = db_get_folders_sorted(view->db, view->sort_order);
    }
    else {
        view->files = db_get_files(view->db);
        view->folders = db_get_folders(view->db);
        view->sort_order = DATABASE_INDEX_TYPE_NAME;
    }
}

typedef struct {
    FsearchDatabaseView *view;
    DynamicArrayCompareDataFunc compare_func;
    bool parallel_sort;
} FsearchSortContext;

static void
db_sort_array(DynamicArray *array, DynamicArrayCompareDataFunc sort_func, bool parallel_sort) {
    if (!array) {
        return;
    }
    if (parallel_sort) {
        darray_sort_multi_threaded(array, (DynamicArrayCompareFunc)sort_func);
    }
    else {
        darray_sort(array, (DynamicArrayCompareFunc)sort_func);
    }
}

static gpointer
db_sort_task(gpointer data, GCancellable *cancellable) {
    FsearchSortContext *ctx = data;
    FsearchDatabaseView *view = ctx->view;

    if (view->sort_started_func) {
        view->sort_started_func(view, view->user_data);
    }

    GTimer *timer = g_timer_new();
    g_timer_start(timer);

    db_sort_array(view->folders, ctx->compare_func, ctx->parallel_sort);
    db_sort_array(view->files, ctx->compare_func, ctx->parallel_sort);

    g_timer_stop(timer);
    const double seconds = g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);
    timer = NULL;

    g_debug("[sort] finished in %2.fms", seconds * 1000);

    if (view->sort_finished_func) {
        view->sort_finished_func(view, view->user_data);
    }

    return NULL;
}

static void
db_sort_task_cancelled(FsearchTask *task, gpointer data) {
    FsearchSortContext *ctx = data;

    free(ctx);
    ctx = NULL;

    fsearch_task_free(task);
    task = NULL;
}

static void
db_sort_task_finished(FsearchTask *task, gpointer result, gpointer data) {
    db_sort_task_cancelled(task, data);
}

static void
db_view_update_sort(FsearchDatabaseView *view) {
    if (!view || !view->db) {
        return;
    }

    if (!view->query || fsearch_query_matches_everything(view->query)) {
        // we're matching everything, so if the database has the entries already sorted we don't need
        // to sort again
        darray_unref(view->files);
        darray_unref(view->folders);

        if (db_has_entries_sorted_by_type(view->db, view->sort_order)) {
            if (view->sort_started_func) {
                view->sort_started_func(view, view->user_data);
            }
            view->files = db_get_files_sorted(view->db, view->sort_order);
            view->folders = db_get_folders_sorted(view->db, view->sort_order);
            if (view->sort_finished_func) {
                view->sort_finished_func(view, view->user_data);
            }
            return;
        }

        view->files = db_get_files_copy(view->db);
        view->folders = db_get_folders_copy(view->db);
    }

    bool parallel_sort = true;

    g_debug("[sort] started: %d", view->sort_order);
    DynamicArrayCompareFunc func = NULL;
    switch (view->sort_order) {
    case DATABASE_INDEX_TYPE_NAME:
        func = (DynamicArrayCompareFunc)db_entry_compare_entries_by_name;
        break;
    case DATABASE_INDEX_TYPE_PATH:
        func = (DynamicArrayCompareFunc)db_entry_compare_entries_by_path;
        break;
    case DATABASE_INDEX_TYPE_SIZE:
        func = (DynamicArrayCompareFunc)db_entry_compare_entries_by_size;
        break;
    case DATABASE_INDEX_TYPE_FILETYPE:
        func = (DynamicArrayCompareFunc)db_entry_compare_entries_by_type;
        parallel_sort = false;
        break;
    case DATABASE_INDEX_TYPE_MODIFICATION_TIME:
        func = (DynamicArrayCompareFunc)db_entry_compare_entries_by_modification_time;
        break;
    default:
        func = (DynamicArrayCompareFunc)db_entry_compare_entries_by_position;
    }

    FsearchSortContext *ctx = calloc(1, sizeof(FsearchSortContext));
    g_assert(ctx != NULL);

    ctx->view = view;
    ctx->compare_func = (DynamicArrayCompareDataFunc)func;
    ctx->parallel_sort = parallel_sort;

    FsearchTask *task = fsearch_task_new(1, db_sort_task, db_sort_task_finished, db_sort_task_cancelled, ctx);
    fsearch_task_queue(view->task_queue, task, FSEARCH_TASK_CLEAR_SAME_ID);
}

static void
db_view_update_entries(FsearchDatabaseView *view) {
    if (!view || !view->db) {
        return;
    }

    if (view->search_started_func) {
        view->search_started_func(view, view->user_data);
    }

    DynamicArray *files = NULL;
    DynamicArray *folders = NULL;

    if (db_has_entries_sorted_by_type(view->db, view->sort_order)) {
        files = db_get_files_sorted(view->db, view->sort_order);
        folders = db_get_folders_sorted(view->db, view->sort_order);
    }
    else {
        files = db_get_files(view->db);
        folders = db_get_folders(view->db);
    }

    FsearchQuery *q = fsearch_query_new(view->query_text,
                                        files,
                                        folders,
                                        view->sort_order,
                                        view->filter,
                                        view->db->thread_pool,
                                        view->query_flags,
                                        view->query_id++,
                                        view->id,
                                        view);

    if (fsearch_query_matches_everything(q)) {
        db_view_on_match_everything(view);
        if (view->query) {
            fsearch_query_free(view->query);
        }
        view->query = q;

        if (view->view_changed_func) {
            view->view_changed_func(view, view->user_data);
        }
        if (view->search_finished_func) {
            view->search_finished_func(view, view->user_data);
        }
    }
    else {
        db_search_queue(view->task_queue, q, db_view_task_query_finished, db_view_task_query_cancelled);
    }
}

void
db_view_set_filter(FsearchDatabaseView *view, FsearchFilter *filter) {
    if (!view) {
        return;
    }
    g_mutex_lock(&view->mutex);
    if (view->filter) {
        fsearch_filter_unref(view->filter);
    }
    view->filter = fsearch_filter_ref(filter);

    db_view_update_entries(view);

    g_mutex_unlock(&view->mutex);
}

FsearchQuery *
db_view_get_query(FsearchDatabaseView *view) {
    return view->query;
}

FsearchQueryFlags
db_view_get_query_flags(FsearchDatabaseView *view) {
    return view->query_flags;
}

void
db_view_set_query_flags(FsearchDatabaseView *view, FsearchQueryFlags query_flags) {
    if (!view) {
        return;
    }
    g_mutex_lock(&view->mutex);
    view->query_flags = query_flags;

    db_view_update_entries(view);

    g_mutex_unlock(&view->mutex);
}

void
db_view_set_query_text(FsearchDatabaseView *view, const char *query_text) {
    if (!view) {
        return;
    }
    g_mutex_lock(&view->mutex);
    if (view->query_text) {
        free(view->query_text);
    }
    view->query_text = strdup(query_text ? query_text : "");

    db_view_update_entries(view);

    g_mutex_unlock(&view->mutex);
}

void
db_view_set_sort_order(FsearchDatabaseView *view, FsearchDatabaseIndexType sort_order) {
    if (!view) {
        return;
    }
    g_mutex_lock(&view->mutex);
    bool needs_update = view->sort_order != sort_order;
    view->sort_order = sort_order;

    if (needs_update) {
        db_view_update_sort(view);
    }

    g_mutex_unlock(&view->mutex);
}

uint32_t
db_view_get_num_folders(FsearchDatabaseView *view) {
    assert(view != NULL);
    return view->folders ? darray_get_num_items(view->folders) : 0;
}

uint32_t
db_view_get_num_files(FsearchDatabaseView *view) {
    assert(view != NULL);
    return view->files ? darray_get_num_items(view->files) : 0;
}

uint32_t
db_view_get_num_entries(FsearchDatabaseView *view) {
    assert(view != NULL);
    return db_view_get_num_folders(view) + db_view_get_num_files(view);
}

FsearchDatabaseIndexType
db_view_get_sort_order(FsearchDatabaseView *view) {
    assert(view != NULL);
    return view->sort_order;
}

static void
db_sorted_entries_free(FsearchDatabase *db) {
    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_TYPES; i++) {
        DynamicArray *files = db->sorted_files[i];
        if (files) {
            darray_unref(files);
            files = NULL;
        }
        db->sorted_files[i] = NULL;

        DynamicArray *folders = db->sorted_folders[i];
        if (folders) {
            darray_unref(folders);
            folders = NULL;
        }
        db->sorted_folders[i] = NULL;
    }
}

static void
db_sort_entries(FsearchDatabase *db, DynamicArray *entries, DynamicArray **sorted_entries) {
    // first sort by path
    darray_sort_multi_threaded(entries, (DynamicArrayCompareFunc)db_entry_compare_entries_by_path);
    sorted_entries[DATABASE_INDEX_TYPE_PATH] = darray_copy(entries);

    // then by name
    darray_sort(entries, (DynamicArrayCompareFunc)db_entry_compare_entries_by_name);

    // now build individual lists sorted by all of the indexed metadata
    if ((db->index_flags & DATABASE_INDEX_FLAG_SIZE) != 0) {
        sorted_entries[DATABASE_INDEX_TYPE_SIZE] = darray_copy(entries);
        darray_sort_multi_threaded(sorted_entries[DATABASE_INDEX_TYPE_SIZE],
                                   (DynamicArrayCompareFunc)db_entry_compare_entries_by_size);
    }

    if ((db->index_flags & DATABASE_INDEX_FLAG_MODIFICATION_TIME) != 0) {
        sorted_entries[DATABASE_INDEX_TYPE_MODIFICATION_TIME] = darray_copy(entries);
        darray_sort_multi_threaded(sorted_entries[DATABASE_INDEX_TYPE_MODIFICATION_TIME],
                                   (DynamicArrayCompareFunc)db_entry_compare_entries_by_modification_time);
    }
}

static void
db_sort(FsearchDatabase *db) {
    assert(db != NULL);

    GTimer *timer = g_timer_new();

    // first we sort all the files
    DynamicArray *files = db->sorted_files[DATABASE_INDEX_TYPE_NAME];
    if (files) {
        db_sort_entries(db, files, db->sorted_files);

        const double seconds = g_timer_elapsed(timer, NULL);
        g_timer_reset(timer);
        g_debug("[db_sort] sorted files: %f s", seconds);
    }

    // then we sort all the folders
    DynamicArray *folders = db->sorted_folders[DATABASE_INDEX_TYPE_NAME];
    if (folders) {
        db_sort_entries(db, folders, db->sorted_folders);

        const double seconds = g_timer_elapsed(timer, NULL);
        g_debug("[db_sort] sorted folders: %f s", seconds);
    }

    g_timer_destroy(timer);
    timer = NULL;
}

static void
db_update_timestamp(FsearchDatabase *db) {
    assert(db != NULL);
    db->timestamp = time(NULL);
}

static void
db_entry_update_folder_indices(FsearchDatabase *db) {
    if (!db || !db->sorted_folders[DATABASE_INDEX_TYPE_NAME]) {
        return;
    }
    const uint32_t num_folders = darray_get_num_items(db->sorted_folders[DATABASE_INDEX_TYPE_NAME]);
    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = darray_get_item(db->sorted_folders[DATABASE_INDEX_TYPE_NAME], i);
        if (!folder) {
            continue;
        }
        db_entry_set_idx((FsearchDatabaseEntry *)folder, i);
    }
}

static uint8_t
get_name_offset(const char *old, const char *new) {
    uint8_t offset = 0;
    while (old[offset] == new[offset] && old[offset] != '\0' && new[offset] != '\0') {
        offset++;
    }
    return offset;
}

static FILE *
db_file_open_locked(const char *file_path, const char *mode) {
    FILE *file_pointer = fopen(file_path, mode);
    if (!file_pointer) {
        g_debug("[db_file] can't open database file: %s", file_path);
        return NULL;
    }

    int file_descriptor = fileno(file_pointer);
    if (flock(file_descriptor, LOCK_EX | LOCK_NB) == -1) {
        g_debug("[db_file] database file is already locked by a different process: %s", file_path);

        fclose(file_pointer);
        file_pointer = NULL;
    }

    return file_pointer;
}

static const uint8_t *
db_load_entry_shared_from_memory(const uint8_t *data_block,
                                 FsearchDatabaseIndexFlags index_flags,
                                 FsearchDatabaseEntry *entry,
                                 GString *previous_entry_name) {
    // name_offset: character position after which previous_entry_name and entry_name differ
    uint8_t name_offset = *data_block++;

    // name_len: length of the new name characters
    uint8_t name_len = *data_block++;

    // erase previous name starting at name_offset
    g_string_erase(previous_entry_name, name_offset, -1);

    char name[256] = "";
    // name: new characters to be appended to previous_entry_name
    if (name_len > 0) {
        memcpy(name, data_block, name_len);
        name[name_len] = '\0';
    }
    data_block += name_len;

    // now we can build the new full file name
    g_string_append(previous_entry_name, name);
    db_entry_set_name(entry, previous_entry_name->str);

    if ((index_flags & DATABASE_INDEX_FLAG_SIZE) != 0) {
        // size: size of file/folder
        off_t size = 0;
        memcpy(&size, data_block, 8);
        data_block += 8;

        db_entry_set_size(entry, size);
    }

    if ((index_flags & DATABASE_INDEX_FLAG_MODIFICATION_TIME) != 0) {
        // mtime: modification time file/folder
        time_t mtime = 0;
        memcpy(&mtime, data_block, 8);
        data_block += 8;

        db_entry_set_mtime(entry, mtime);
    }

    return data_block;
}

static bool
db_load_entry_shared(FILE *fp, FsearchDatabaseEntry *entry, GString *previous_entry_name) {
    // name_offset: character position after which previous_entry_name and entry_name differ
    uint8_t name_offset = 0;
    if (fread(&name_offset, 1, 1, fp) != 1) {
        g_debug("[db_load] failed to load name offset");
        return false;
    }

    // name_len: length of the new name characters
    uint8_t name_len = 0;
    if (fread(&name_len, 1, 1, fp) != 1) {
        g_debug("[db_load] failed to load name length");
        return false;
    }

    // erase previous name starting at name_offset
    g_string_erase(previous_entry_name, name_offset, -1);

    char name[256] = "";
    // name: new characters to be appended to previous_entry_name
    if (name_len > 0) {
        if (fread(name, name_len, 1, fp) != 1) {
            g_debug("[db_load] failed to load name");
            return false;
        }
        name[name_len] = '\0';
    }

    // now we can build the new full file name
    g_string_append(previous_entry_name, name);
    db_entry_set_name(entry, previous_entry_name->str);

    // size: size of file/folder
    uint64_t size = 0;
    if (fread(&size, 8, 1, fp) != 1) {
        g_debug("[db_load] failed to load size");
        return false;
    }
    db_entry_set_size(entry, (off_t)size);

    return true;
}

static bool
db_load_header(FILE *fp) {
    char magic[5] = "";
    if (fread(magic, strlen(DATABASE_MAGIC_NUMBER), 1, fp) != 1) {
        return false;
    }
    magic[4] = '\0';
    if (strcmp(magic, DATABASE_MAGIC_NUMBER) != 0) {
        g_debug("[db_load] invalid magic number: %s", magic);
        return false;
    }

    uint8_t majorver = 0;
    if (fread(&majorver, 1, 1, fp) != 1) {
        return false;
    }
    if (majorver != DATABASE_MAJOR_VERSION) {
        g_debug("[db_load] invalid major version: %d", majorver);
        return false;
    }

    uint8_t minorver = 0;
    if (fread(&minorver, 1, 1, fp) != 1) {
        return false;
    }
    if (minorver != DATABASE_MINOR_VERSION) {
        g_debug("[db_load] invalid minor version: %d", minorver);
        return false;
    }

    return true;
}

static bool
db_load_parent_idx(FILE *fp, uint32_t *parent_idx) {
    if (fread(parent_idx, 4, 1, fp) != 1) {
        g_debug("[db_load] failed to load parent_idx");
        return false;
    }
    return true;
}

static bool
db_load_folders(FILE *fp,
                FsearchDatabaseIndexFlags index_flags,
                DynamicArray *folders,
                uint32_t num_folders,
                uint64_t folder_block_size) {
    bool res = true;

    GString *previous_entry_name = g_string_sized_new(256);

    uint8_t *folder_block = calloc(folder_block_size + 1, sizeof(uint8_t));
    assert(folder_block != NULL);

    if (fread(folder_block, sizeof(uint8_t), folder_block_size, fp) != folder_block_size) {
        g_debug("[db_load] failed to read file block");
        goto out;
    }

    const uint8_t *fb = folder_block;
    // load folders
    uint32_t idx = 0;
    for (idx = 0; idx < num_folders; idx++) {
        FsearchDatabaseEntryFolder *folder = darray_get_item(folders, idx);
        FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)folder;

        fb = db_load_entry_shared_from_memory(fb, index_flags, entry, previous_entry_name);

        // parent_idx: index of parent folder
        uint32_t parent_idx = 0;
        memcpy(&parent_idx, fb, 4);
        fb += 4;

        if (parent_idx != db_entry_get_idx(entry)) {
            FsearchDatabaseEntryFolder *parent = darray_get_item(folders, parent_idx);
            db_entry_set_parent(entry, parent);
        }
        else {
            // parent_idx and idx are the same (i.e. folder is a root index) so it has no parent
            db_entry_set_parent(entry, NULL);
        }
    }

    // fail if we didn't read the correct number of bytes
    if (fb - folder_block != folder_block_size) {
        g_debug("[db_load] wrong amount of memory read: %lu != %lu", fb - folder_block, folder_block_size);
        res = false;
        goto out;
    }

    // fail if we didn't read the correct number of folders
    if (idx != num_folders) {
        g_debug("[db_load] failed to read folders (read %d of %d)", idx, num_folders);
        res = false;
    }

out:
    free(folder_block);
    folder_block = NULL;

    g_string_free(previous_entry_name, TRUE);
    previous_entry_name = NULL;

    return res;
}

static bool
db_load_files(FILE *fp,
              FsearchDatabaseIndexFlags index_flags,
              FsearchMemoryPool *pool,
              DynamicArray *folders,
              DynamicArray *files,
              uint32_t num_files,
              uint64_t file_block_size) {
    bool result = true;
    GString *previous_entry_name = g_string_sized_new(256);

    uint8_t *file_block = calloc(file_block_size + 1, sizeof(uint8_t));
    assert(file_block != NULL);

    if (fread(file_block, sizeof(uint8_t), file_block_size, fp) != file_block_size) {
        g_debug("[db_load] failed to read file block");
        goto out;
    }

    const uint8_t *fb = file_block;
    // load folders
    uint32_t idx = 0;
    for (idx = 0; idx < num_files; idx++) {
        FsearchDatabaseEntryFile *file = fsearch_memory_pool_malloc(pool);
        FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)file;
        db_entry_set_type(entry, DATABASE_ENTRY_TYPE_FILE);
        db_entry_set_idx(entry, idx);

        fb = db_load_entry_shared_from_memory(fb, index_flags, entry, previous_entry_name);

        // parent_idx: index of parent folder
        uint32_t parent_idx = 0;
        memcpy(&parent_idx, fb, 4);
        fb += 4;

        FsearchDatabaseEntryFolder *parent = darray_get_item(folders, parent_idx);
        db_entry_set_parent(entry, parent);

        darray_add_item(files, file);
    }
    if (fb - file_block != file_block_size) {
        g_debug("[db_load] wrong amount of memory read: %lu != %lu", fb - file_block, file_block_size);
        goto out;
    }

    // fail if we didn't read the correct number of files
    if (result && idx != num_files) {
        g_debug("[db_load] failed to read files (read %d of %d)", idx, num_files);
        result = false;
    }

out:
    free(file_block);
    file_block = NULL;

    g_string_free(previous_entry_name, TRUE);
    previous_entry_name = NULL;

    return result;
}

static bool
db_load_sorted_entries(FILE *fp, DynamicArray *src, uint32_t num_src_entries, DynamicArray *dest) {

    uint32_t *indexes = calloc(num_src_entries + 1, sizeof(uint32_t));
    assert(indexes != NULL);

    bool res = true;

    if (fread(indexes, 4, num_src_entries, fp) != num_src_entries) {
        res = false;
    }
    else {
        for (uint32_t i = 0; i < num_src_entries; i++) {
            uint32_t idx = indexes[i];
            void *entry = darray_get_item(src, idx);
            if (!entry) {
                return false;
            }
            darray_add_item(dest, entry);
        }
    }

    free(indexes);
    indexes = NULL;

    return res;
}

static bool
db_load_sorted_arrays(FILE *fp, DynamicArray **sorted_folders, DynamicArray **sorted_files) {
    uint32_t num_sorted_arrays = 0;

    DynamicArray *files = sorted_files[0];
    DynamicArray *folders = sorted_folders[0];

    if (fread(&num_sorted_arrays, 4, 1, fp) != 1) {
        g_debug("[db_load] failed to load number of sorted arrays");
        return false;
    }

    for (uint32_t i = 0; i < num_sorted_arrays; i++) {
        uint32_t sorted_array_id = 0;
        if (fread(&sorted_array_id, 4, 1, fp) != 1) {
            g_debug("[db_load] failed to load sorted array id");
            return false;
        }

        if (sorted_array_id < 1 || sorted_array_id >= NUM_DATABASE_INDEX_TYPES) {
            g_debug("[db_load] sorted array id is not supported: %d", sorted_array_id);
            return false;
        }

        const uint32_t num_folders = darray_get_num_items(folders);
        sorted_folders[sorted_array_id] = darray_new(num_folders);
        if (!db_load_sorted_entries(fp, folders, num_folders, sorted_folders[sorted_array_id])) {
            g_debug("[db_load] failed to load sorted folder indexes: %d", sorted_array_id);
            return false;
        }

        const uint32_t num_files = darray_get_num_items(files);
        sorted_files[sorted_array_id] = darray_new(num_files);
        if (!db_load_sorted_entries(fp, files, num_files, sorted_files[sorted_array_id])) {
            g_debug("[db_load] failed to load sorted file indexes: %d", sorted_array_id);
            return false;
        }
    }

    return true;
}

bool
db_load(FsearchDatabase *db, const char *file_path, void (*status_cb)(const char *)) {
    assert(file_path != NULL);
    assert(db != NULL);

    FILE *fp = db_file_open_locked(file_path, "rb");
    if (!fp) {
        return false;
    }

    DynamicArray *folders = NULL;
    DynamicArray *files = NULL;
    DynamicArray *sorted_folders[NUM_DATABASE_INDEX_TYPES] = {NULL};
    DynamicArray *sorted_files[NUM_DATABASE_INDEX_TYPES] = {NULL};

    if (!db_load_header(fp)) {
        goto load_fail;
    }

    uint64_t index_flags = 0;
    if (fread(&index_flags, 8, 1, fp) != 1) {
        goto load_fail;
    }

    uint32_t num_folders = 0;
    if (fread(&num_folders, 4, 1, fp) != 1) {
        goto load_fail;
    }

    uint32_t num_files = 0;
    if (fread(&num_files, 4, 1, fp) != 1) {
        goto load_fail;
    }
    g_debug("[db_load] load %d folders, %d files", num_folders, num_files);

    uint64_t folder_block_size = 0;
    if (fread(&folder_block_size, 8, 1, fp) != 1) {
        goto load_fail;
    }

    uint64_t file_block_size = 0;
    if (fread(&file_block_size, 8, 1, fp) != 1) {
        goto load_fail;
    }
    g_debug("[db_load] folder size: %lu, file size: %lu", folder_block_size, file_block_size);

    // pre-allocate the folders array so we can later map parent indices to the corresponding pointers
    sorted_folders[DATABASE_INDEX_TYPE_NAME] = darray_new(num_folders);
    folders = sorted_folders[DATABASE_INDEX_TYPE_NAME];

    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = fsearch_memory_pool_malloc(db->folder_pool);
        FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)folder;
        db_entry_set_idx(entry, i);
        db_entry_set_type(entry, DATABASE_ENTRY_TYPE_FOLDER);
        db_entry_set_parent(entry, NULL);
        darray_add_item(folders, folder);
    }

    if (status_cb) {
        status_cb(_("Loading folders…"));
    }
    // load folders
    if (!db_load_folders(fp, index_flags, folders, num_folders, folder_block_size)) {
        goto load_fail;
    }

    if (status_cb) {
        status_cb(_("Loading files…"));
    }
    // load files
    sorted_files[DATABASE_INDEX_TYPE_NAME] = darray_new(num_files);
    files = sorted_files[DATABASE_INDEX_TYPE_NAME];
    if (!db_load_files(fp, index_flags, db->file_pool, folders, files, num_files, file_block_size)) {
        goto load_fail;
    }

    if (!db_load_sorted_arrays(fp, sorted_folders, sorted_files)) {
        goto load_fail;
    }

    db_sorted_entries_free(db);

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_TYPES; i++) {
        db->sorted_files[i] = sorted_files[i];
        db->sorted_folders[i] = sorted_folders[i];
    }

    db->num_entries = num_files + num_folders;
    db->num_files = num_files;
    db->num_folders = num_folders;
    db->index_flags = index_flags;

    fclose(fp);
    fp = NULL;

    return true;

load_fail:
    g_debug("[db_load] load failed");

    if (fp) {
        fclose(fp);
        fp = NULL;
    }

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_TYPES; i++) {
        if (sorted_folders[i]) {
            darray_unref(sorted_folders[i]);
        }
        sorted_folders[i] = NULL;

        if (sorted_files[i]) {
            darray_unref(sorted_files[i]);
        }
        sorted_files[i] = NULL;
    }

    return false;
}

size_t
write_data_to_file(FILE *fp, const void *data, size_t data_size, size_t num_elements, bool *write_failed) {
    if (data_size == 0 || num_elements == 0) {
        return 0;
    }
    if (fwrite(data, data_size, num_elements, fp) != num_elements) {
        *write_failed = true;
        return 0;
    }
    return data_size * num_elements;
}

static size_t
db_save_entry_shared(FILE *fp,
                     FsearchDatabaseIndexFlags index_flags,
                     FsearchDatabaseEntry *entry,
                     uint32_t parent_idx,
                     GString *previous_entry_name,
                     GString *new_entry_name,
                     bool *write_failed) {
    // init new_entry_name with the name of the current entry
    g_string_erase(new_entry_name, 0, -1);
    g_string_append(new_entry_name, db_entry_get_name_raw(entry));

    size_t bytes_written = 0;
    // name_offset: character position after which previous_entry_name and new_entry_name differ
    const uint8_t name_offset = get_name_offset(previous_entry_name->str, new_entry_name->str);
    bytes_written += write_data_to_file(fp, &name_offset, 1, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save name offset");
        goto out;
    }

    // name_len: length of the new name characters
    const uint8_t name_len = new_entry_name->len - name_offset;
    bytes_written += write_data_to_file(fp, &name_len, 1, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save name length");
        goto out;
    }

    // append new unique characters to previous_entry_name starting at name_offset
    g_string_erase(previous_entry_name, name_offset, -1);
    g_string_append(previous_entry_name, new_entry_name->str + name_offset);

    if (name_len > 0) {
        // name: new characters to be written to file
        const char *name = previous_entry_name->str + name_offset;
        bytes_written += write_data_to_file(fp, name, name_len, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save name");
            goto out;
        }
    }

    if ((index_flags & DATABASE_INDEX_FLAG_SIZE) != 0) {
        // size: file or folder size (folder size: sum of all children sizes)
        const uint64_t size = db_entry_get_size(entry);
        bytes_written += write_data_to_file(fp, &size, 8, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save size");
            goto out;
        }
    }

    if ((index_flags & DATABASE_INDEX_FLAG_MODIFICATION_TIME) != 0) {
        // mtime: modification time of file/folder
        const uint64_t mtime = db_entry_get_mtime(entry);
        bytes_written += write_data_to_file(fp, &mtime, 8, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save modification time");
            goto out;
        }
    }

    // parent_idx: index of parent folder
    bytes_written += write_data_to_file(fp, &parent_idx, 4, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save parent_idx");
        goto out;
    }

out:
    return bytes_written;
}

static size_t
db_save_header(FILE *fp, bool *write_failed) {
    size_t bytes_written = 0;

    const char magic[] = DATABASE_MAGIC_NUMBER;
    bytes_written += write_data_to_file(fp, magic, strlen(magic), 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save magic number");
        goto out;
    }

    const uint8_t majorver = DATABASE_MAJOR_VERSION;
    bytes_written += write_data_to_file(fp, &majorver, 1, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save major version number");
        goto out;
    }

    const uint8_t minorver = DATABASE_MINOR_VERSION;
    bytes_written += write_data_to_file(fp, &minorver, 1, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save minor version number");
        goto out;
    }

out:
    return bytes_written;
}

static size_t
db_save_files(FILE *fp,
              FsearchDatabaseIndexFlags index_flags,
              DynamicArray *files,
              uint32_t num_files,
              bool *write_failed) {
    size_t bytes_written = 0;

    GString *name_prev = g_string_sized_new(256);
    GString *name_new = g_string_sized_new(256);

    for (uint32_t i = 0; i < num_files; i++) {
        FsearchDatabaseEntryFile *file = darray_get_item(files, i);
        FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)file;

        // let's also update the idx of the file here while we're at it to make sure we have the correct
        // idx set when we store the fast sort indexes
        db_entry_set_idx(entry, i);

        FsearchDatabaseEntryFolder *parent = db_entry_get_parent(entry);
        const uint32_t parent_idx = db_entry_get_idx((FsearchDatabaseEntry *)parent);
        bytes_written += db_save_entry_shared(fp, index_flags, entry, parent_idx, name_prev, name_new, write_failed);
        if (*write_failed == true)
            goto out;
    }

out:
    g_string_free(name_prev, TRUE);
    name_prev = NULL;

    g_string_free(name_new, TRUE);
    name_new = NULL;

    return bytes_written;
}

static uint32_t *
build_sorted_entry_index_list(DynamicArray *entries, uint32_t num_entries) {
    if (num_entries < 1) {
        return NULL;
    }
    uint32_t *indexes = calloc(num_entries + 1, sizeof(uint32_t));
    assert(indexes != NULL);

    for (int i = 0; i < num_entries; i++) {
        FsearchDatabaseEntry *entry = darray_get_item(entries, i);
        indexes[i] = db_entry_get_idx(entry);
    }
    return indexes;
}

static size_t
db_save_sorted_entries(FILE *fp, DynamicArray *entries, uint32_t num_entries, bool *write_failed) {
    size_t bytes_written = 0;
    uint32_t *sorted_entry_index_list = NULL;
    if (num_entries < 1) {
        // nothing to write, we're done here
        goto out;
    }

    sorted_entry_index_list = build_sorted_entry_index_list(entries, num_entries);
    if (!sorted_entry_index_list) {
        *write_failed = true;
        g_debug("[db_save] failed to create sorted index list");
        goto out;
    }

    bytes_written += write_data_to_file(fp, sorted_entry_index_list, 4, num_entries, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save sorted index list");
        goto out;
    }

out:
    if (sorted_entry_index_list) {
        free(sorted_entry_index_list);
        sorted_entry_index_list = NULL;
    }
    return bytes_written;
}

static size_t
db_save_sorted_arrays(FILE *fp, FsearchDatabase *db, uint32_t num_files, uint32_t num_folders, bool *write_failed) {
    size_t bytes_written = 0;
    uint32_t num_sorted_arrays = 0;
    for (uint32_t i = 1; i < NUM_DATABASE_INDEX_TYPES; i++) {
        if (db->sorted_folders[i] && db->sorted_files[i]) {
            num_sorted_arrays++;
        }
    }

    bytes_written += write_data_to_file(fp, &num_sorted_arrays, 4, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save number of sorted arrays: %d", num_sorted_arrays);
        goto out;
    }

    if (num_sorted_arrays < 1) {
        goto out;
    }

    for (uint32_t id = 1; id < NUM_DATABASE_INDEX_TYPES; id++) {
        DynamicArray *folders = db->sorted_folders[id];
        DynamicArray *files = db->sorted_files[id];
        if (!files || !folders) {
            continue;
        }

        // id: this is the id of the sorted files
        bytes_written += write_data_to_file(fp, &id, 4, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save sorted arrays id: %d", id);
            goto out;
        }

        bytes_written += db_save_sorted_entries(fp, folders, num_folders, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save sorted folders");
            goto out;
        }
        bytes_written += db_save_sorted_entries(fp, files, num_files, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save sorted files");
            goto out;
        }
    }

out:
    return bytes_written;
}

static size_t
db_save_folders(FILE *fp,
                FsearchDatabaseIndexFlags index_flags,
                DynamicArray *folders,
                uint32_t num_folders,
                bool *write_failed) {
    size_t bytes_written = 0;

    GString *name_prev = g_string_sized_new(256);
    GString *name_new = g_string_sized_new(256);

    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = darray_get_item(folders, i);
        FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)folder;

        FsearchDatabaseEntryFolder *parent = db_entry_get_parent(entry);
        const uint32_t parent_idx = parent ? db_entry_get_idx((FsearchDatabaseEntry *)parent) : db_entry_get_idx(entry);
        bytes_written += db_save_entry_shared(fp, index_flags, entry, parent_idx, name_prev, name_new, write_failed);
        if (*write_failed == true) {
            goto out;
        }
    }

out:
    g_string_free(name_prev, TRUE);
    name_prev = NULL;

    g_string_free(name_new, TRUE);
    name_new = NULL;

    return bytes_written;
}

static size_t
db_save_indexes(FILE *fp, FsearchDatabase *db, bool *write_failed) {
    // TODO
    return 0;
}

static size_t
db_save_excludes(FILE *fp, FsearchDatabase *db, bool *write_failed) {
    // TODO
    return 0;
}

static size_t
db_save_exclude_pattern(FILE *fp, FsearchDatabase *db, bool *write_failed) {
    // TODO
    return 0;
}

bool
db_save(FsearchDatabase *db, const char *path) {
    assert(path != NULL);
    assert(db != NULL);

    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_debug("[db_save] database path doesn't exist: %s", path);
        return false;
    }

    GTimer *timer = g_timer_new();
    g_timer_start(timer);

    GString *path_full = g_string_new(path);
    g_string_append_c(path_full, G_DIR_SEPARATOR);
    g_string_append(path_full, "fsearch.db");

    GString *path_full_temp = g_string_new(path_full->str);
    g_string_append(path_full_temp, ".tmp");

    FILE *fp = db_file_open_locked(path_full_temp->str, "wb");
    if (!fp) {
        goto save_fail;
    }

    db_entry_update_folder_indices(db);

    bool write_failed = false;

    size_t bytes_written = 0;

    bytes_written += db_save_header(fp, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    const uint64_t index_flags = db->index_flags;
    bytes_written += write_data_to_file(fp, &index_flags, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    DynamicArray *files = db->sorted_files[DATABASE_INDEX_TYPE_NAME];
    DynamicArray *folders = db->sorted_folders[DATABASE_INDEX_TYPE_NAME];

    const uint32_t num_folders = darray_get_num_items(folders);
    bytes_written += write_data_to_file(fp, &num_folders, 4, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    const uint32_t num_files = darray_get_num_items(files);
    bytes_written += write_data_to_file(fp, &num_files, 4, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    uint64_t folder_block_size = 0;
    const uint64_t folder_block_size_offset = bytes_written;
    bytes_written += write_data_to_file(fp, &folder_block_size, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    uint64_t file_block_size = 0;
    const uint64_t file_block_size_offset = bytes_written;
    bytes_written += write_data_to_file(fp, &file_block_size, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    bytes_written += db_save_indexes(fp, db, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }
    bytes_written += db_save_excludes(fp, db, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }
    bytes_written += db_save_exclude_pattern(fp, db, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }
    folder_block_size = db_save_folders(fp, index_flags, folders, num_folders, &write_failed);
    bytes_written += folder_block_size;
    if (write_failed == true) {
        goto save_fail;
    }
    file_block_size = db_save_files(fp, index_flags, files, num_files, &write_failed);
    bytes_written += file_block_size;
    if (write_failed == true) {
        goto save_fail;
    }
    bytes_written += db_save_sorted_arrays(fp, db, num_files, num_folders, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    // now that we know the size of the file/folder block we've written, store it in the file header
    if (fseek(fp, (long int)folder_block_size_offset, SEEK_SET) != 0) {
        goto save_fail;
    }
    bytes_written += write_data_to_file(fp, &folder_block_size, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }
    bytes_written += write_data_to_file(fp, &file_block_size, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    // remove current database file
    unlink(path_full->str);

    fclose(fp);
    fp = NULL;

    // rename temporary fsearch.db.tmp to fsearch.db
    if (rename(path_full_temp->str, path_full->str) != 0) {
        goto save_fail;
    }

    g_string_free(path_full, TRUE);
    path_full = NULL;

    g_string_free(path_full_temp, TRUE);
    path_full_temp = NULL;

    const double seconds = g_timer_elapsed(timer, NULL);
    g_timer_stop(timer);
    g_timer_destroy(timer);
    timer = NULL;

    g_debug("[db_save] database file saved in: %f ms", seconds * 1000);

    return true;

save_fail:
    g_warning("save fail");

    if (fp) {
        fclose(fp);
        fp = NULL;
    }

    // remove temporary fsearch.db.tmp file
    unlink(path_full_temp->str);

    g_string_free(path_full, TRUE);
    path_full = NULL;

    g_string_free(path_full_temp, TRUE);
    path_full_temp = NULL;

    g_timer_destroy(timer);
    timer = NULL;

    return false;
}

static bool
file_is_excluded(const char *name, char **exclude_files) {
    if (exclude_files) {
        for (int i = 0; exclude_files[i]; ++i) {
            if (!fnmatch(exclude_files[i], name, 0)) {
                return true;
            }
        }
    }
    return false;
}

static bool
directory_is_excluded(const char *name, GList *excludes) {
    while (excludes) {
        FsearchExcludePath *fs_path = excludes->data;
        if (!strcmp(name, fs_path->path)) {
            if (fs_path->enabled) {
                return true;
            }
            return false;
        }
        excludes = excludes->next;
    }
    return false;
}

typedef struct DatabaseWalkContext {
    FsearchDatabase *db;
    GString *path;
    GTimer *timer;
    GCancellable *cancellable;
    void (*status_cb)(const char *);
    bool exclude_hidden;
} DatabaseWalkContext;

static int
db_folder_scan_recursive(DatabaseWalkContext *walk_context, FsearchDatabaseEntryFolder *parent) {
    if (walk_context->cancellable && g_cancellable_is_cancelled(walk_context->cancellable)) {
        return WALK_CANCEL;
    }

    GString *path = walk_context->path;
    g_string_append_c(path, G_DIR_SEPARATOR);

    // remember end of parent path
    const gsize path_len = path->len;

    DIR *dir = NULL;
    if (!(dir = opendir(path->str))) {
        return WALK_BADIO;
    }

    const double elapsed_seconds = g_timer_elapsed(walk_context->timer, NULL);
    if (elapsed_seconds > 0.1) {
        if (walk_context->status_cb) {
            walk_context->status_cb(path->str);
        }
        g_timer_start(walk_context->timer);
    }

    FsearchDatabase *db = walk_context->db;

    struct dirent *dent = NULL;
    while ((dent = readdir(dir))) {
        if (walk_context->cancellable && g_cancellable_is_cancelled(walk_context->cancellable)) {
            closedir(dir);
            return WALK_CANCEL;
        }
        if (walk_context->exclude_hidden && dent->d_name[0] == '.') {
            // file is dotfile, skip
            continue;
        }
        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) {
            continue;
        }
        if (file_is_excluded(dent->d_name, db->exclude_files)) {
            continue;
        }

        // create full path of file/folder
        g_string_truncate(path, path_len);
        g_string_append(path, dent->d_name);

        struct stat st;
        if (lstat(path->str, &st) == -1) {
            // warn("Can't stat %s", fn);
            continue;
        }

        const bool is_dir = S_ISDIR(st.st_mode);
        if (is_dir && directory_is_excluded(path->str, db->excludes)) {
            g_debug("[db_scan] excluded directory: %s", path->str);
            continue;
        }

        if (is_dir) {
            FsearchDatabaseEntryFolder *folder_entry = fsearch_memory_pool_malloc(db->folder_pool);
            FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)folder_entry;
            db_entry_set_name(entry, dent->d_name);
            db_entry_set_type(entry, DATABASE_ENTRY_TYPE_FOLDER);
            db_entry_set_mtime(entry, st.st_mtime);
            db_entry_set_parent(entry, parent);

            darray_add_item(db->sorted_folders[DATABASE_INDEX_TYPE_NAME], folder_entry);

            db->num_folders++;

            db_folder_scan_recursive(walk_context, folder_entry);
        }
        else {
            FsearchDatabaseEntryFile *file_entry = fsearch_memory_pool_malloc(db->file_pool);
            db_entry_set_name(file_entry, dent->d_name);
            db_entry_set_size(file_entry, st.st_size);
            db_entry_set_mtime(file_entry, st.st_mtime);
            db_entry_set_type(file_entry, DATABASE_ENTRY_TYPE_FILE);
            db_entry_set_parent(file_entry, parent);
            db_entry_update_parent_size(file_entry);

            darray_add_item(db->sorted_files[DATABASE_INDEX_TYPE_NAME], file_entry);

            db->num_files++;
        }

        db->num_entries++;
    }

    closedir(dir);
    return WALK_OK;
}

static void
db_scan_folder(FsearchDatabase *db, const char *dname, GCancellable *cancellable, void (*status_cb)(const char *)) {
    assert(dname != NULL);
    assert(dname[0] == G_DIR_SEPARATOR);
    g_debug("[db_scan] scan path: %s", dname);

    if (!g_file_test(dname, G_FILE_TEST_IS_DIR)) {
        g_warning("[db_scan] %s doesn't exist", dname);
        return;
    }

    GString *path = g_string_new(dname);
    // remove leading path separator '/' for root directory
    if (strcmp(path->str, G_DIR_SEPARATOR_S) == 0) {
        g_string_erase(path, 0, 1);
    }

    GTimer *timer = g_timer_new();
    g_timer_start(timer);
    DatabaseWalkContext walk_context = {
        .db = db,
        .path = path,
        .timer = timer,
        .cancellable = cancellable,
        .status_cb = status_cb,
        .exclude_hidden = db->exclude_hidden,
    };

    FsearchDatabaseEntryFolder *parent = fsearch_memory_pool_malloc(db->folder_pool);
    FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)parent;
    db_entry_set_name(entry, path->str);
    db_entry_set_parent(entry, NULL);
    db_entry_set_type(entry, DATABASE_ENTRY_TYPE_FOLDER);

    darray_add_item(db->sorted_folders[DATABASE_INDEX_TYPE_NAME], parent);
    db->num_folders++;
    db->num_entries++;

    uint32_t res = db_folder_scan_recursive(&walk_context, parent);

    g_string_free(path, TRUE);
    g_timer_destroy(timer);
    if (res == WALK_OK) {
        g_debug("[db_scan] scanned: %d files, %d files -> %d total", db->num_files, db->num_folders, db->num_entries);
        return;
    }

    g_warning("[db_scan] walk error: %d", res);
}

static gint
compare_index_path(FsearchIndex *p1, FsearchIndex *p2) {
    return strcmp(p1->path, p2->path);
}

static gint
compare_exclude_path(FsearchExcludePath *p1, FsearchExcludePath *p2) {
    return strcmp(p1->path, p2->path);
}

FsearchDatabase *
db_new(GList *indexes, GList *excludes, char **exclude_files, bool exclude_hidden) {
    FsearchDatabase *db = g_new0(FsearchDatabase, 1);
    g_mutex_init(&db->mutex);
    if (indexes) {
        db->indexes = g_list_copy_deep(indexes, (GCopyFunc)fsearch_index_copy, NULL);
        db->indexes = g_list_sort(db->indexes, (GCompareFunc)compare_index_path);
    }
    if (excludes) {
        db->excludes = g_list_copy_deep(excludes, (GCopyFunc)fsearch_exclude_path_copy, NULL);
        db->excludes = g_list_sort(db->excludes, (GCompareFunc)compare_exclude_path);
    }
    if (exclude_files) {
        db->exclude_files = g_strdupv(exclude_files);
    }

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_TYPES; i++) {
        db->sorted_files[i] = NULL;
        db->sorted_folders[i] = NULL;
    }
    db->file_pool = fsearch_memory_pool_new(NUM_DB_ENTRIES_FOR_POOL_BLOCK,
                                            db_entry_get_sizeof_file_entry(),
                                            (GDestroyNotify)db_file_entry_destroy);
    db->folder_pool = fsearch_memory_pool_new(NUM_DB_ENTRIES_FOR_POOL_BLOCK,
                                              db_entry_get_sizeof_folder_entry(),
                                              (GDestroyNotify)db_folder_entry_destroy);

    db->thread_pool = fsearch_thread_pool_init();

    db->exclude_hidden = exclude_hidden;
    db->ref_count = 1;
    return db;
}

static void
db_free(FsearchDatabase *db) {
    assert(db != NULL);

    g_debug("[db_free] freeing...");
    db_lock(db);
    if (db->ref_count > 0) {
        g_warning("[db_free] pending references on free: %d", db->ref_count);
    }

    db_sorted_entries_free(db);

    if (db->file_pool) {
        fsearch_memory_pool_free(db->file_pool);
        db->file_pool = NULL;
    }

    if (db->folder_pool) {
        fsearch_memory_pool_free(db->folder_pool);
        db->folder_pool = NULL;
    }

    if (db->indexes) {
        g_list_free_full(db->indexes, (GDestroyNotify)fsearch_index_free);
        db->indexes = NULL;
    }
    if (db->excludes) {
        g_list_free_full(db->excludes, (GDestroyNotify)fsearch_exclude_path_free);
        db->excludes = NULL;
    }
    if (db->exclude_files) {
        g_strfreev(db->exclude_files);
        db->exclude_files = NULL;
    }
    if (db->thread_pool) {
        fsearch_thread_pool_free(db->thread_pool);
        db->thread_pool = NULL;
    }
    db_unlock(db);

    g_mutex_clear(&db->mutex);
    g_free(db);
    db = NULL;

    malloc_trim(0);

    g_debug("[db_free] freed");
    return;
}

time_t
db_get_timestamp(FsearchDatabase *db) {
    assert(db != NULL);
    return db->timestamp;
}

uint32_t
db_get_num_files(FsearchDatabase *db) {
    assert(db != NULL);
    return db->num_files;
}

uint32_t
db_get_num_folders(FsearchDatabase *db) {
    assert(db != NULL);
    return db->num_folders;
}

uint32_t
db_get_num_entries(FsearchDatabase *db) {
    assert(db != NULL);
    return db->num_entries;
}

void
db_unlock(FsearchDatabase *db) {
    assert(db != NULL);
    g_mutex_unlock(&db->mutex);
}

void
db_lock(FsearchDatabase *db) {
    assert(db != NULL);
    g_mutex_lock(&db->mutex);
}

bool
db_try_lock(FsearchDatabase *db) {
    assert(db != NULL);
    return g_mutex_trylock(&db->mutex);
}

static bool
is_valid_sort_type(FsearchDatabaseIndexType sort_type) {
    if (0 <= sort_type && sort_type < NUM_DATABASE_INDEX_TYPES) {
        return true;
    }
    return false;
}

bool
db_has_entries_sorted_by_type(FsearchDatabase *db, FsearchDatabaseIndexType sort_type) {
    assert(db != NULL);

    if (is_valid_sort_type(sort_type)) {
        return db->sorted_folders[sort_type] ? true : false;
    }
    return false;
}

DynamicArray *
db_get_folders_sorted_copy(FsearchDatabase *db, FsearchDatabaseIndexType sort_type) {
    assert(db != NULL);
    if (!is_valid_sort_type(sort_type)) {
        return NULL;
    }
    DynamicArray *folders = db->sorted_folders[sort_type];
    return folders ? darray_copy(folders) : NULL;
}

DynamicArray *
db_get_files_sorted_copy(FsearchDatabase *db, FsearchDatabaseIndexType sort_type) {
    assert(db != NULL);
    if (!is_valid_sort_type(sort_type)) {
        return NULL;
    }
    DynamicArray *files = db->sorted_files[sort_type];
    return files ? darray_copy(files) : NULL;
}

DynamicArray *
db_get_folders_copy(FsearchDatabase *db) {
    return db_get_folders_sorted_copy(db, DATABASE_INDEX_TYPE_NAME);
}

DynamicArray *
db_get_files_copy(FsearchDatabase *db) {
    return db_get_files_sorted_copy(db, DATABASE_INDEX_TYPE_NAME);
}

DynamicArray *
db_get_folders_sorted(FsearchDatabase *db, FsearchDatabaseIndexType sort_type) {
    assert(db != NULL);
    if (!is_valid_sort_type(sort_type)) {
        return NULL;
    }

    DynamicArray *folders = db->sorted_folders[sort_type];
    return darray_ref(folders);
}

DynamicArray *
db_get_files_sorted(FsearchDatabase *db, FsearchDatabaseIndexType sort_type) {
    assert(db != NULL);
    if (!is_valid_sort_type(sort_type)) {
        return NULL;
    }

    DynamicArray *files = db->sorted_files[sort_type];
    return darray_ref(files);
}

DynamicArray *
db_get_files(FsearchDatabase *db) {
    assert(db != NULL);
    return db_get_files_sorted(db, DATABASE_INDEX_TYPE_NAME);
}

DynamicArray *
db_get_folders(FsearchDatabase *db) {
    assert(db != NULL);
    return db_get_folders_sorted(db, DATABASE_INDEX_TYPE_NAME);
}

bool
db_scan(FsearchDatabase *db, GCancellable *cancellable, void (*status_cb)(const char *)) {
    assert(db != NULL);

    bool ret = false;

    db_sorted_entries_free(db);

    db->index_flags |= DATABASE_INDEX_FLAG_NAME;
    db->index_flags |= DATABASE_INDEX_FLAG_SIZE;
    db->index_flags |= DATABASE_INDEX_FLAG_MODIFICATION_TIME;

    db->sorted_files[DATABASE_INDEX_TYPE_NAME] = darray_new(1024);
    db->sorted_folders[DATABASE_INDEX_TYPE_NAME] = darray_new(1024);

    for (GList *l = db->indexes; l != NULL; l = l->next) {
        FsearchIndex *fs_path = l->data;
        if (!fs_path->path) {
            continue;
        }
        if (!fs_path->enabled) {
            continue;
        }
        if (fs_path->update) {
            db_scan_folder(db, fs_path->path, cancellable, status_cb);
        }
    }
    db_sort(db);
    return ret;
}

FsearchDatabase *
db_ref(FsearchDatabase *db) {
    if (!db) {
        return NULL;
    }
    db_lock(db);
    db->ref_count++;
    db_unlock(db);
    g_debug("[db_ref] increased to: %d", db->ref_count);
    return db;
}

void
db_unref(FsearchDatabase *db) {
    assert(db != NULL);
    db_lock(db);
    db->ref_count--;
    db_unlock(db);
    g_debug("[db_unref] dropped to: %d", db->ref_count);
    if (db->ref_count <= 0) {
        db_free(db);
    }
}
