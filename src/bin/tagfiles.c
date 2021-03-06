/*
 * Copyright (C) 2014 Hendrik Siedelmann <hendrik.siedelmann@googlemail.com>
 *
 * This file is part of lime.
 * 
 * Lime is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Lime is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Lime.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <Eina.h>
#include <Eio.h>
#include <exempi/xmp.h>
#include <exempi/xmpconsts.h>
#include <assert.h>
#include <Ecore.h>
#include <fcntl.h>
#include <Efreet.h>

#include "tagfiles.h"

#define MAX_XMP_FILE 1024*1024

#define GROUP_SCANNING 0
#define GROUP_COMPLETE 1
#define GROUP_LOADED   2

struct _Tagged_File {
  const char *dirname;
  const char *filename;
  const char *sidecar;
  char *last_fc; //serialization of the last filter chain
  //Eina_Array *tags;
  void *data;
};

struct _File_Group {
  int state;
  Tagfiles *tagfiles;
  Eina_Inarray *files; //tagged files
  //const char *sidecar; //sidecar file used for the image group
  const char *basename;
  Eina_Hash *tags; //all tags of the group
  int32_t tag_rating;
  //Eina_Array *group_tags; //tags that are assigned specifically to the group
};

typedef struct {
  char *path;
  Tagfiles *tagfiles;
} Ls_Info;

typedef struct {
  File_Group *group;
  void (*cb)(File_Group *group);
} Group_Changed_Cb_Data;

typedef struct {
  Eina_Array *tags;
  File_Group *group;
} Scanner_Feedback_Data;

struct _Tagfiles {
  int scanned_files;
  int scanned_dirs;
  int idx;
  int files_sorted;
  int unsorted_insert;
  int full_dir_inserted_idx;
  int cur_dir_files_count;
  int xmp_scanned_idx;
  Ecore_Thread *xmp_thread;
  Eina_Inarray *dirs_ls;
  Eina_Inarray *files_ls;
  Eina_Inarray *files;
  Eina_Hash *files_hash;
  void (*progress_cb)(Tagfiles *files, void *data);
  void (*done_cb)(Tagfiles *files, void *data);
  void (*known_tags_cb)(Tagfiles *files, void *data, const char *new_tag);
  Eina_Hash *known_tags;
  char *progress_wait_for_file;
  void *cb_data;
  Eina_List *group_changed_cb;
};

void filegroup_gettags(File_Group *group, Ecore_Thread *thread);
void save_sidecar(File_Group *group);
static void _xmp_scanner(void *data, Ecore_Thread *th);
static void _xmp_finish(void *data, Ecore_Thread *th);
static void _xmp_notify(void *data, Ecore_Thread *thread, void *msg_data);
static void _ls_done_cb(void *data, Eio_File *handler);

/* eina_inarray_pop was returning wrong elements - higher thatn actually inserted! */
void *eina_inarray_custom_pop(Eina_Inarray *ar)
{
  void *data;
  
  data = eina_inarray_nth(ar, eina_inarray_count(ar)-1);
  eina_inarray_pop(ar);
  
  return data;
}

void tagfiles_group_changed_cb_insert(Tagfiles *tagfiles, File_Group *group, void (*filegroup_changed_cb)(File_Group *group))
{
  Group_Changed_Cb_Data *data = malloc(sizeof(Group_Changed_Cb_Data));
  
  data->cb = filegroup_changed_cb;
  data->group = group;
  
  tagfiles->group_changed_cb = eina_list_append(tagfiles->group_changed_cb, data);
}

void tagfiles_group_changed_cb_delete(Tagfiles *tagfiles, File_Group *group)
{
  Group_Changed_Cb_Data *data;
  Eina_List *l;
  
  if (!tagfiles->group_changed_cb)
    return;
  
  EINA_LIST_FOREACH(tagfiles->group_changed_cb, l, data)
    if (data->group == group)
      break;
    
  if (!data || data->group != group)
    //not found
    return;
  
  tagfiles->group_changed_cb = eina_list_remove_list(tagfiles->group_changed_cb, l);
}

void tagfiles_group_changed_cb_flush(Tagfiles *files)
{
  //FIXME free data (use inarray?)
  files->group_changed_cb = NULL;
}

void tagfiles_group_changed_cb_del(Tagfiles *files, File_Group *group)
{
  Eina_List *l;
  Group_Changed_Cb_Data *data;
  
  EINA_LIST_FOREACH(files->group_changed_cb, l, data) {
    if (data->group == group) {
      files->group_changed_cb = eina_list_remove_list(files->group_changed_cb, l);
      free(data);
      return;
    }
  }
}

void call_group_changed_cb(Tagfiles *files, File_Group *group)
{
  Eina_List *l;
  Group_Changed_Cb_Data *data;
  
  EINA_LIST_FOREACH(files->group_changed_cb, l, data) {
    if (group == data->group)
      data->cb(group);
  }
}

int filegroup_cmp(File_Group **a, File_Group **b)
{
  return strcmp((*a)->basename, (*b)->basename);
}

Eina_Bool filegroup_tags_valid(File_Group *group)
{
  //FIXME load tags and GROUP_COMPLETE handling
  if (group->state != GROUP_LOADED) {
    //start scanning so info will be available soon
    run_scanner(group->tagfiles);
    
    return EINA_FALSE;
  }
  
  return EINA_TRUE;
}

Eina_Hash *filegroup_tags(File_Group *group)
{
  if (group->state != GROUP_LOADED)
    //"undefined"
    abort();
  
  return group->tags;
}

int filegroup_rating(File_Group *group)
{
  if (group->state != GROUP_LOADED)
    abort();
  
  return (int)group->tag_rating;
}

void filegroup_rating_set(File_Group *group, int rating)
{
  assert(group->state == GROUP_LOADED);
  
  if (rating != group->tag_rating) {
    group->tag_rating = rating;
    
    filegroup_save_sidecars(group);
  }
}

void tagged_file_sidecar_set(Tagged_File *f, const char *name)
{
  f->sidecar = name;
}

void run_scanner(Tagfiles *tagfiles)
{
  if (tagfiles->xmp_thread)
    return;
  
  if (tagfiles->xmp_scanned_idx < eina_inarray_count(tagfiles->files) && tagfiles->xmp_scanned_idx < tagfiles->full_dir_inserted_idx)
    tagfiles->xmp_thread = ecore_thread_feedback_run(_xmp_scanner, _xmp_notify, _xmp_finish, NULL, tagfiles, EINA_FALSE);
}

static void _xmp_finish(void *data, Ecore_Thread *th)
{
  Tagfiles *tagfiles = data;
  
  tagfiles->xmp_thread = NULL;
  
  //ls_done was called between _xmp_scanner exited and before finish was called
  //if we are over the current index and still scanning dirs stop scanning xmp files (do that later) 
  //FIXME multiple threads!
  /*if (!eina_inarray_count(tagfiles->dirs_ls))
    run_scanner(tagfiles);
  else if (tagfiles->xmp_scanned_idx < tagfiles_idx(tagfiles))
    run_scanner(tagfiles);*/
  //just run scanner slows down overall loading times but makes tags available earlier
  run_scanner(tagfiles);
}

static void _xmp_notify(void *data, Ecore_Thread *thread, void *msg_data)
{
  Tagfiles *tagfiles = data;
  Scanner_Feedback_Data *changed = msg_data;
  
  if (tagfiles->known_tags_cb && changed->tags) {
    while (eina_array_count(changed->tags))
      tagfiles->known_tags_cb(tagfiles, tagfiles->cb_data, eina_array_pop(changed->tags));
  }
  
  call_group_changed_cb(tagfiles, changed->group);
  
  if (changed->tags)
    eina_array_free(changed->tags);
  free(changed);
}

static void _xmp_scanner(void *data, Ecore_Thread *th)
{
  Tagfiles *tagfiles = data;
  
  while (tagfiles->xmp_scanned_idx < eina_inarray_count(tagfiles->files) && tagfiles->xmp_scanned_idx < tagfiles->full_dir_inserted_idx) {
    filegroup_gettags(*(File_Group**)eina_inarray_nth(tagfiles->files, tagfiles->xmp_scanned_idx),  th);
    tagfiles->xmp_scanned_idx++;
  }
}

int filegroup_cmp_neg(File_Group **a, File_Group **b)
{
  return -filegroup_cmp(a, b);
}

static void _files_check_sort(Tagfiles *files)
{
  if (files->files_sorted)
    return;
  
  eina_inarray_sort(files->files, (Eina_Compare_Cb)filegroup_cmp);
  files->files_sorted = EINA_TRUE;
}

int tagfiles_scanned_dirs(Tagfiles *tagfiles)
{
  return tagfiles->scanned_dirs;
}

Eina_Hash *tagfiles_known_tags(Tagfiles *tagfiles)
{  
  return tagfiles->known_tags;
}

void tagfiles_add_tag(Tagfiles *tagfiles, const char *tag)
{    
  eina_hash_add(tagfiles->known_tags, tag, tag);
  
  if (tagfiles->known_tags_cb)
      tagfiles->known_tags_cb(tagfiles, tagfiles->cb_data, tag);
  
}

int tagfiles_scanned_files(Tagfiles *tagfiles)
{
  return tagfiles->scanned_files;
}

void tagfiles_del(Tagfiles *files)
{
  printf("FIXME del tagfiles!\n");
}


//FIXME check scanning is finished?!
void tagfiles_del_curgroup(Tagfiles *tagfiles)
{
  assert(tagfiles->xmp_scanned_idx == eina_inarray_count(tagfiles->files));
  
  eina_inarray_remove_at(tagfiles->files, tagfiles->idx);
  
  tagfiles->xmp_scanned_idx = eina_inarray_count(tagfiles->files);
  if (tagfiles->idx >= eina_inarray_count(tagfiles->files))
    tagfiles->idx = eina_inarray_count(tagfiles->files)-1;
}

File_Group *tagfiles_nth(Tagfiles *tagfiles, int idx)
{
  File_Group *group;

  while (idx < 0)
    idx += tagfiles_count(tagfiles);
  
  idx = idx % tagfiles_count(tagfiles);
  
  if (idx < eina_inarray_count(tagfiles->files)) {
    _files_check_sort(tagfiles);
    assert(*(File_Group**)eina_inarray_nth(tagfiles->files, idx));
    return *(File_Group**)eina_inarray_nth(tagfiles->files, idx);
  }
  else {
    //we have to take files from files_ls
    
    //we have already inserte files from this dir: always need to sort after insert
    if (tagfiles->unsorted_insert) {
      while (eina_inarray_count(tagfiles->files_ls)) {
	group = *(File_Group**)eina_inarray_custom_pop(tagfiles->files_ls);
	assert(group);
	eina_inarray_push(tagfiles->files, &group);
      }
      tagfiles->files_sorted = EINA_FALSE;
      //eina_inarray_sort(tagfiles->files, filegroup_cmp);
    }
    //we have not yet inserted any files from the dir - can sort before!
    else {
      assert(tagfiles->files_sorted == EINA_TRUE);
      eina_inarray_sort(tagfiles->files_ls, (Eina_Compare_Cb)filegroup_cmp_neg);
      while (eina_inarray_count(tagfiles->files_ls)) {
	group = *(File_Group**)eina_inarray_custom_pop(tagfiles->files_ls);
	assert(group);
	eina_inarray_push(tagfiles->files, &group);
      }
    }
    tagfiles->unsorted_insert = EINA_TRUE;
    
    _files_check_sort(tagfiles);
    assert(idx < eina_inarray_count(tagfiles->files));
    assert(*(File_Group**)eina_inarray_nth(tagfiles->files, idx));

    return *(File_Group**)eina_inarray_nth(tagfiles->files, idx);
  }
}


File_Group *tagfiles_get(Tagfiles *tagfiles)
{
  return tagfiles_nth(tagfiles, tagfiles->idx);
}

int tagfiles_idx(Tagfiles *files)
{ 
  return files->idx;
}

int tagfiles_idx_set(Tagfiles *files, int idx)
{
  //FIXME check if we are still listing files?
  
  while (idx < 0)
    idx += tagfiles_count(files);
  
  files->idx = idx % tagfiles_count(files);
  
  return idx;
}

int tagfiles_step(Tagfiles *files, int step)
{
  tagfiles_idx_set(files, tagfiles_idx(files)+step);
  
  return tagfiles_idx(files);
}

int tagfiles_count(Tagfiles *files)
{
  return eina_inarray_count(files->files)+eina_inarray_count(files->files_ls);
}

Tagged_File *filegroup_nth(File_Group *g, int n)
{
  assert(n < eina_inarray_count(g->files));
  
  return ((Tagged_File*)eina_inarray_nth(g->files, n));
}

const char *tagged_file_name(Tagged_File *f)
{
  return f->filename;
}

const char *tagged_file_sidecar(Tagged_File *f)
{
  return f->sidecar;
}

void filegroup_data_attach(File_Group *g, int n, void *data)
{
  assert(n < eina_inarray_count(g->files));
  
  ((Tagged_File*)eina_inarray_nth(g->files, n))->data = data;
}

void *filegroup_data_get(File_Group *g, int n)
{
  assert(n < eina_inarray_count(g->files));
  
  return ((Tagged_File*)eina_inarray_nth(g->files, n))->data;
}

int filegroup_count(File_Group *g)
{
  return eina_inarray_count(g->files);
}

char *tagged_file_filterchain(Tagged_File *f)
{
  //FIXME check group state == GROUP_LOADED?
  
  return f->last_fc;
}

Ls_Info *ls_info_new(const char *path, Tagfiles *files)
{
  Ls_Info *i = calloc(sizeof(Ls_Info), 1);
  i->path = path;
  i->tagfiles = files;
  return i;
}

void ls_info_del(Ls_Info *i)
{
  free(i);
}

void filegroup_move_trash(File_Group *group)
{
  int i;
  Tagged_File *f;
  Efreet_Uri uri;
  uri.protocol = "file";
  uri.hostname = NULL;
  
  for(i=0;i<filegroup_count(group);i++) {
    f = filegroup_nth(group, i);
    if (!tagged_file_name(f))
      continue;
    uri.path = tagged_file_name(f);
    if (efreet_trash_delete_uri(&uri, 0) != 1)
      printf("delete failed for %s\n", tagged_file_name(f));
    if (tagged_file_sidecar(f)) {
      uri.path = tagged_file_sidecar(f);
      if (efreet_trash_delete_uri(&uri, 0) != 1)
        printf("delete failed for %s\n", tagged_file_sidecar(f));
    }

    /*if (group->sidecar) {
      uri.path = group->sidecar;
      if (efreet_trash_delete_uri(&uri, 0) != 1)
	printf("delete failed for %s\n", group->sidecar);
    }*/
  }
}

static Eina_Bool
_ls_filter_cb(void *data, Eio_File *handler, const Eina_File_Direct_Info *info)
{  
  if (info->type == EINA_FILE_REG || info->type == EINA_FILE_LNK || info->type == EINA_FILE_UNKNOWN || info->type == EINA_FILE_DIR)
    return EINA_TRUE;
    
  return EINA_FALSE;
}

//TODO don't create empty sidecar files?
Tagged_File tag_file_new(Tagfiles *tagfiles, File_Group *group, const char *name)
{
  int i;
  Tagged_File *f;
  Tagged_File file = {NULL, NULL, NULL};
  
  if (eina_str_has_extension(name, ".xmp")) {
    for(i=0;i<filegroup_count(group);i++) {
      f = filegroup_nth(group, i);
      if (tagged_file_name(f) && !strncmp(tagged_file_name(f),name,strlen(tagged_file_name(f))))
        tagged_file_sidecar_set(f, name);
    }
    tagged_file_sidecar_set(&file, name);
  }
  else {
    for(i=0;i<filegroup_count(group);i++) {
      f = filegroup_nth(group, i);
      if (tagged_file_sidecar(f) && !strncmp(tagged_file_sidecar(f),name,strlen(name))) {
        if (tagged_file_sidecar(&file))
          printf("FIXME: multiple sidecars per file %s vs %s\n", tagged_file_sidecar(&file), tagged_file_sidecar(f));
        tagged_file_sidecar_set(&file, tagged_file_sidecar(f));
      }
    }
    file.filename = name;
  }

  return file;
}

File_Group *file_group_new(Tagfiles *tagfiles, const char *name, const char *basename)
{
  Tagged_File file_new;
  File_Group *group = calloc(sizeof(File_Group), 1);
  
  group->files = eina_inarray_new(sizeof(Tagged_File), 2);
  group->tags = eina_hash_string_superfast_new(NULL);
  group->tagfiles = tagfiles;
  group->basename = basename;
  
  eina_hash_add(tagfiles->files_hash, basename, group);
  
  file_new = tag_file_new(tagfiles, group, name);

  eina_inarray_push(group->files, &file_new);
  return group;
}

void file_group_add(Tagfiles *tagfiles, File_Group *group, const char *name)
{
  Tagged_File file_new;
  
  file_new = tag_file_new(tagfiles, group, name);
  eina_inarray_push(group->files, &file_new);
}

void insert_file(Tagfiles *tagfiles, const char *file)
{
  const char *basename;
  File_Group *group;
  
  basename = strrchr(file, '/');
  if (!basename)
    basename = file;
  
  basename = strchr(basename, '.');
  
  if (basename)
    basename = eina_stringshare_add_length(file, basename-file);
  else
    basename = eina_stringshare_add(file);
  
  group = eina_hash_find(tagfiles->files_hash, basename);
  
  if (!group) {
    group = file_group_new(tagfiles, file, basename);
    assert(group);
    eina_inarray_push(tagfiles->files_ls, &group);
  }
  else
    file_group_add(tagfiles, group, file);
  
  call_group_changed_cb(tagfiles, group);
  
  return;
}

static void _ls_main_cb(void *data, Eio_File *handler, const Eina_File_Direct_Info *info)
{
  Ls_Info *finfo = data;
  Tagfiles *tagfiles = finfo->tagfiles;
  const char *file;
  
  file = eina_stringshare_add(info->path);
  if (!file)
    return;

  if (info->type != EINA_FILE_DIR && info->type != EINA_FILE_LNK) {
    tagfiles->cur_dir_files_count++;
    tagfiles->scanned_files++;
    insert_file(tagfiles, file);
    
    if (tagfiles->progress_wait_for_file) {
      if (!strcmp(tagfiles->progress_wait_for_file, file)) {
	printf("found file ad idx %d\n", tagfiles_count(tagfiles)-1);
	tagfiles->progress_wait_for_file = NULL;
	tagfiles->idx = tagfiles_count(tagfiles)-1;
      }
    }
    else
      tagfiles->progress_cb(tagfiles, tagfiles->cb_data);
  }
  else {
    tagfiles->scanned_dirs++;
    //if (info->type == EINA_FILE_LNK)
    eina_inarray_push(tagfiles->dirs_ls, &file);
  }
}

int tagfiles_init(void)
{
  return xmp_init();
}

void tagfiles_shutdown(void)
{
  xmp_terminate();
}

void tagfile_xmp_gettags(File_Group *group, Tagged_File *file, Ecore_Thread *thread)
{
  int len;
  FILE *f;
  XmpPtr xmp;
  char *buf;
  const char *tag;
  XmpIteratorPtr iter;
  XmpStringPtr propValue;
  Scanner_Feedback_Data *feedback_data = calloc(sizeof(Scanner_Feedback_Data), 1);
  
  buf = malloc(MAX_XMP_FILE);
  
  f = fopen(file->sidecar, "r");
  
  if (!f) {
    //FIXME check in thread if we have to do any call!
    //FIXME feedback for file only?
    feedback_data->group = group;
    ecore_thread_feedback(thread, feedback_data);
    return;
  }
  
  len = fread(buf, 1, MAX_XMP_FILE, f);
    
  fclose(f);
  
  xmp = xmp_new(buf, len);
  
  if (!xmp) {
    printf("parse failed\n");
    xmp_free(xmp);
    free(buf);
    //FIXME check in thread if we have to do any call!
    feedback_data->group = group;
    ecore_thread_feedback(thread, feedback_data);
    return;
  }
  
  if (xmp_prefix_namespace_uri("lr", NULL))
  {
    propValue = xmp_string_new();
    iter = xmp_iterator_new(xmp, "http://ns.adobe.com/lightroom/1.0/", "lr:hierarchicalSubject", XMP_ITER_JUSTLEAFNODES);
  
    while (xmp_iterator_next(iter, NULL, NULL, propValue, NULL)) {
      tag = strdup(xmp_string_cstr(propValue));
      if (!eina_hash_find(group->tags, tag))
			eina_hash_add(group->tags, tag, tag);
      if (!eina_hash_find(group->tagfiles->known_tags, tag)) {
			eina_hash_add(group->tagfiles->known_tags, tag, tag);
	if (!feedback_data->tags) feedback_data->tags = eina_array_new(8);
	eina_array_push(feedback_data->tags, tag);
      }
    }

    xmp_iterator_free(iter); 
    xmp_string_free(propValue);
  }
  
  if (xmp_prefix_namespace_uri("xmp", NULL))
  {
    if (!xmp_get_property_int32(xmp, "http://ns.adobe.com/xap/1.0/", "xmp:Rating", &group->tag_rating, NULL))
      group->tag_rating = 0;
  }
  
  if (xmp_prefix_namespace_uri("lime", NULL))
  {
    propValue = xmp_string_new();
    if (xmp_get_property(xmp, "http://technik-stinkt.de/lime/0.1/", "lime:lastFilterChain", propValue, NULL))
      file->last_fc = strdup(xmp_string_cstr(propValue));
    else
      file->last_fc = NULL;
    xmp_string_free(propValue);
  }
  
  
  xmp_free(xmp);
  free(buf);

  feedback_data->group = group;
  ecore_thread_feedback(thread, feedback_data);  
}

void filegroup_gettags(File_Group *group, Ecore_Thread *thread)
{
  int i;
  group->state = GROUP_LOADED;
  
  //FIXME why was this here?
  /*if (!group->sidecar) {
    //FIXME check in thread if we have to do any call!
    feedback_data->group = group;
    ecore_thread_feedback(thread, feedback_data);
    return;
  }*/
  
  for(i=0;i<filegroup_count(group);i++)
    if (tagged_file_sidecar(filegroup_nth(group, i)))
      tagfile_xmp_gettags(group, filegroup_nth(group, i), thread);
  
  return;
}

static void _ls_error_cb(void *data, Eio_File *handler, int error)
{
  Ls_Info *info = data;
  fprintf(stderr, "error: [%s] - could not ls %s\n", strerror(error),info->path);
  _ls_done_cb(data, handler);
}

int dir_strcmp_neg(const char **a, const char **b)
{
  return -strcmp(*a, *b);
}

typedef struct {
  const char *filename;
  int size;
} Preload_Data;

static void _fadvice_file(void *data, Ecore_Thread *th)
{
  int fd;
  Preload_Data *preload = data;
  
  eina_sched_prio_drop();
  
  fd = open(preload->filename, O_RDONLY);
  posix_fadvise(fd, 0,preload->size, POSIX_FADV_WILLNEED);
  close(fd);
  
  return;
}

//FIXME check tag filter?
void tagfiles_preload_headers(Tagfiles *tagfiles, int direction, int range, int size)
{
  int i;
  File_Group *group;
  const char *filename;
  Preload_Data *preload;
  
  //for(j=tagfiles_idx(tagfiles);j<tagfiles_idx(tagfiles)+range*direction;j+=direction) {
    group = tagfiles_nth(tagfiles, tagfiles_idx(tagfiles)+range*direction);
    for(i=0;i<filegroup_count(group);i++) {
      filename = tagged_file_name(filegroup_nth(group, i));
      if (filename) {
	preload = malloc(sizeof(Preload_Data));
	preload->size = size;
	preload->filename = filename;
	ecore_thread_run(_fadvice_file, NULL, NULL, preload);
      }
    }
  //}
}

Eina_Bool _idle_ls_continue(void *data) 
{
  Tagfiles *tagfiles = data;
  char *dir;
  
  //FIXME do sort in extra thread instead of when idle?
  eina_inarray_sort(tagfiles->dirs_ls, (Eina_Compare_Cb)dir_strcmp_neg);
  dir = *(char**)eina_inarray_custom_pop(tagfiles->dirs_ls);
  eio_file_direct_ls(dir, &_ls_filter_cb, &_ls_main_cb,&_ls_done_cb, &_ls_error_cb, ls_info_new(dir, tagfiles));
    
  return ECORE_CALLBACK_CANCEL;
}

static void _ls_done_cb(void *data, Eio_File *handler)
{
  Ls_Info *info = data;
  Tagfiles *tagfiles = info->tagfiles;
  File_Group *group;
  
  free(info);

  //we have already inserted files from this dir: always need to sort after insert
  if (tagfiles->unsorted_insert) {
    while (eina_inarray_count(tagfiles->files_ls)) {
      group = *(File_Group**)eina_inarray_custom_pop(tagfiles->files_ls);
      assert(group);
      eina_inarray_push(tagfiles->files, &group);
    }
    tagfiles->files_sorted = EINA_FALSE;
  }
  //we have not yet inserted any files from the dir - can sort before!
  else {
    if (tagfiles->files_sorted)
      eina_inarray_sort(tagfiles->files_ls, (Eina_Compare_Cb)filegroup_cmp_neg);
    while (eina_inarray_count(tagfiles->files_ls)) {
      group = *(File_Group**)eina_inarray_custom_pop(tagfiles->files_ls);
      //assert(group);
      //FIXME
      if (group)
	eina_inarray_push(tagfiles->files, &group);
    }
  }
  _files_check_sort(tagfiles);
  tagfiles->full_dir_inserted_idx += tagfiles->cur_dir_files_count;
  tagfiles->cur_dir_files_count = 0;
    
  //have finished dir - next dir is guaranteed to come after all files already seen
  tagfiles->unsorted_insert = EINA_FALSE;
  
  run_scanner(tagfiles);
  
  if (eina_inarray_count(tagfiles->dirs_ls)) {
    //dirs are sorted before scanning so we do not need to sort all files at once!
    ecore_idler_add(_idle_ls_continue, tagfiles);
  }
  else
    tagfiles->done_cb(tagfiles, tagfiles->cb_data);
}


Tagfiles *tagfiles_new_from_dir(const char *path, void (*progress_cb)(Tagfiles *files, void *data), void (*done_cb)(Tagfiles *files, void *data), void (*known_tags_cb)(Tagfiles *files, void *data, const char *new_tag))
{
  char *dir;
  struct stat path_stat;
  Tagfiles *files;
  Ls_Info *info;
  
  if (stat(path, &path_stat) < 0)
    return NULL;
  
  printf("start tagfiles with %s\n");
  
  files = calloc(sizeof(Tagfiles), 1);
  
  if (S_ISDIR(path_stat.st_mode)) {
    printf("%s is dir! %d %d\n", path, ecore_file_is_dir(path));
    dir = path;
  } 
  else {
    dir = ecore_file_dir_get(path);
    files->progress_wait_for_file = path;
    printf("dir is %s\n", dir);
  }
  
  files->progress_cb = progress_cb;
  files->done_cb = done_cb;
  files->known_tags_cb = known_tags_cb;
  files->known_tags = eina_hash_string_superfast_new(NULL);
  files->files = eina_inarray_new(sizeof(File_Group*), 128);
  files->files_hash = eina_hash_string_superfast_new(NULL);
  files->files_ls = eina_inarray_new(sizeof(File_Group*), 128);
  files->dirs_ls = eina_inarray_new(sizeof(char *), 32);
  files->files_sorted = EINA_TRUE;
  
  eio_file_direct_ls(dir, &_ls_filter_cb, &_ls_main_cb,&_ls_done_cb, &_ls_error_cb, ls_info_new(dir,files));
  
  return files;
}

const char *filegroup_basename(File_Group *group)
{
  return group->basename;
}

Tagged_File *tagged_file_new_from_path(const char *path)
{
  Tagged_File *file = calloc(sizeof(Tagged_File), 1);
  char *filename;
  
  filename = strrchr(path, '/');
  
  if (filename) {
    file->dirname = eina_stringshare_add_length(path, filename-path);
    file->filename = eina_stringshare_add(filename+1);
  }
  else
    file->filename = eina_stringshare_add(path);

  return file;
}

int cmp_img_file(char *a, char *b)
{
	if (strlen(a) == 12 && strlen(b) == 12 && a[0] == 'P' && b[0] == 'P' && a[8] == '.' && b[8] == '.')
		//cmp just file number not date
		return strcmp(a+4, b+4);
	
	return strcmp(a, b);
	
}

int cmp_tagged_files(Tagged_File *a, Tagged_File *b)
{
  int cmp;
   
  cmp = strcmp(a->dirname, b->dirname);
  
  if (!cmp)
    cmp = strcmp(a->filename, b->filename);
  
  return cmp;
}

Eina_Bool xmp_add_tags_lr_func(const Eina_Hash *hash, const void *key, void *data, void *fdata)
{
  const char *tag = data;
  XmpPtr xmp = fdata;
  
  xmp_append_array_item(xmp, "http://ns.adobe.com/lightroom/1.0/", "lr:hierarchicalSubject", XMP_PROP_ARRAY_IS_UNORDERED, tag, 0);
  
  return 1;
}

Eina_Bool xmp_add_tags_dc_func(const Eina_Hash *hash, const void *key, void *data, void *fdata)
{
  const char *tag = data;
  XmpPtr xmp = fdata;
  
  xmp_append_array_item(xmp, "http://purl.org/dc/elements/1.1/", "dc:subject", XMP_PROP_ARRAY_IS_UNORDERED, tag, 0);
  
  return 1;
}

Eina_Bool xmp_add_tags_dk_func(const Eina_Hash *hash, const void *key, void *data, void *fdata)
{
  char *tag = strdup(data);
  char *replace;
  XmpPtr xmp = fdata;
  
  while ((replace = strchr(tag, '|')))
    *replace = '/';
  
  xmp_append_array_item(xmp, "http://www.digikam.org/ns/1.0/", "digiKam:TagsList", XMP_PROP_ARRAY_IS_UNORDERED, tag, 0);
  
  //FIXME
  free(tag);
  
  return 1;
}

static void new_xmp_file(Tagged_File *file, File_Group *group)
{
  FILE *f;
  const char *buf;
  XmpPtr xmp;
  XmpStringPtr xmp_buf = xmp_string_new();
  
  xmp = xmp_new_empty();
  xmp_register_namespace("http://ns.adobe.com/lightroom/1.0/", "lr", NULL);
  xmp_register_namespace("http://purl.org/dc/elements/1.1/", "dc", NULL);
  xmp_register_namespace("http://www.digikam.org/ns/1.0/", "digiKam", NULL);
  xmp_register_namespace("http://technik-stinkt.de/lime/0.1/", "lime", NULL);
  
  eina_hash_foreach(group->tags, xmp_add_tags_lr_func, xmp);
  eina_hash_foreach(group->tags, xmp_add_tags_dc_func, xmp);
  eina_hash_foreach(group->tags, xmp_add_tags_dk_func, xmp);
  
  if (group->tag_rating) {
    if (!xmp_prefix_namespace_uri("xmp", NULL))
      xmp_register_namespace("http://ns.adobe.com/xap/1.0/", "xmp", NULL);
    xmp_set_property_int32(xmp, "http://ns.adobe.com/xap/1.0/", "xmp:Rating", group->tag_rating, 0);
  }
  
  if (file->last_fc)
    xmp_set_property(xmp, "http://technik-stinkt.de/lime/0.1/", "lime:lastFilterChain", file->last_fc, 0);
  
  xmp_serialize(xmp, xmp_buf, XMP_SERIAL_OMITPACKETWRAPPER | XMP_SERIAL_USECOMPACTFORMAT, 0);
  
  assert(file->sidecar);
  
  f = fopen(file->sidecar, "w");
  assert(f);
  buf = xmp_string_cstr(xmp_buf);
  fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n%s\n", buf);
  fclose(f);
  
  xmp_string_free(xmp_buf);
  xmp_free(xmp);
  
  printf("wrote new tag file!\n");
}


void tagged_file_new_sidecar(Tagged_File *file)
{
  char *buf;
  
  if (!tagged_file_name(file) || tagged_file_sidecar(file))
    return;
  buf = malloc(strlen(file->filename)+5);
  sprintf(buf, "%s.xmp", file->filename);
  file->sidecar = buf;
}

//FIXME merge new tag file with tag save/new/read (xmp init/read)
void tagged_file_sidecar_save(Tagged_File *file, File_Group *group)
{
  FILE *f;
  int len;
  XmpPtr xmp = NULL;
  XmpStringPtr xmp_buf = xmp_string_new();
  char *buf = malloc(MAX_XMP_FILE);
  const char *tag;
  XmpIteratorPtr iter;
  XmpStringPtr propValue;
  
  if (!file->sidecar)
    tagged_file_new_sidecar(file);
  
  f = fopen(file->sidecar, "r");

  if (f) {
    len = fread(buf, 1, MAX_XMP_FILE, f);
    fclose(f);
    
    xmp = xmp_new(buf, len);
    free(buf);
  }
  
  if (!xmp) {
    printf("xmp parse failed, overwriting with new!\n");
    xmp_free(xmp);
    new_xmp_file(file, group);
    return;
  }
  
  if (!xmp_prefix_namespace_uri("lr", NULL))
      xmp_register_namespace("http://ns.adobe.com/lightroom/1.0/", "lr", NULL);
  
  if (!xmp_prefix_namespace_uri("dc", NULL))
    xmp_register_namespace("http://purl.org/dc/elements/1.1/", "dc", NULL);
  
  if (!xmp_prefix_namespace_uri("digiKam", NULL))
    xmp_register_namespace("http://www.digikam.org/ns/1.0/", "digiKam", NULL);
  
  if (!xmp_prefix_namespace_uri("lime", NULL))
    xmp_register_namespace("http://technik-stinkt.de/lime/0.1/", "lime", NULL);
  
  //delete all keyword tags 
  xmp_delete_property(xmp, "http://ns.adobe.com/lightroom/1.0/", "lr:hierarchicalSubject");
  xmp_delete_property(xmp, "http://www.digikam.org/ns/1.0/", "digiKam:TagsList");
  xmp_delete_property(xmp, "http://ns.microsoft.com/photo/1.0/", "MicrosoftPhoto:LastKeywordXMP");
  xmp_delete_property(xmp, "http://purl.org/dc/elements/1.1/", "dc:subject");
  xmp_delete_property(xmp, "http://ns.adobe.com/xap/1.0/", "xmp:Rating");
  xmp_delete_property(xmp, "http://technik-stinkt.de/lime/0.1/", "lime:lastFilterChain");

  eina_hash_foreach(group->tags, xmp_add_tags_lr_func, xmp);
  eina_hash_foreach(group->tags, xmp_add_tags_dc_func, xmp);
  eina_hash_foreach(group->tags, xmp_add_tags_dk_func, xmp);

  if (group->tag_rating) {
    if (!xmp_prefix_namespace_uri("xmp", NULL))
      xmp_register_namespace("http://ns.adobe.com/xap/1.0/", "xmp", NULL);
    xmp_set_property_int32(xmp, "http://ns.adobe.com/xap/1.0/", "xmp:Rating", group->tag_rating, 0);
  }

  if (file->last_fc)
    xmp_set_property(xmp, "http://technik-stinkt.de/lime/0.1/", "lime:lastFilterChain", file->last_fc, 0);
      
  xmp_serialize(xmp, xmp_buf, XMP_SERIAL_OMITPACKETWRAPPER | XMP_SERIAL_USECOMPACTFORMAT, 0);
  
  buf = (char*)xmp_string_cstr(xmp_buf);
  
  f = fopen(file->sidecar, "w");
  assert(f);
  buf = (char*)xmp_string_cstr(xmp_buf);
  fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n%s\n", buf);
  fclose(f);
  
  xmp_string_free(xmp_buf);
  xmp_free(xmp);
}

void filegroup_save_sidecars(File_Group *group)
{
  int i;
  Tagged_File *file;
  
  EINA_INARRAY_FOREACH(group->files, file)
    tagged_file_sidecar_save(file, group);
}

//

void tagged_file_filterchain_set(Tagged_File *file, File_Group *group, const char *fc)
{    
  if (fc) {
    file->last_fc = strdup(fc);
    if (strstr(file->last_fc, ",memsink"))
      *strstr(file->last_fc, ",memsink") = '\0';
    else if (strstr(file->last_fc, "memsink") == file->last_fc) {
      free(file->last_fc);
      file->last_fc = NULL;
    }
  }
  else
    file->last_fc = NULL;
  
  tagged_file_sidecar_save(file, group);
}