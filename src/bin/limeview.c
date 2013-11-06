#include <Elementary.h>
#include <elm_win_legacy.h>
#include <Ecore.h>
#include <Eio.h>
#include <fcntl.h>

#include <exempi/xmp.h>
#include <exempi/xmpconsts.h>

#include "Lime.h"
#include "cli.h"

#define MAX_FAST_SCALEDOWN 5
#define TILE_SIZE DEFAULT_TILE_SIZE
#define MAX_XMP_FILE 1024*1024

#define RSYNC_INSTANCE_COUNT 1

int high_quality_delay =  150;
int max_reaction_delay =  1000;
int fullscreen = 0;

Eina_Hash *known_tags;
Elm_Genlist_Item_Class *tags_list_itc;
Elm_Genlist_Item_Class *tags_filter_itc;

typedef struct {
  const char *extensions;
  char *path;
  const char *server;
  const char *dir;
  Eina_Array *list;
  Evas_Object *eo_progress;
  int count;
} Export_Data;

typedef struct {
  const char *filename;
  const char *sidecar;
  Eina_Array *tags;
} Tagged_File;

typedef struct {
  Eina_Inarray *files; //tagged files
  const char *sidecar; //sidecar file used for the image group
  Eina_Hash *tags; //all tags of the group or a file in the group
  int32_t tag_rating;
  //Eina_Array *group_tags; //tags that are assigned specifically to the group
} File_Group;

Eina_Inarray *files;

char image_path[EINA_PATH_MAX];
char *image_file;
Eina_Hash *tags_filter;
int tags_filter_rating = 0;
char *dir;

typedef struct {
  Filter *f;
  Evas_Object *frame;
  Elm_Object_Item *item;
} Filter_Chain;

typedef struct {
  Eina_Matrixsparse **mats;
  Evas_Object **high_of_layer;
  Evas_Object **low_of_layer;
  int scale_max;
  Eina_Array *images;
} Mat_Cache;

int max_workers;
Filter *sink, *contr, *blur, *load;
Evas_Object *clipper, *win, *scroller, *file_slider, *filter_list, *select_filter, *pos_label;
Evas_Object *tab_group, *tab_filter, *tab_settings, *tab_tags, *tab_current, *tab_box, *tab_export, *tab_tags, *tags_list, *tags_filter_list, *seg_rating;
Evas_Object *group_list, *export_box, *export_extensions, *export_path, *main_vbox;
Evas_Object *grid = NULL, *vbox_bottom, *bg;
char *labelbuf;
char *pos_lbl_buf;
int posx, posy;
Evas_Object *slider_blur, *slider_contr, *gridbox;
int cache_size;
Dim size;
Mat_Cache *mat_cache;
Mat_Cache *mat_cache_old;
int forbid_fill = 1;
Eina_List *filter_last_selected = NULL;
Eina_List *filter_chain = NULL;
Filter *(*select_filter_func)(void);
Eina_List *files_unsorted = NULL;
int verbose;
Eina_Array *finished_threads = NULL;
Ecore_Timer *preview_timer = NULL;

int export_count;
int file_step = 1;
int file_idx = 0;
int group_idx;
int bench_idx = 0;
int preview_tiles = 0;

int *thread_ids;

float scale_goal = 1.0;
Bench_Step *bench;
int fit;

int worker;
void (*pending_action)(void *data, Evas_Object *obj);
void *pending_data;
Evas_Object *pending_obj;

static void on_scroller_move(void *data, Evas_Object *obj, void *event_info);
static void fill_scroller(void);
void workerfinish_schedule(void (*func)(void *data, Evas_Object *obj), void *data, Evas_Object *obj);
void filter_settings_create_gui(Eina_List *chain_node, Evas_Object *box);
void fill_scroller_preview(void);

Filter_Chain *fc_new(Filter *f)
{
  Filter_Chain *fc = calloc(sizeof(Filter_Chain), 1);
  fc->f = f;
  
  return fc;
}

void delgrid(void);

typedef struct {
  Evas_Object *img;
  int scale;
  uint8_t *buf;
  Rect area;
  int t_id;
  int scaled_preview;
  int packx, packy, packw, packh;
  int show_direct;
} _Img_Thread_Data;

void size_recalc(void)
{  
  Dim *size_ptr;
  forbid_fill++;
  size_ptr = (Dim*)filter_core_by_type(sink, MT_IMGSIZE);
  if (size_ptr) {
    size = *size_ptr;
  }
  
  forbid_fill--;
}

void grid_setsize(void)
{
  int x,y,w,h;
  
  size_recalc();
  
  if (size.width && size.height) {
    elm_grid_size_set(grid, size.width, size.height);
    elm_grid_pack_set(clipper, 0, 0, size.width, size.height);
    elm_box_recalculate(gridbox);
  }
  else {
    elm_grid_size_set(grid, 200, 200);
    elm_grid_pack_set(clipper, 0, 0, 200, 200);
    elm_box_recalculate(gridbox);
    return;
  }
  
  //FIXME useful?
  //if (forbid_fill)
    //return;
  
  
  if (fit) {

    elm_scroller_region_get(scroller,&x,&y,&w,&h);

    //FIXME!!!
    if (!w || !h) {
      printf("scroller has no area!\n");
      return;
    }
    else {
      scale_goal = (float)size.width / w;	
      if ((float)size.height / h > scale_goal)
	scale_goal = (float)size.height / h;
    }
  }
    
  evas_object_size_hint_min_set(grid,  size.width/scale_goal, size.height/scale_goal);
  elm_box_recalculate(gridbox);
}

void xmp_gettags(const char *file, File_Group *group)
{
  int len;
  FILE *f;
  XmpPtr xmp;
  char *buf;
  const char *tag;
  XmpIteratorPtr iter;
  XmpStringPtr propValue;
  
  buf = malloc(MAX_XMP_FILE);
  
  f = fopen(file, "r");
  
  if (!f)
    return;
  
  len = fread(buf, 1, MAX_XMP_FILE, f);
    
  fclose(f);
  
  xmp = xmp_new(buf, len);
  
  if (!xmp) {
    printf("parse failed\n");
    xmp_free(xmp);
    free(buf);
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
      if (!eina_hash_find(known_tags, tag))
			eina_hash_add(known_tags, tag, tag);
    }

    xmp_iterator_free(iter); 
    xmp_string_free(propValue);
  }
  
  if (xmp_prefix_namespace_uri("xmp", NULL))
  {
    if (!xmp_get_property_int32(xmp, "http://ns.adobe.com/xap/1.0/", "xmp:Rating", &group->tag_rating, NULL))
      group->tag_rating = 0;
  }
  
  
  xmp_free(xmp);
  free(buf);
  
  return;
}

void int_changed_do(void *data, Evas_Object *obj)
{
  Meta *m = data;
  
  assert(!worker);

  forbid_fill++;
  
  bench_delay_start();
  
  lime_setting_int_set(m->filter, m->name, (int)elm_slider_value_get(obj));
  
  lime_config_test(sink);
  
  delgrid();
  
  size_recalc();
  
  forbid_fill--;
  
  fill_scroller_preview();
  fill_scroller();
}

void float_changed_do(void *data, Evas_Object *obj)
{
  Meta *m = data;
  
  assert(!worker);
  
  forbid_fill++;
  
  bench_delay_start();
  
  lime_setting_float_set(m->filter, m->name, (float)elm_slider_value_get(obj));
  
  lime_config_test(sink);
  
  delgrid();
  
  size_recalc();
  
  forbid_fill--;
  
  fill_scroller_preview();
  fill_scroller();
}

void del_filter_settings(void);

void remove_filter_do(void *data, Evas_Object *obj)
{
  Eina_List *chain_node = data;
  Filter_Chain *fc = eina_list_data_get(chain_node);
  Filter_Chain *prev, *next;
  
  assert(!worker);

  forbid_fill++;
  
  //we can not delete the first (input) or last (output) filter in the chain
  assert(eina_list_prev(chain_node));
  assert(eina_list_next(chain_node));
  
  prev = eina_list_data_get(eina_list_prev(chain_node));
  next = eina_list_data_get(eina_list_next(chain_node));
  
  del_filter_settings(); 
  filter_chain = eina_list_remove_list(filter_chain, chain_node);
  
  elm_object_item_del(fc->item);
  filter_connect(prev->f, 0, next->f, 0);
  
  delgrid();
  
  //test_filter_config(load);
  
  size_recalc();
  
  forbid_fill--;
  
  fill_scroller_preview();
  fill_scroller();
}

void _on_filter_select(void *data, Evas_Object *obj, void *event_info)
{ 
  del_filter_settings();
  
  filter_settings_create_gui(data, tab_filter);
  filter_last_selected = data;
}

void insert_before_do(void *data, Evas_Object *obj)
{
  Filter_Chain *fc_source, *fc_sink, *fc;
  Eina_List *source_l, *sink_l;
  Filter *f;

  forbid_fill++;
  
  if (!filter_last_selected) {
    sink_l = eina_list_last(filter_chain);
    source_l = eina_list_prev(sink_l);
  }
  else {
    sink_l = filter_last_selected;
    source_l = eina_list_prev(sink_l);
  }
    
  fc_sink = eina_list_data_get(sink_l);
  fc_source = eina_list_data_get(source_l);
  f = select_filter_func();
  fc = fc_new(f);
  
  filter_chain = eina_list_append_relative_list(filter_chain, fc, source_l);
  
  if (filter_last_selected)
    fc->item = elm_list_item_insert_after(filter_list, fc_sink->item, f->fc->name, NULL, NULL, &_on_filter_select, eina_list_prev(sink_l));
  else
    fc->item = elm_list_item_prepend(filter_list, f->fc->name, NULL, NULL, &_on_filter_select, eina_list_next(source_l));

  elm_list_item_selected_set(fc->item, EINA_TRUE);
  elm_list_go(filter_list);
  
  filter_connect(fc_source->f, 0, f, 0);
  filter_connect(f, 0, fc_sink->f, 0);

  delgrid();
  
  lime_config_test(sink);
  
  size_recalc();
  
  forbid_fill--;
  
  fill_scroller_preview();
  fill_scroller();
}

void insert_rotation_do(void *data, Evas_Object *obj)
{
  int rotation = (intptr_t)data;
  Filter_Chain *fc_source, *fc_sink, *fc;
  Eina_List *source_l, *sink_l;
  Filter *f;

  forbid_fill++;
  
  sink_l = eina_list_last(filter_chain);
  source_l = eina_list_prev(sink_l);
    
  fc_sink = eina_list_data_get(sink_l);
  fc_source = eina_list_data_get(source_l);
  f = filter_core_simplerotate.filter_new_f();
  fc = fc_new(f);
  
  lime_setting_int_set(f, "rotation", rotation); 
  
  filter_chain = eina_list_append_relative_list(filter_chain, fc, source_l);
  
  if (filter_last_selected)
    fc->item = elm_list_item_insert_after(filter_list, fc_sink->item, f->fc->name, NULL, NULL, &_on_filter_select, eina_list_prev(sink_l));
  else
    fc->item = elm_list_item_prepend(filter_list, f->fc->name, NULL, NULL, &_on_filter_select, eina_list_next(source_l));

  elm_list_item_selected_set(fc->item, EINA_TRUE);
  elm_list_go(filter_list);
  
  filter_connect(fc_source->f, 0, f, 0);
  filter_connect(f, 0, fc_sink->f, 0);

  delgrid();
  
  lime_config_test(sink);
  
  size_recalc();
  
  forbid_fill--;
  
  fill_scroller_preview();
  fill_scroller();
}

void insert_after_do(void *data, Evas_Object *obj)
{
  Filter_Chain *fc_source, *fc_sink, *fc;
  Eina_List *source_l, *sink_l;
  Filter *f;
  
  if (!select_filter_func) return;
  
  forbid_fill++;
  
  if (!filter_last_selected) {
    source_l = filter_chain;
    sink_l = eina_list_next(source_l);
  }
  else {
    source_l = filter_last_selected;
    sink_l = eina_list_next(source_l);
  }
    
  fc_sink = eina_list_data_get(sink_l);
  fc_source = eina_list_data_get(source_l);
  f = select_filter_func();
  fc = fc_new(f);
  
  filter_chain = eina_list_append_relative_list(filter_chain, fc, source_l);
  
  if (filter_last_selected)
    fc->item = elm_list_item_insert_before(filter_list, fc_source->item, f->fc->name, NULL, NULL, &_on_filter_select, eina_list_next(source_l));
  else
    fc->item = elm_list_item_append(filter_list, strdup(f->fc->name), NULL, NULL, &_on_filter_select, eina_list_next(source_l));

  elm_list_item_selected_set(fc->item, EINA_TRUE);
  elm_list_go(filter_list);
  
  filter_connect(fc_source->f, 0, f, 0);
  filter_connect(f, 0, fc_sink->f, 0);
  
  lime_config_test(sink);
  
  delgrid();
  //grid_setsize();
  
  forbid_fill--;
  
  fill_scroller_preview();
  fill_scroller();
}

static void
on_int_changed(void *data, Evas_Object *obj, void *event_info)
{
  workerfinish_schedule(&int_changed_do, data, obj);
  
}

static void on_select_filter_select(void *data, Evas_Object *obj, void *event_info)
{
  Filter_Core *fc = data;
  
  elm_object_text_set(select_filter, fc->shortname);
  select_filter_func = fc->filter_new_f;
}


static void
on_remove_filter(void *data, Evas_Object *obj, void *event_info)
{
  workerfinish_schedule(&remove_filter_do, data, obj);
}

static void
on_float_changed(void *data, Evas_Object *obj, void *event_info)
{ 
  workerfinish_schedule(&float_changed_do, data, obj);
}

void slider_insert(Evas_Object *vbox, Meta *setting)
{
  int i;
  Meta *sub;
  Evas_Object *slider;
  int imin = 0, imax = 100;
  float fmin = 0.0, fmax = 100.0;
  
  slider = elm_slider_add(vbox);

  elm_slider_unit_format_set(slider, "%.1f");
  evas_object_size_hint_weight_set(slider, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(slider, EVAS_HINT_FILL, 0);
  if (setting->type == MT_INT) {
    evas_object_smart_callback_add(slider, "delay,changed", on_int_changed, setting);
    for(i=0;i<ma_count(setting->childs);i++) {
      sub = ma_data(setting->childs, i);
      assert(sub->type == setting->type);
      if (!strcmp("PARENT_SETTING_MIN", sub->name))
	imin = *(int*)sub->data;
      else if (!strcmp("PARENT_SETTING_MAX", sub->name))
	imax = *(int*)sub->data;
      
    }
    elm_slider_min_max_set(slider, (float)imin, (float)imax);
    elm_slider_value_set(slider, (float)*(int*)setting->data);
  }
  else if (setting->type == MT_FLOAT) {
    evas_object_smart_callback_add(slider, "delay,changed", on_float_changed, setting);
    for(i=0;i<ma_count(setting->childs);i++) {
      sub = ma_data(setting->childs, i);
      assert(sub->type == setting->type);
      if (!strcmp("PARENT_SETTING_MIN", sub->name))
	fmin = *(float*)sub->data;
      else if (!strcmp("PARENT_SETTING_MAX", sub->name))
	fmax = *(float*)sub->data;
    }
    elm_slider_min_max_set(slider, fmin, fmax);
    elm_slider_value_set(slider, *(float*)setting->data);
  }
  elm_box_pack_end(vbox, slider);
  evas_object_show(slider);
}

void filter_settings_create_gui(Eina_List *chain_node, Evas_Object *box)
{
  int i;
  Evas_Object *vbox, *btn;
  Filter_Chain *fc = eina_list_data_get(chain_node);
  
  if (!fc->f->settings || !ea_count(fc->f->settings))
    return;
  
  fc->frame = elm_frame_add(win);
  evas_object_size_hint_weight_set(fc->frame, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(fc->frame, EVAS_HINT_FILL, 0);
  elm_box_pack_end(box, fc->frame);
  elm_object_text_set(fc->frame, fc->f->fc->name);
  
  vbox = elm_box_add(fc->frame);
  elm_box_horizontal_set(vbox, EINA_FALSE);
  evas_object_size_hint_weight_set(vbox, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(vbox, EVAS_HINT_FILL, 0);
  elm_object_content_set(fc->frame, vbox);
  
  btn = elm_button_add(fc->frame);
  elm_object_text_set(btn, "remove");
  evas_object_smart_callback_add(btn, "clicked", &on_remove_filter, chain_node);
  elm_box_pack_end(vbox, btn);
  evas_object_show(btn);
  
  
  for(i=0;i<ea_count(fc->f->settings);i++)
    slider_insert(vbox, ea_data(fc->f->settings, i));
  
  evas_object_show(vbox);
  evas_object_show(fc->frame);
}

void del_filter_settings(void)
{
  Filter_Chain *chain_data;
    
  if (filter_last_selected) {
    chain_data = eina_list_data_get(filter_last_selected);
    elm_box_unpack(tab_filter, chain_data->frame);
    evas_object_del(chain_data->frame);
    chain_data->frame = NULL;
  }
  
  filter_last_selected = NULL;
}

Mat_Cache *mat_cache_new(void)
{
  Mat_Cache *mat_cache = calloc(sizeof(Mat_Cache), 1);
  
  mat_cache->scale_max = -1;
  mat_cache->images = eina_array_new(64);
  
  return mat_cache;
}

void mat_cache_flush(Mat_Cache *mat_cache)
{
  int i;
  
  for(i=0;i<=mat_cache->scale_max;i++) {
    if (mat_cache->mats[i]) {
      eina_matrixsparse_size_set(mat_cache->mats[i], 1, 1);
      eina_matrixsparse_data_idx_set(mat_cache->mats[i], 0, 0, NULL);
    }
    mat_cache->low_of_layer[i] = NULL;
    mat_cache->high_of_layer[i] = NULL;
  }
  
  while (ea_count(mat_cache->images)) {
    //here segfault!
    evas_object_del(ea_pop(mat_cache->images));
  }
}

void mat_cache_del(Mat_Cache *mat_cache)
{
  mat_cache_flush(mat_cache);
  
  eina_array_free(mat_cache->images);
  
  free(mat_cache);
}

/*void mat_free_func(void *user_data, void *cell_data)
{
  Evas_Object *img = cell_data;
  
  evas_object_del(img);
}*/

void mat_cache_max_set(Mat_Cache *mat_cache, int scale)
{
  int i;
  
  if (scale <= mat_cache->scale_max)
    return;
  
  mat_cache->mats = realloc(mat_cache->mats, sizeof(Eina_Matrixsparse*)*(scale+1));
  mat_cache->high_of_layer = realloc(mat_cache->high_of_layer, sizeof(Evas_Object*)*(scale+1));
  mat_cache->low_of_layer = realloc(mat_cache->low_of_layer, sizeof(Evas_Object*)*(scale+1));
  
  for(i=mat_cache->scale_max+1;i<=scale;i++) {
    mat_cache->mats[i] = NULL;
    mat_cache->high_of_layer[i] = NULL;
    mat_cache->low_of_layer[i] = NULL;
  }
    
  mat_cache->scale_max = scale;
}

void *mat_cache_get(Mat_Cache *mat_cache, int scale, int x, int y)
{
  long unsigned int mx, my;
  
  if (scale > mat_cache->scale_max)
    return NULL;

  if (!mat_cache->mats[scale])
    return NULL;

  eina_matrixsparse_size_get(mat_cache->mats[scale], &mx, &my);

  if (x >= mx)
    return NULL;

 if (y >= my)
    return NULL;
 
  return eina_matrixsparse_data_idx_get(mat_cache->mats[scale], x, y);
}

void mat_cache_obj_stack(Mat_Cache *mat_cache, Evas_Object *obj, int scale)
{
  int i;
  
  mat_cache_max_set(mat_cache, scale);
  
  if (mat_cache->high_of_layer[scale]) {
    evas_object_stack_above(obj, mat_cache->high_of_layer[scale]);
    mat_cache->high_of_layer[scale] = obj;
    return;
  } 
  
  for(i=scale-1;i>=0;i--)
    if (mat_cache->low_of_layer[i]) {
      evas_object_stack_below(obj, mat_cache->low_of_layer[i]);
      mat_cache->low_of_layer[scale] = obj;
      mat_cache->high_of_layer[scale] = obj;
      return;
    }
    
    
  for(i=scale+1;i<=size.scaledown_max;i++)
    if (mat_cache->high_of_layer[i]) {
      evas_object_stack_above(obj, mat_cache->high_of_layer[i]);
      mat_cache->low_of_layer[scale] = obj;
      mat_cache->high_of_layer[scale] = obj;
      return;
    }
    
  mat_cache->low_of_layer[scale] = obj;
  mat_cache->high_of_layer[scale] = obj;
}

float actual_scale_get()
{
  int x,y,w,h,grid_w,grid_h;
  
  elm_scroller_region_get(scroller, &x, &y, &w, &h);
  evas_object_size_hint_min_get(grid, &grid_w, &grid_h);
    
  return (float)size.width / grid_w;
}

void mat_cache_set(Mat_Cache *mat_cache, int scale, int x, int y, void *data)
{
  long unsigned int mx, my;
  
  mat_cache_max_set(mat_cache, scale);
  
  if (!mat_cache->mats[scale])
    mat_cache->mats[scale] = eina_matrixsparse_new(x+1, y+1, NULL, NULL);  
  
  eina_matrixsparse_size_get(mat_cache->mats[scale], &mx, &my);

  if (x >= mx) {
    eina_matrixsparse_size_set(mat_cache->mats[scale], x+1, my);
    //FIXME this should not be necessary!
    eina_matrixsparse_size_get(mat_cache->mats[scale], &mx, &my);
  }
  
  if (y >= my) {
    eina_matrixsparse_size_set(mat_cache->mats[scale], mx, y+1);
    //FIXME just tried if duplicate of above code helbs
    eina_matrixsparse_size_get(mat_cache->mats[scale], &mx, &my);
  }
  
  //printf("pos: %dx%d before %dx%d\n", eina)
  eina_matrixsparse_data_idx_set(mat_cache->mats[scale], x, y, data);
  
  eina_array_push(mat_cache->images, data);
}

void elm_exit_do(void *data, Evas_Object *obj)
{
  elm_exit();
}

void workerfinish_schedule(void (*func)(void *data, Evas_Object *obj), void *data, Evas_Object *obj)
{
  if (!worker) {
    func(data, obj);
  }
  else {
    pending_action = func;
    pending_data = data;
    pending_obj = obj;
  }
}

static void
_process_tile(void *data, Ecore_Thread *th)
{
  _Img_Thread_Data *tdata = data;
    
  lime_render_area(&tdata->area, sink, tdata->t_id);
}

int bench_do(void)
{
  int x, y, w, h;
  Filter *f;
  
  if (bench_idx == 0)
    mat_cache_flush(mat_cache);
  
  bench_delay_start();
  
  elm_scroller_region_get(scroller, &x, &y, &w, &h);

  if (bench[bench_idx].x == -1) {
    elm_exit_do(NULL, NULL);
    return 1;
  }
  
  if (bench[bench_idx].new_f_core) {
    select_filter_func = bench[bench_idx].new_f_core->filter_new_f;
    insert_before_do(NULL, NULL);
  }
  
  if (bench[bench_idx].setting_float) {
    f = eina_list_data_get(eina_list_nth(filter_chain, eina_list_count(filter_chain) - 1 - bench[bench_idx].filter));
    lime_setting_float_set(f, bench[bench_idx].setting_float, *(float*)bench[bench_idx].val);
    delgrid();
  }
  else if (bench[bench_idx].setting_int) {
    f = eina_list_data_get(eina_list_nth(filter_chain, bench[bench_idx].filter));
    lime_setting_int_set(f, bench[bench_idx].setting_float, *(float*)bench[bench_idx].val);
    delgrid();
  } 
  
  if (bench[bench_idx].scale == -1)
    fit = 1;
  else {
    fit = 1;
    scale_goal = bench[bench_idx].scale;
  }
  //grid_setsize();
  
  elm_scroller_region_show(scroller, bench[bench_idx].x, bench[bench_idx].y, 1, 1);
  
  bench_idx++;

  fprintf(stderr, "benchmark progress: %d\n", bench_idx);
  
  return 0;
}

void _insert_image(_Img_Thread_Data *tdata)
{
    //image data has a ref counter, need to decrease it!
  evas_object_image_data_set(tdata->img, tdata->buf);
  evas_object_image_data_update_add(tdata->img, 0, 0, TILE_SIZE, TILE_SIZE);
  evas_object_show(tdata->img);
  
  elm_grid_pack(grid, tdata->img, tdata->packx, tdata->packy, tdata->packw, tdata->packh);
  
  mat_cache_obj_stack(mat_cache, tdata->img, tdata->scale);
  
  evas_object_clip_set(tdata->img, clipper);
}

Eina_Bool _display_preview(void *data)
{
  int i;
    
  evas_object_show(clipper);
  mat_cache_del(mat_cache_old);
  mat_cache_old = NULL;
  grid_setsize();
  for(i=0;i<ea_count(finished_threads);i++) {
    _insert_image(ea_data(finished_threads, i));
    free(ea_data(finished_threads, i));
  }
  eina_array_free(finished_threads);
  finished_threads = NULL;
  
  
      
  printf("finaly delay forced: %f\n", bench_delay_get());

  preview_timer = NULL;

    
  return ECORE_CALLBACK_CANCEL;
}

static void
_finished_tile(void *data, Ecore_Thread *th)
{
  int i;
  void (*pend_tmp_func)(void *data, Evas_Object *obj);
  _Img_Thread_Data *tdata = data;
  Evas_Object *pend_tmp_obj;
  void *pend_tmp_data;
  double delay = bench_delay_get();
  
  assert(tdata->t_id != -1);
  
  thread_ids[tdata->t_id] = 0;
  tdata->t_id = -1;
    
  if (tdata->scaled_preview)
    preview_tiles--;
  
  worker--;
  
  if (delay < max_reaction_delay)
    fill_scroller_preview();
  
  if (!pending_action)
    fill_scroller();
  
  if (mat_cache_old) {
    if (preview_tiles || (!pending_action && delay < high_quality_delay && worker)) {
      printf("delay for now: %f\n", delay);
      eina_array_push(finished_threads, data);
      
      if (!preview_timer && !preview_tiles) {
	preview_timer = ecore_timer_add((high_quality_delay - delay)*0.001, &_display_preview, NULL);
      }

      return;
    }
    
    if (preview_timer) {
      printf("no force!\n");
      ecore_timer_del(preview_timer);
      preview_timer = NULL;
    }
    
    printf("finaly delay: %f\n", bench_delay_get());
    
    evas_object_show(clipper);
    mat_cache_del(mat_cache_old);
    mat_cache_old = NULL;
    grid_setsize();
    for(i=0;i<ea_count(finished_threads);i++) {
      _insert_image(ea_data(finished_threads, i));
      free(ea_data(finished_threads, i));
    }
    eina_array_free(finished_threads);
    finished_threads = NULL;
  }

  _insert_image(tdata);

  
  if (!worker && pending_action) {
    pend_tmp_func = pending_action;
    pend_tmp_data = pending_data;
    pend_tmp_obj = pending_obj;
    pending_action = NULL;
    pending_data = NULL;
    pending_obj = NULL;
    pend_tmp_func(pend_tmp_data, pend_tmp_obj);
  }
    
  /*if (bench || verbose) {
    if (tdata->scaled_preview)
      bench_delay_stop(BENCHMARK_DELAY_SCALED);
    else
      bench_delay_stop(BENCHMARK_DELAY_FULL);
  }
  
  _insert_image(tdata);
  
  worker--;
  
  free(tdata);
  
  if (pending_action) {
    if (worker) return;
    
    pend_tmp_func = pending_action;
    pend_tmp_data = pending_data;
    pend_tmp_obj = pending_obj;
    pending_action = NULL;
    pending_data = NULL;
    pending_obj = NULL;
    pend_tmp_func(pend_tmp_data, pend_tmp_obj);
  }
  else if (tdata->t_id != max_workers && tdata->t_id != -1) {
    fill_scroller();
    
    if (bench)
      while (!worker) {
	if (bench_do())
	  return;
	fill_scroller();
      }
  }*/

}

int lock_free_thread_id(void)
{
  int i;
  
  for(i=0;i<max_workers;i++)
    if (!thread_ids[i]) {
      thread_ids[i] = 1;
      return i;
    }
    
  abort();
}

int fill_area(int xm, int ym, int wm, int hm, int minscale, int preview)
{
  int x,y,w,h;
  int i, j;
  Evas_Object *cell;
  uint8_t *buf;
  int scale;
  int scalediv;
  Rect area;
  float actual_scalediv;
  Evas_Object *img;
  int minx, miny, maxx, maxy;
  int scale_start;
  int actual_scale;
  int started = 0;
  
  _Img_Thread_Data *tdata;
  
  if (forbid_fill)
    return 0;
  
  if (worker >= max_workers)
    return 0;
  
  elm_scroller_region_get(scroller, &x, &y, &w, &h);
  
  if (!w || !h) {
    printf("FIXME avoid fill_scroller_preview: scroller does not yet have region!");
    return 0;
  }
  
  actual_scalediv = actual_scale_get();
  
  x += xm;
  y += ym;
  w += wm;
  h += hm;
  
  x *= actual_scalediv;
  y *= actual_scalediv;
  w *= actual_scalediv;
  h *= actual_scalediv;
  
  actual_scale = 0;
  while (actual_scalediv >= 2.0) {
    actual_scalediv *= 0.5;
    actual_scale++;
  }

  minscale = actual_scale + minscale;
  
  if (minscale > size.scaledown_max)
    minscale = size.scaledown_max;
  
  scale_start = minscale + MAX_FAST_SCALEDOWN;
  
  if (scale_start > size.scaledown_max)
    scale_start = size.scaledown_max;

  for(scale=scale_start;scale>=minscale;scale--) {
    //additional scaledown for preview
    scalediv = ((uint32_t)1) << scale;
    for(j=y/TILE_SIZE/scalediv;j<(y+h+TILE_SIZE*scalediv-1)/TILE_SIZE/scalediv;j++)
      for(i=x/TILE_SIZE/scalediv;i<(x+w+TILE_SIZE*scalediv-1)/TILE_SIZE/scalediv;i++) {
	cell = mat_cache_get(mat_cache, scale, i, j);
	
	if (i*TILE_SIZE*scalediv >= size.width || j*TILE_SIZE*scalediv >= size.height)
	  continue;
		
	if (!cell) {
	  img = evas_object_image_filled_add(evas_object_evas_get(win));
	  evas_object_image_colorspace_set(img, EVAS_COLORSPACE_ARGB8888);
	  evas_object_image_alpha_set(img, EINA_FALSE);
	  evas_object_image_size_set(img, TILE_SIZE, TILE_SIZE);
	  evas_object_image_smooth_scale_set(img, EINA_FALSE); 
	  evas_object_image_scale_hint_set(img, EVAS_IMAGE_SCALE_HINT_STATIC);
	  evas_object_image_scale_hint_set(img, EVAS_IMAGE_CONTENT_HINT_STATIC);
	  
	  minx = i*TILE_SIZE*scalediv;
	  miny = j*TILE_SIZE*scalediv;
	  maxx = minx + TILE_SIZE*scalediv;
	  maxy = miny + TILE_SIZE*scalediv;
	  
	  if (minx < 0) {
	    minx = 0;
	  }
	  if (miny < 0) {
	    miny = 0;
	  }
	  if (maxx > size.width) {
	    maxx = size.width;
	  }
	  if (maxy > size.height) {
	    maxy = size.height;
	  }
	  
	  //FIXME does not work, clipping is at the moment done uncoditionally in _image_insert
	  /*if (crop)
	    evas_object_clip_set(img, clipper);*/
	  
	  area.corner.scale = scale;
	  area.corner.x = i*TILE_SIZE;
	  area.corner.y = j*TILE_SIZE;  
	  area.width = TILE_SIZE;
	  area.height = TILE_SIZE;
	  
	  tdata = calloc(sizeof(_Img_Thread_Data), 1);
	 
  	  buf = evas_object_image_data_get(img, EINA_TRUE);
	  
	  //elm_grid_pack(grid, img, i*TILE_SIZE*scalediv, j*TILE_SIZE*scalediv, TILE_SIZE*scalediv, TILE_SIZE*scalediv);
	  
	  tdata->buf = buf;
	  tdata->img = img;
	  tdata->scale = scale;
	  tdata->area = area;
	  tdata->packx = i*TILE_SIZE*scalediv;
	  tdata->packy = j*TILE_SIZE*scalediv;
	  tdata->packw = TILE_SIZE*scalediv;
	  tdata->packh = TILE_SIZE*scalediv;

	  /*if (scale == scale_start)
	    tdata->scaled_preview = 1;
	  else
	    tdata->scaled_preview = 0;*/
	  
	  tdata->scaled_preview = preview;
	  
	  tdata->t_id = lock_free_thread_id();
	  
	  assert(buf);
	  filter_memsink_buffer_set(sink, tdata->buf, tdata->t_id);
	  
	  mat_cache_set(mat_cache, scale, i, j, img);
	  	  
	  worker++;
	  
	  ecore_thread_run(_process_tile, _finished_tile, NULL, tdata);
	  started++;

	  if (worker >= max_workers)
	    return started;
	}
      }
  }
  
  return started;
}

void fill_scroller_preview(void)
{
  preview_tiles += fill_area(0,0,0,0, MAX_FAST_SCALEDOWN, 1);
}

static void fill_scroller(void)
{
  int x, y, w, h, grid_w, grid_h;
  float scale;
  
  evas_object_size_hint_min_get(grid, &grid_w, &grid_h);
  elm_scroller_region_get(scroller, &x, &y, &w, &h);
  
  if (!w || !h) {
    printf("FIXME avoid fill_scroller: scroller does not yet have region!");
    return;
  }

  if (grid_w && grid_h) {
  scale = size.width / grid_w;	
  if ((float)size.height / grid_h > scale)
    scale = (float)size.height / grid_h;
  }
  else
    scale = INFINITY;
  sprintf(pos_lbl_buf, "%dx%d - %dx%d @ %.2f\n", x, y, w, h, scale);
  elm_object_text_set(pos_label, pos_lbl_buf);
  
  if (!w)
    w++;
  if (!h)
    h++;

  if (fill_area(0,0,0,0, MAX_FAST_SCALEDOWN, 0))
    return;
    
  if (fill_area(0,0,0,0,0, 0))
    return;
}

static void
on_scroller_move(void *data, Evas_Object *obj, void *event_info)
{
  if (forbid_fill)
    return;
  
  fill_scroller();
}

static void
on_done(void *data, Evas_Object *obj, void *event_info)
{
  // quit the mainloop (elm_run function will return)
  workerfinish_schedule(&elm_exit_do, NULL, NULL);
}


void group_select_do(void *data, Evas_Object *obj)
{
  int failed;
  const char *filename;
  
  delgrid();
    
  File_Group *group = eina_inarray_nth(files, file_idx);
  
  group_idx = *(int*)data;
    
  failed = 1;
  
  while(failed) {
    if (group_idx == eina_inarray_count(group->files)) {
      group_idx = 0;
    }
    
    filename = ((Tagged_File*)eina_inarray_nth(group->files, group_idx))->filename;
    if (!filename) {
      group_idx++;
      continue;
    }
      
    strcpy(image_file, filename);
    lime_setting_string_set(load, "filename", image_path);
    
    failed = lime_config_test(sink);
    if (failed)
      group_idx++;
  }
  size_recalc();

  fill_scroller_preview();
  fill_scroller();
}

void tags_select_do(void *data, Evas_Object *obj)
{
  int failed;
  const char *filename;
  
  delgrid();
    
  File_Group *group = eina_inarray_nth(files, file_idx);
  
  group_idx = *(int*)data;
    
  failed = 1;
  
  while(failed) {
    if (group_idx == eina_inarray_count(group->files)) {
      group_idx = 0;
    }
    
    filename = ((Tagged_File*)eina_inarray_nth(group->files, group_idx))->filename;
    if (!filename) {
      group_idx++;
      continue;
    }
      
    strcpy(image_file, filename);
    lime_setting_string_set(load, "filename", image_path);
    
    failed = lime_config_test(sink);
    if (failed)
      group_idx++;
  }

  size_recalc();

  fill_scroller_preview();
  fill_scroller();
}

void delgrid(void)
{  
  bench_delay_start();
  
  if (mat_cache_old)
    return;
  
  //mat_cache_flush(mat_cache);
  mat_cache_old = mat_cache;
  mat_cache = mat_cache_new();
  
  finished_threads = eina_array_new(16);
}

int files_similar(const char *a, const char *b)
{
  int had_point = 0;
  
  if (!a || !b)
    return 0;
  
  while (*a == *b && *a != '\0' && *b != '\0') {
    if ((*a == '.') && (*b == '.'))
      had_point = 1;
    a++;
    b++;
    if (*a == '/' && *b == '/')
      had_point = 0;
  }
  
  if ((*a == '.') && (*b == '.'))
      had_point = 1;
  
  return had_point;
}

int wrap_files_idx(float idx)
{
  if (idx < 0)
    return eina_inarray_count(files)-1;
  if (idx >= eina_inarray_count(files))
    return 0;
  return idx;
}

void step_image_do(void *data, Evas_Object *obj);

static void on_jump_image(void *data, Evas_Object *obj, void *event_info)
{
  if (file_idx == (int)elm_slider_value_get(file_slider))
    return;
  
  workerfinish_schedule(&step_image_do, NULL, NULL);
}


static void
on_group_select(void *data, Evas_Object *obj, void *event_info)
{ 
  //FIXME reenable, avoid double call
  //workerfinish_schedule(&group_select_do, data, obj);
}

typedef struct {
  const char *tag;
  File_Group *group;
}
Tags_List_Item_Data;

Eina_Bool tags_hash_func(const Eina_Hash *hash, const void *key, void *data, void *fdata)
{
  Tags_List_Item_Data *tag = malloc(sizeof(Tags_List_Item_Data));
 
  tag->tag = data;
  tag->group = fdata;
  
  elm_genlist_item_append(tags_list, tags_list_itc, tag, NULL, ELM_GENLIST_ITEM_NONE, NULL, NULL);

  return 1;
}


Eina_Bool tags_hash_filter_func(const Eina_Hash *hash, const void *key, void *data, void *fdata)
{
  elm_genlist_item_append(tags_filter_list, tags_filter_itc, data, NULL, ELM_GENLIST_ITEM_NONE, NULL, NULL);
  
  return 1;
}

typedef struct {
  File_Group *group;
  int valid;
} Filter_Check_Data;

Eina_Bool tag_filter_check_and_hash_func(const Eina_Hash *hash, const void *key, void *data, void *fdata)
{
  Filter_Check_Data *check = fdata;
  
  if (!eina_hash_find(check->group->tags, data))
    check->valid = 0;
  
  return 1;
}

Eina_Bool tag_filter_check_or_hash_func(const Eina_Hash *hash, const void *key, void *data, void *fdata)
{
  Filter_Check_Data *check = fdata;
  
  if (eina_hash_find(check->group->tags, data))
    check->valid = 1;
  
  return 1;
}

int group_in_filters(File_Group *group, Eina_Hash *filters)
{
  Filter_Check_Data check;
  
  if (group->tag_rating < tags_filter_rating)
    return 0;
  
  //for or hash!
  if (!eina_hash_population(filters))
    check.valid = 1;
  else
    check.valid = 0;
  check.group = group;
  
  eina_hash_foreach(filters, tag_filter_check_or_hash_func, &check);
  
  return check.valid;
}

void step_image_do(void *data, Evas_Object *obj)
{
  int i;
  int *idx_cp;
  int start_idx;
  int failed;
  File_Group *group;
  const char *filename;
  Elm_Object_Item *item;
  
  file_idx = elm_slider_value_get(file_slider);
  
  bench_delay_start();
  
  assert(!worker);
  
  assert(files);
  
  if (!eina_inarray_count(files))
    return;
  
  delgrid();
    
  forbid_fill++;
  
  start_idx = file_idx;
  
  group = eina_inarray_nth(files, file_idx);
    
  failed = 1;
  
  while (failed) {
    group_idx = 0;
    
    
    if (group_in_filters(group, tags_filter)) {
      while(failed) {
	if (group_idx == eina_inarray_count(group->files))
	  break;
	
	filename = ((Tagged_File*)eina_inarray_nth(group->files, group_idx))->filename;
	//FIXME better file ending list (no static file endings!)
	if (!filename || (!eina_str_has_extension(filename, ".jpg") 
		       && !eina_str_has_extension(filename, ".JPG")
		       && !eina_str_has_extension(filename, ".tif")
		       && !eina_str_has_extension(filename, ".TIF")
		       && !eina_str_has_extension(filename, ".tiff")
		       && !eina_str_has_extension(filename, ".TIFF"))) {
	  group_idx++;
	  continue;
	}
	  
	strcpy(image_file, filename);
	lime_setting_string_set(load, "filename", image_path);

	failed = lime_config_test(sink);
	if (failed)
	  group_idx++;
      }
    }
    else
      failed = 1;
    
    if (!failed)
      break;
    
    file_idx = wrap_files_idx(file_idx + file_step);
    group = eina_inarray_nth(files, file_idx);
    
    if (start_idx == file_idx){
      elm_exit_do(NULL, NULL);
      return;
    }
  }
  
  elm_list_clear(group_list);
  for(i=0;i<eina_inarray_count(group->files);i++)
    if (((Tagged_File*)eina_inarray_nth(group->files, i))->filename) {
      idx_cp = malloc(sizeof(int));
      *idx_cp = i;
      item = elm_list_item_append(group_list, ((Tagged_File*)eina_inarray_nth(group->files, i))->filename, NULL, NULL, &on_group_select, idx_cp);
      if (group_idx == i) {
	elm_list_item_selected_set(item, EINA_TRUE);
      }
    }

  elm_list_go(group_list);
  
  //update tag list
  elm_genlist_clear(tags_list);
  eina_hash_foreach(known_tags, tags_hash_func, group);
  
  //update tag rating
  elm_segment_control_item_selected_set(elm_segment_control_item_get(seg_rating, group->tag_rating), EINA_TRUE);
  
  elm_slider_value_set(file_slider, file_idx+0.1);
  
  forbid_fill--;
 
  size_recalc();
  
  fill_scroller_preview();
  fill_scroller();
}

void del_file_done(void *data, Eio_File *handler)
{
  printf("moved %s to delete\n", (char*)data);
}

void del_file_error(void *data, Eio_File *handler, int error)
{
  printf("del failed with %s on %s!\n", strerror(error), (char*)data);
}

void file_group_del(File_Group *group)
{
  //FIXME
}

void delete_image_do(void *data, Evas_Object *obj)
{
  char *dest = malloc(EINA_PATH_MAX);
  char *dest_file = dest + (image_file - image_path);

  Tagged_File *file;
  File_Group *group;
  
  if (!ecore_file_exists("delete"))
    ecore_file_mkdir("delete");

  group = eina_inarray_nth(files, file_idx);
  
  EINA_INARRAY_FOREACH(group->files, file) {
    if (file->filename) {
      strcpy(dest, image_path);
      sprintf(dest_file, "delete/%s", file->filename);
      strcpy(image_file, file->filename);
      eio_file_move(image_path, dest, NULL, &del_file_done, &del_file_error, file->filename);
    }
    if (file->sidecar) {
      strcpy(dest, image_path);
      sprintf(dest_file, "delete/%s", file->sidecar);
      strcpy(image_file, file->sidecar);
      eio_file_move(image_path, dest, NULL, &del_file_done, &del_file_error, file->sidecar);
    }
  }
  
  //ATTENTION! inarray, group points to different group aufter inarray_remove_at!!!
  if (!eina_inarray_remove_at(files, file_idx))
    abort();
  
  elm_slider_min_max_set(file_slider, 0, eina_inarray_count(files)-1);
  
  step_image_do(NULL, NULL);
  
  free(dest);
  file_group_del(group);
}


typedef struct {
  int w, h;
  int new_w, new_h;
  int co_x, co_y;
  int cn_x, cn_y;
  int c_w, c_h;
} _Grid_Zoom;

Elm_Transit_Effect *_trans_grid_zoom_contex_new(int w, int h, int new_w, int new_h, int co_x, int co_y, int cn_x, int cn_y, int c_w, int c_h)
{
  _Grid_Zoom *zoom;
  
  zoom = malloc(sizeof(_Grid_Zoom));
  if (!zoom) return NULL;
  
  zoom->w = w;
  zoom->h = h;
  zoom->new_w = new_w;
  zoom->new_h = new_h;
  
  zoom->co_x = co_x;
  zoom->co_y = co_y;
  zoom->cn_x = cn_x;
  zoom->cn_y = cn_y;
  zoom->c_w = c_w;
  zoom->c_h = c_h;
  
  return zoom;
}

void _trans_grid_zoom_contex_del(void *data, Elm_Transit *transit)
{
  free(data);
  
  if (forbid_fill)
    return;
  
  //grid_setsize();
  fill_scroller();
}

void _trans_grid_zoom_trans_cb(Elm_Transit_Effect *effect, Elm_Transit *transit, double progress)
{
  _Grid_Zoom *zoom = effect;
  
  double inv = 1.0 - progress;
  
  //elm_grid_size_set(grid, zoom->w*(1.0-progress)+zoom->new_w*(progress), zoom->h*(1.0-progress)+zoom->new_h*progress);
  evas_object_size_hint_min_set(grid,  zoom->w*(1.0-progress)+zoom->new_w*(progress), zoom->h*(1.0-progress)+zoom->new_h*progress);
  
  elm_box_recalculate(gridbox);
  
  elm_scroller_region_show(scroller, 
			   zoom->co_x*inv+zoom->cn_x*progress-zoom->c_w/2,
			   zoom->co_y*inv+zoom->cn_y*progress-zoom->c_h/2,
			   zoom->c_w,
			   zoom->c_h
  );
  
  if (!forbid_fill)
    fill_scroller();
  
}



/*static void
export_error_cb(void *data, Eio_File *handler, int error)
{
  printf("error on copy export");
}*/

/*static void
export_done_cb(void *data, Eio_File *handler)
{
  char *filename;
  char dst[EINA_PATH_MAX];
  
  if (export_list)
    elm_progressbar_value_set(export->eo_progress, 1.0 - (double)ea_count(export_list)/export_count);
  else
    elm_progressbar_value_set(export->eo_progress, 0.0);    
  
  if (export_list) {
    filename = eina_array_pop(export_list);
    if (!ea_count(export_list)) {
      eina_array_free(export_list);
      export_list = NULL;
      //elm_object_disabled_set(
    }
    sprintf(dst, "/home/hendrik/weltreise/foto_blog/%s", filename);
    eio_file_copy(filename, strdup(dst), NULL, export_done_cb, export_error_cb, NULL);
  }
}*/

/*static void
on_exe_images_cp(void *data, Evas_Object *obj, void *event_info)
{
  int i, j;
  File_Group *group;
  const char *filename;
  char dst[64];
  
  elm_progressbar_value_set(export->eo_progress, 0.0);
  
  for(i=0;i<eina_inarray_count(files);i++) {
    group = eina_inarray_nth(files, i);
    if (group_in_filters(group, tags_filter)) {
      for(j=0;j<eina_inarray_count(group->files);j++) {
	filename = ((Tagged_File*)eina_inarray_nth(group->files, j))->filename;
	if (filename && strstr(filename, ".JPG")) {
	  if (!export_list)
	    export_list = eina_array_new(32);
	  eina_array_push(export_list, filename);
	  //printf("in f: %s\n", filename);
	  //sprintf(dst, "/media/blacksheep/weltreise/export/%s", filename);
	  //ecore_file_cp(filename, dst);
	}
      }
    }
  }
  
  if (export_list) {
    export_count = ea_count(export_list);
    filename = eina_array_pop(export_list);
    if (!ea_count(export_list)) {
      eina_array_free(export_list);
      export_list = NULL;
    }
    sprintf(dst, "/home/hendrik/weltreise/foto_blog/%s", filename);
    eio_file_copy(filename, strdup(dst), NULL, export_done_cb, export_error_cb, NULL);
  }
    
}*/

typedef struct {
  char *filename;
  Export_Data *export;
} Export_Job;

void start_single_export(Export_Data *export, char *filename)
{
  Eina_Array *dirs;
  char dst[2048];
  Export_Job *job;
  
  if (!filename) {
    if (!ea_count(export->list)) {
      export->list = NULL;
      return;
    }
    filename = eina_array_pop(export->list);
  }
  
  job = malloc(sizeof(Export_Job));
  job->export = export;
  job->filename = filename;
  
  dirs = eina_file_split(dir);
  if (export->server)
    sprintf(dst, "rsync -rt --progress \"./%s\" %s:\"\'%s/%s/%s\'\"", filename, export->server,export->path, export->dir, filename);
  else
    sprintf(dst, "rsync -rt --progress \"./%s\" \'%s/%s/%s\'", filename, export->path, export->dir, filename);
  eina_array_free(dirs);
  //FIXME free dirs string(s?)?
  printf("start export of: %s\n", dst);
  ecore_exe_run(dst, job);
}


static Eina_Bool _rsync_term(void *data, int type, void *event)
{
  int i;
  Ecore_Exe_Event_Del *del_event = event;
  Export_Job *job = ecore_exe_data_get(del_event->exe);
  
  assert(job);
  
  if (job->filename) {
    printf("file: %s return code: %d\n", job->filename, del_event->exit_code);

    if (del_event->exit_code)
      start_single_export(job->export, job->filename);
    else
      start_single_export(job->export, NULL);
  }
  else 
    for(i=0;i<RSYNC_INSTANCE_COUNT;i++)
      start_single_export(job->export, NULL);
  
  if (job->export->list) {
    elm_progressbar_value_set(job->export->eo_progress, 1.0 - (double)ea_count(job->export->list)/job->export->count);
    printf("around %d remaining\n", ea_count(job->export->list));
  }
  else {
    elm_progressbar_value_set(job->export->eo_progress, 100.0);
    printf("export_finished\n");
  }
  
  free(job);
  
  return ECORE_CALLBACK_PASS_ON;
}

static void
on_exe_images_rsync(void *data, Evas_Object *obj, void *event_info)
{
  int i, j;
  File_Group *group;
  const char *filename;
  char dst[2048];
  Eina_Array *dirs;
  Export_Data *export = calloc(sizeof(Export_Data), 1);
  Export_Job *job;

  export->extensions = elm_entry_entry_get(export_extensions);
  if (export->extensions && !strlen(export->extensions)) {
    export->extensions = NULL;
  }
  export->path = strdup(elm_entry_entry_get(export_path));
  
  //progress bar
  export->eo_progress = elm_progressbar_add(export_box);
  evas_object_size_hint_weight_set(export->eo_progress, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(export->eo_progress, EVAS_HINT_FILL, 0);
  evas_object_show(export->eo_progress);
  elm_box_pack_end(export_box, export->eo_progress);
  
  elm_progressbar_value_set(export->eo_progress, 0.0);
  
  for(i=0;i<eina_inarray_count(files);i++) {
    group = eina_inarray_nth(files, i);
    if (group_in_filters(group, tags_filter)) {
      for(j=0;j<eina_inarray_count(group->files);j++) {
	filename = ((Tagged_File*)eina_inarray_nth(group->files, j))->filename;
	if (filename && (!export->extensions || strstr(filename, export->extensions))) {
	  if (!export->list)
	    export->list = eina_array_new(32);
	  eina_array_push(export->list, filename);
	}
      }
      filename = group->sidecar;
	if (filename && (!export->extensions || strstr(filename, export->extensions))) {
	  if (!export->list)
	    export->list = eina_array_new(32);
	  eina_array_push(export->list, filename);
	}
    }
  }
  
  if (export->list) {
    export->count = ea_count(export->list);
    filename = eina_array_data_get(export->list, eina_array_count(export->list)-1);
    if (!ea_count(export->list)) {
      eina_array_free(export->list);
      export->list = NULL;
    }
    dirs = eina_file_split(dir);
    export->dir = strdup(eina_array_data_get(dirs, eina_array_count(dirs)-1));
    //FIXME for export to server!
    //ecore_exe_run(dst, NULL);
	 eina_array_free(dirs);
    
    if (export->path[strlen(export->path)-1] == '/')
      export->path[strlen(export->path)-1] = '\0';
    
    if (strstr(export->path, ":/")) {
      export->server = strdup(export->path);
      export->path = strstr(export->server, ":/");
      export->path[0] = '\0';
      export->path++;
      
      job = calloc(sizeof(Export_Job), 1);
      job->export = export;
      
      //FIXME read server from path!
      sprintf(dst, "ssh %s mkdir -p \"\'%s/%s\'\"", export->server, export->path, export->dir);
      printf("mkdir: %s\n", dst);
      
      ecore_exe_run(dst, job);
    }
    else {
      sprintf(dst, "%s/%s", export->path, export->dir);
      printf("make dir: %s\n", dst);
      ecore_file_mkdir(dst);
      start_single_export(export, NULL);
    }
      
    
    
    //FIXME doesn't wait for the mkdir to quit!!!
    //sprintf(dst, "rsync -rt --progress \"./%s\" caren@technik-stinkt.de:\"\'/home/caren/fotos_upload/%s/%s\'\"", filename, eina_array_data_get(dirs, eina_array_count(dirs)-1), filename);
    //eio_file_copy(filename, strdup(dst), NULL, export_done_cb, export_error_cb, NULL);
    //free(eina_array_data_get(dirs, 0));
    //eina_array_free(dirs);
    //FIXME free dirs?
    //printf("%s\n", dst);
    //eina_array_pop(export_list);
    //ecore_exe_run(dst, filename);
    
  }
    
}

static void
on_fit_image(void *data, Evas_Object *obj, void *event_info)
{
  Elm_Transit *trans;
  int x,y,w,h;
  int grid_w, grid_h;
  float new_scaledown;
  
  if (fit != 1) {
    elm_scroller_region_get(scroller, &x, &y, &w, &h);
    
    evas_object_size_hint_min_get(grid, &grid_w, &grid_h);
    
    fit = 1;

    if (!w || !h) {
      scale_goal = 1.0;
    }
    else {
      scale_goal = (float)size.width / w;	
      if ((float)size.height / h > scale_goal)
	scale_goal = (float)size.height / h;
    }
    
    new_scaledown = 1.0/actual_scale_get()*scale_goal;
    
    trans = elm_transit_add();
    elm_transit_object_add(trans, grid);
    elm_transit_effect_add(trans, &_trans_grid_zoom_trans_cb, _trans_grid_zoom_contex_new(grid_w, grid_h, size.width/scale_goal,size.height/scale_goal,x+w/2,y+h/2,x/new_scaledown+w/(new_scaledown*2),y/new_scaledown+h/(new_scaledown*2),w,h), &_trans_grid_zoom_contex_del);
    elm_transit_duration_set(trans, 0.5);
    elm_transit_tween_mode_set(trans, ELM_TRANSIT_TWEEN_MODE_SINUSOIDAL);
    elm_transit_repeat_times_set(trans, 0);
    elm_transit_go(trans);
  }
}

static void
on_fullscreen(void *data, Evas_Object *obj, void *event_info)
{
  
  if (!fullscreen) {
    elm_bg_color_set(bg, 0, 0, 0);
    elm_win_fullscreen_set(win, EINA_TRUE);
    elm_box_unpack(main_vbox, vbox_bottom);
    evas_object_hide(vbox_bottom);
  }
  else {
    elm_win_fullscreen_set(win, EINA_FALSE);
    elm_box_pack_end(main_vbox, vbox_bottom);
    evas_object_show(vbox_bottom);
  }
  
  fullscreen = 1-fullscreen;
}

static void
on_origscale_image(void *data, Evas_Object *obj, void *event_info)
{
  Elm_Transit *trans;
  int x,y,w,h;
  int grid_w, grid_h;
  float new_scaledown;
  
  if (scale_goal != 1.0) {
    elm_scroller_region_get(scroller, &x, &y, &w, &h);
    
    evas_object_size_hint_min_get(grid, &grid_w, &grid_h);
    
    fit = 0;
    scale_goal = 1.0;
    new_scaledown = 1.0/actual_scale_get()*scale_goal;
    
    trans = elm_transit_add();
    elm_transit_object_add(trans, grid);
    elm_transit_effect_add(trans, &_trans_grid_zoom_trans_cb, _trans_grid_zoom_contex_new(grid_w, grid_h, size.width/scale_goal,size.height/scale_goal,x+w/2,y+h/2,x/new_scaledown+w/(new_scaledown*2),y/new_scaledown+h/(new_scaledown*2),w,h), &_trans_grid_zoom_contex_del);
    elm_transit_duration_set(trans, 0.5);
    elm_transit_tween_mode_set(trans, ELM_TRANSIT_TWEEN_MODE_SINUSOIDAL);
    elm_transit_repeat_times_set(trans, 0);
    elm_transit_go(trans);
  }
}


static void
on_next_image(void *data, Evas_Object *obj, void *event_info)
{
  file_step = 1;
  
  //if (grid && )
  
  elm_slider_value_set(file_slider, wrap_files_idx(elm_slider_value_get(file_slider)+1)+0.1);
  
  workerfinish_schedule(&step_image_do, NULL, NULL);
}

static void
on_delete_image(void *data, Evas_Object *obj, void *event_info)
{
  workerfinish_schedule(&delete_image_do, NULL, NULL);
}

static void
on_prev_image(void *data, Evas_Object *obj, void *event_info)
{
  file_step = -1;
  
  elm_slider_value_set(file_slider, wrap_files_idx(elm_slider_value_get(file_slider)-0.9));
  
  workerfinish_schedule(&step_image_do, NULL, NULL);
}

void zoom_in_do(void)
{
  Elm_Transit *trans;
  int x,y,w,h;
  int grid_w, grid_h;
  float new_scaledown;
  
  if (scale_goal > 0.25) {
    elm_scroller_region_get(scroller, &x, &y, &w, &h);
    
    evas_object_size_hint_min_get(grid, &grid_w, &grid_h);
    
    fit = 0;
    
    scale_goal /= 1.5;
    if (scale_goal < 0.25)
      scale_goal = 0.25;
    
    new_scaledown = 1.0/actual_scale_get()*scale_goal;
    
    trans = elm_transit_add();
    elm_transit_object_add(trans, grid);
    elm_transit_effect_add(trans, &_trans_grid_zoom_trans_cb, _trans_grid_zoom_contex_new(grid_w, grid_h, size.width/scale_goal,size.height/scale_goal,x+w/2,y+h/2,x/new_scaledown+w/(new_scaledown*2),y/new_scaledown+h/(new_scaledown*2),w,h), &_trans_grid_zoom_contex_del);
    elm_transit_duration_set(trans, 0.5);
    elm_transit_tween_mode_set(trans, ELM_TRANSIT_TWEEN_MODE_SINUSOIDAL);
    elm_transit_repeat_times_set(trans, 0);
    elm_transit_go(trans);
  }
}


static void
on_zoom_in(void *data, Evas_Object *obj, void *event_info)
{
  zoom_in_do();
}


void zoom_out_do(void)
{
  Elm_Transit *trans;
  int x,y,w,h;
  int grid_w, grid_h;
  float new_scaledown;
  
  elm_scroller_region_get(scroller, &x, &y, &w, &h);
  
  evas_object_size_hint_min_get(grid, &grid_w, &grid_h);
  
  fit = 0;
  
  scale_goal *= 1.5;
  
  new_scaledown = 1.0/actual_scale_get()*scale_goal;
  
  trans = elm_transit_add();
  elm_transit_object_add(trans, grid);
  elm_transit_effect_add(trans, &_trans_grid_zoom_trans_cb, _trans_grid_zoom_contex_new(grid_w, grid_h, size.width/scale_goal,size.height/scale_goal,x+w/2,y+h/2,x/new_scaledown+w/(new_scaledown*2),y/new_scaledown+h/(new_scaledown*2),w,h), &_trans_grid_zoom_contex_del);
  elm_transit_duration_set(trans, 0.5);
  elm_transit_tween_mode_set(trans, ELM_TRANSIT_TWEEN_MODE_SINUSOIDAL);
  elm_transit_repeat_times_set(trans, 0);
  elm_transit_go(trans);
}

static void
on_zoom_out(void *data, Evas_Object *obj, void *event_info)
{
  zoom_out_do();
}

static void on_insert_before(void *data, Evas_Object *obj, void *event_info)
{
  workerfinish_schedule(&insert_before_do, NULL, NULL);
}

static void on_insert_after(void *data, Evas_Object *obj, void *event_info)
{
  workerfinish_schedule(&insert_after_do, NULL, NULL);
}

static Eina_Bool
_ls_filter_cb(void *data, Eio_File *handler, const Eina_File_Direct_Info *info)
{  
  if (info->type == EINA_FILE_REG || info->type == EINA_FILE_LNK || info->type == EINA_FILE_UNKNOWN)
    return EINA_TRUE;
  
  return EINA_FALSE;
}


Tagged_File tag_file_new(File_Group *group, const char *name)
{
  Tagged_File file = {NULL, NULL, NULL};
  
  if (eina_str_has_extension(name, ".xmp")) {
    file.sidecar = name;
    //FIXME only one sidecar per group for now
    assert(!group->sidecar);
    group->sidecar = file.sidecar;
    strcpy(image_file, name);
    xmp_gettags(image_path, group);
  }
  else 
    file.filename = name;

  return file;
}

File_Group *file_group_new(const char *name)
{
  Tagged_File file_new;
  File_Group *group = calloc(sizeof(File_Group), 1);
  
  group->files = eina_inarray_new(sizeof(Tagged_File), 2);
  group->tags = eina_hash_string_superfast_new(NULL); //eina_hash_stringshared_new(NULL);
  
  file_new = tag_file_new(group, name);
  eina_inarray_push(group->files, &file_new);
  
  return group;
}

void file_group_add(File_Group *group, const char *name)
{
  Tagged_File file_new;
  //FIXME handel fitting sidecar and images
  
  file_new = tag_file_new(group, name);
  eina_inarray_push(group->files, &file_new);
}

void insert_file(const char *file)
{
  const char *last_name;
  File_Group *last_group;
  
  if (!eina_inarray_count(files)) {
    eina_inarray_push(files, file_group_new(file));
    return;
  }
  
  last_group = eina_inarray_nth(files, eina_inarray_count(files)-1);
  
  last_name = ((Tagged_File*)eina_inarray_nth(last_group->files, 0))->filename;
  if (!last_name)
    last_name = ((Tagged_File*)eina_inarray_nth(last_group->files, 0))->sidecar;
  
  if (files_similar(last_name, file)) {
    file_group_add(last_group, file);
    return;
  }
  
  eina_inarray_push(files, file_group_new(file));
}

int cmp_img_file(char *a, char *b)
{
	if (strlen(a) == 12 && strlen(b) == 12 && a[0] == 'P' && b[0] == 'P' && a[8] == '.' && b[8] == '.')
		//cmp just file number not date
		return strcmp(a+4, b+4);
	
	return strcmp(a, b);
	
}

static void
_ls_done_cb(void *data, Eio_File *handler)
{
  Eina_List *l, *l_next;
  const char *file;
  Eina_Compare_Cb cmp_func = (Eina_Compare_Cb)strcmp;
  
  files_unsorted = eina_list_sort(files_unsorted, 0, cmp_func);
  EINA_LIST_FOREACH_SAFE(files_unsorted, l, l_next, file)
    insert_file(file);

  if (!eina_inarray_count(files)) {
    printf("no files found!\n");
    workerfinish_schedule(&elm_exit_do, NULL, NULL);
    return;
  }
	
  elm_slider_min_max_set(file_slider, 0, eina_inarray_count(files)-1);
  evas_object_smart_callback_add(file_slider, "changed", &on_jump_image, NULL);
  elm_slider_value_set(file_slider, 0);
  evas_object_size_hint_weight_set(file_slider, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(file_slider, EVAS_HINT_FILL, 0);
  elm_slider_unit_format_set(file_slider, "%.0f");
  evas_object_show(file_slider);
  
  
  gridbox = elm_box_add(win);
  elm_object_content_set(scroller, gridbox);
  evas_object_size_hint_weight_set(gridbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(gridbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
  evas_object_show(gridbox);
  
  grid = elm_grid_add(win);
  clipper = evas_object_rectangle_add(evas_object_evas_get(win));
  elm_grid_pack(grid, clipper, 0, 0, size.width, size.height);
  evas_object_size_hint_min_set(grid,  200, 200);
  elm_box_recalculate(gridbox);
  elm_box_pack_start(gridbox, grid);
  evas_object_show(grid);
  
  elm_genlist_clear(tags_filter_list);
  eina_hash_foreach(known_tags, tags_hash_filter_func, NULL);
  
  
  //grid_setsize();
  
  bench_delay_start();
  
  evas_object_show(scroller);
 
  grid_setsize(); 
  step_image_do(NULL, NULL);
  forbid_fill--;
  //fill_scroller_preview();
  //fill_scroller();;
}

static void
_ls_main_cb(void *data, Eio_File *handler, const Eina_File_Direct_Info *info)
{
  const char *file = eina_stringshare_add(&info->path[info->name_start]);
  
  if (!eina_inarray_count(files) || strcmp(eina_inarray_nth(files, 0), file))
    files_unsorted = eina_list_append(files_unsorted, file);
}

static void
_ls_error_cb(void *data, Eio_File *handler, int error)
{
  fprintf(stderr, "error: [%s]\n", strerror(error));
  workerfinish_schedule(&elm_exit_do, NULL, NULL);
}

static void on_tab_select(void *data, Evas_Object *obj, void *event_info)
{
  evas_object_hide(tab_current);
  elm_box_unpack(tab_box, tab_current);
  
  tab_current = (Evas_Object*)data;
  
  elm_box_pack_end(tab_box, tab_current);
  evas_object_show(tab_current);
}

void _scroller_resize_cb(void *data, Evas *e, Evas_Object *obj, void *event_info)
{
  if (forbid_fill)
    return;
    
  grid_setsize();
  fill_scroller();
}

void shortcut(void *data, Evas *e, Evas_Object *obj, void *event_info)
//Eina_Bool shortcut(void *data, Evas_Object *obj, Evas_Object *src, Evas_Callback_Type type, void *event_info)
{
  struct _Evas_Event_Key_Down *key;
  
  //if (type ==  EVAS_CALLBACK_KEY_DOWN) {
    key = event_info;
    if (!strcmp(key->keyname, "space"))
      on_next_image(NULL, NULL, NULL);
    /*else if (!strcmp(key->keyname, "plus"))
      zoom_in_do();
    else if (!strcmp(key->keyname, "minus"))
      zoom_out_do();
    else if (!strcmp(key->keyname, "BackSpace"))
      on_prev_image(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "Delete"))
      on_delete_image(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "Escape"))
      on_done(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "o"))
      on_origscale_image(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "f"))
      on_fit_image(NULL, NULL, NULL);
    else*/ //{
      printf("keyboard! \"%s\"\n", key->keyname);
      //return EINA_TRUE;
    //}
    //return EINA_FALSE;
  //}
  
  //return EINA_TRUE;
}

Eina_Bool shortcut_elm(void *data, Evas_Object *obj, Evas_Object *src, Evas_Callback_Type type, void *event_info)
{
  struct _Evas_Event_Key_Down *key;
  
  if (type ==  EVAS_CALLBACK_KEY_DOWN) {
    key = event_info;
    if (!strcmp(key->keyname, "space"))
      on_next_image(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "plus"))
      zoom_in_do();
    else if (!strcmp(key->keyname, "minus"))
      zoom_out_do();
    else if (!strcmp(key->keyname, "BackSpace"))
      on_prev_image(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "Delete"))
      on_delete_image(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "Escape"))
      on_done(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "o"))
      on_origscale_image(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "r"))
      workerfinish_schedule(&insert_rotation_do, (void*)(intptr_t)1, NULL);
    else if (!strcmp(key->keyname, "l"))
      workerfinish_schedule(&insert_rotation_do, (void*)(intptr_t)3, NULL);
    else if (!strcmp(key->keyname, "f"))
      on_fit_image(NULL, NULL, NULL);
    else if (!strcmp(key->keyname, "a"))
      on_fullscreen(NULL, NULL, NULL);
    else {
      printf("keyboard! \"%s\"\n", key->keyname);
      return EINA_TRUE;
    }
    return EINA_FALSE;
  }
  
  return EINA_TRUE;
}

Evas_Object *elm_button_add_pack(Evas_Object *p, const char *text, void (*cb)(void *data, Evas_Object *obj, void *event_info))
{
  Evas_Object *btn = elm_button_add(p);
  evas_object_size_hint_weight_set(btn, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(btn, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_object_text_set(btn, text);
  elm_box_pack_end(p, btn);
  evas_object_show(btn);
  evas_object_smart_callback_add(btn, "clicked", cb, NULL);
  
  return btn;
}

void print_help(void)
{
  printf("usage: limeview - scale invariant image editor/viewer\n");
  printf("limeview [options] [filter1[=val1[,val2]]] [filter2] ... [inputfile/dir]\n");
  printf("   where filter may be one of:\n   \"gauss\", \"sharpen\", \"denoise\", \"contrast\", \"exposure\", \"convert\", \"assert\"\n");
  printf("   source and sink filter in the chain are set by the application\n");
  printf("options:\n");
  printf("   --help,           -h  show this help\n");
  printf("   --cache-size,     -s  set cache size in megabytes (default: 100)\n");
  printf("   --cache-metric,   -m  set cache cache metric (lru/dist/time/hits), \n                         can be repeated for a combined metric (default: lru)\n");
  printf("   --cache-strategy, -f  set cache strategy (rand/rapx/prob, default rapx)\n");
  printf("   --bench,          -b  execute benchmark (global/pan/evaluate/redo/s0/s1/s2/s3)\n                         to off-screen buffer, prints resulting stats\n");
  printf("   --verbose,        -v  prints some more information, mainly cache usage statistics\n");
}

void _on_hq_delay_set(void *data, Evas_Object *obj, void *event_info)
{
  Evas_Object *other_spinner = (Evas_Object*)data;
  
  
  high_quality_delay = elm_spinner_value_get(obj);
  
  if (max_reaction_delay < high_quality_delay)
    elm_spinner_value_set(other_spinner, high_quality_delay);
}

void _on_max_reaction_set(void *data, Evas_Object *obj, void *event_info)
{ 
  Evas_Object *other_spinner = (Evas_Object*)data;
  
  max_reaction_delay = elm_spinner_value_get(obj);
  
  if (max_reaction_delay < high_quality_delay)
    elm_spinner_value_set(other_spinner, max_reaction_delay);
}

Evas_Object *settings_box_add(Evas_Object *parent)
{
  Evas_Object *box, *frame, *spinner_hq, *spinner_mr, *inbox;
  
  box = elm_box_add(parent);
  evas_object_size_hint_weight_set(box, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(box, EVAS_HINT_FILL, EVAS_HINT_FILL);
  
  frame = elm_frame_add(parent);
  evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_box_pack_end(box, frame);
  evas_object_show(frame);
  
  inbox = elm_box_add(parent);
  evas_object_size_hint_weight_set(inbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(inbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_object_content_set(frame, inbox);
  evas_object_show(inbox);
  
  spinner_hq = elm_spinner_add(parent);
  spinner_mr = elm_spinner_add(parent);
  
  evas_object_size_hint_weight_set(spinner_hq, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(spinner_hq, EVAS_HINT_FILL, 0);
  elm_spinner_min_max_set (spinner_hq, 0, 1000);
  elm_spinner_step_set (spinner_hq, 10);
  elm_spinner_round_set(spinner_hq, 10);
  elm_spinner_value_set (spinner_hq, high_quality_delay);
  evas_object_smart_callback_add(spinner_hq, "delay,changed", _on_hq_delay_set, spinner_mr);
  elm_box_pack_end(inbox, spinner_hq);
  evas_object_show(spinner_hq);  

  evas_object_size_hint_weight_set(spinner_mr, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(spinner_mr, EVAS_HINT_FILL, 0);
  elm_spinner_min_max_set (spinner_mr, 0, 1000);
  elm_spinner_step_set (spinner_mr, 10);
  elm_spinner_round_set(spinner_mr, 10);
  elm_spinner_value_set (spinner_mr, max_reaction_delay);
  evas_object_smart_callback_add(spinner_mr, "delay,changed", _on_max_reaction_set, spinner_hq);
  elm_box_pack_end(inbox, spinner_mr);
  evas_object_show(spinner_mr);
  
  return box;
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
  char *tag = data;
  char *replace;
  XmpPtr xmp = fdata;
  
  while ((replace = strchr(tag, '|')))
    *replace = '/';
  
  xmp_append_array_item(xmp, "http://www.digikam.org/ns/1.0/", "digiKam:TagsList", XMP_PROP_ARRAY_IS_UNORDERED, tag, 0);
  
  //FIXME
  //free(tag);
  
  return 1;
}

static void new_tag_file(File_Group *group)
{
  FILE *f;
  const char *buf;
  XmpPtr xmp;
  XmpStringPtr xmp_buf = xmp_string_new();
  
  xmp = xmp_new_empty();
  xmp_register_namespace("http://ns.adobe.com/lightroom/1.0/", "lr", NULL);
  xmp_register_namespace("http://purl.org/dc/elements/1.1/", "dc", NULL);
  xmp_register_namespace("http://www.digikam.org/ns/1.0/", "digiKam", NULL);
  
  eina_hash_foreach(group->tags, xmp_add_tags_lr_func, xmp);
  eina_hash_foreach(group->tags, xmp_add_tags_dc_func, xmp);
  eina_hash_foreach(group->tags, xmp_add_tags_dk_func, xmp);
  
  if (group->tag_rating) {
    if (!xmp_prefix_namespace_uri("xmp", NULL))
      xmp_register_namespace("http://ns.adobe.com/xap/1.0/", "xmp", NULL);
    xmp_set_property_int32(xmp, "http://ns.adobe.com/xap/1.0/", "xmp:Rating", group->tag_rating, 0);
  }
  
  xmp_serialize(xmp, xmp_buf, XMP_SERIAL_OMITPACKETWRAPPER | XMP_SERIAL_USECOMPACTFORMAT, 0);
  
  f = fopen(group->sidecar, "w");
  assert(f);
  buf = xmp_string_cstr(xmp_buf);
  fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n%s\n", buf);
  fclose(f);
  
  xmp_string_free(xmp_buf);
  xmp_free(xmp);
  
  printf("wrote new tag file!\n");
}


void add_group_sidecar(File_Group *group)
{
  char *buf;
  Tagged_File *file;
  
  if (group->sidecar)
    return;
  
  EINA_INARRAY_FOREACH(group->files, file)
    if (!strcmp(file->filename+strlen(file->filename)-4, ".jpg") || !strcmp(file->filename+strlen(file->filename)-4, ".JPG")) {
      buf = malloc(strlen(file->filename)+5);
      sprintf(buf, "%s.xmp", file->filename);
      group->sidecar = buf;
      return;
    }

  file = eina_inarray_nth(group->files, 0);
  buf = malloc(strlen(file->filename+5));
  sprintf(buf, "%s.xmp", file->filename);
  group->sidecar = buf;
}

void save_sidecar(File_Group *group)
{
  int len;
  FILE *f;
  XmpStringPtr xmp_buf = xmp_string_new();
  char *buf = malloc(MAX_XMP_FILE);
  XmpPtr xmp;
  
  add_group_sidecar(group);
  
  printf("write sidecar: %s\n", group->sidecar);
  
  f = fopen(group->sidecar, "r");
  
  if (!f) {
    new_tag_file(group);
    return;
  }
  
  len = fread(buf, 1, MAX_XMP_FILE, f);
  fclose(f);
  
  xmp = xmp_new(buf, len);
  free(buf);
  
  if (!xmp) {
    printf("xmp parse failed, overwriting with new!\n");
    xmp_free(xmp);
    new_tag_file(group);
    return;
  }
  
  if (!xmp_prefix_namespace_uri("lr", NULL))
      xmp_register_namespace("http://ns.adobe.com/lightroom/1.0/", "lr", NULL);
  
  if (!xmp_prefix_namespace_uri("dc", NULL))
    xmp_register_namespace("http://purl.org/dc/elements/1.1/", "dc", NULL);
  
  if (!xmp_prefix_namespace_uri("digiKam", NULL))
    xmp_register_namespace("http://www.digikam.org/ns/1.0/", "digiKam", NULL);
  
  //delete all keyword tags 
  xmp_delete_property(xmp, "http://ns.adobe.com/lightroom/1.0/", "lr:hierarchicalSubject");
  xmp_delete_property(xmp, "http://www.digikam.org/ns/1.0/", "digiKam:TagsList");
  xmp_delete_property(xmp, "http://ns.microsoft.com/photo/1.0/", "MicrosoftPhoto:LastKeywordXMP");
  xmp_delete_property(xmp, "http://purl.org/dc/elements/1.1/", "dc:subject");
  xmp_delete_property(xmp, "http://ns.adobe.com/xap/1.0/", "xmp:Rating");

  eina_hash_foreach(group->tags, xmp_add_tags_lr_func, xmp);
  eina_hash_foreach(group->tags, xmp_add_tags_dc_func, xmp);
  eina_hash_foreach(group->tags, xmp_add_tags_dk_func, xmp);

  if (group->tag_rating) {
    if (!xmp_prefix_namespace_uri("xmp", NULL))
      xmp_register_namespace("http://ns.adobe.com/xap/1.0/", "xmp", NULL);
    xmp_set_property_int32(xmp, "http://ns.adobe.com/xap/1.0/", "xmp:Rating", group->tag_rating, 0);
  }
      
  xmp_serialize(xmp, xmp_buf, XMP_SERIAL_OMITPACKETWRAPPER | XMP_SERIAL_USECOMPACTFORMAT, 0);
  
  buf = (char*)xmp_string_cstr(xmp_buf);
  
  f = fopen(group->sidecar, "w");
  assert(f);
  buf = (char*)xmp_string_cstr(xmp_buf);
  fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n%s\n", buf);
  fclose(f);
  
  printf("write sidecar: %s %d\n", group->sidecar, (int)strlen(buf));
  
  xmp_string_free(xmp_buf);
  xmp_free(xmp);
  
  printf("freed xmp\n");
}

static void on_new_tag(void *data, Evas_Object *obj, void *event_info)
{
  const char *new = elm_entry_entry_get((Evas_Object*)data);
  
  if (eina_hash_find(known_tags, new))
    return;
  
  new = strdup(new);
  
  eina_hash_add(known_tags, new, new);
  
  //update tag list
  //FIXME only clear needed!
  elm_genlist_clear(tags_list);
  /*evas_object_del(tags_list);
  
  tags_list =  elm_genlist_add(win);
  elm_object_tree_focus_allow_set(tags_list, EINA_FALSE);
  elm_box_pack_start(tab_tags, tags_list);
  elm_genlist_select_mode_set(tags_list, ELM_OBJECT_SELECT_MODE_NONE);
  evas_object_size_hint_weight_set(tags_list, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(tags_list, EVAS_HINT_FILL, EVAS_HINT_FILL);
  evas_object_show(tags_list);*/
  
  eina_hash_foreach(known_tags, tags_hash_func, (File_Group*)eina_inarray_nth(files, file_idx));
  
  //update filter list
  elm_genlist_clear(tags_filter_list);
  eina_hash_foreach(known_tags, tags_hash_filter_func, NULL);
}

static void on_tag_changed(void *data, Evas_Object *obj, void *event_info)
{
  Tags_List_Item_Data *tag = data;
  File_Group *group = (File_Group*)eina_inarray_nth(files, file_idx);
  
  if (elm_check_state_get(obj)) {
    eina_hash_add(tag->group->tags, tag->tag, tag->tag);
  }
  else {
    assert(eina_hash_find(tag->group->tags, tag->tag));
    eina_hash_del_by_key(tag->group->tags, tag->tag);
  }
  
  save_sidecar(tag->group);
  
  if (!group_in_filters(group, tags_filter))
    step_image_do(NULL, NULL);
}

static void on_tag_filter_changed(void *data, Evas_Object *obj, void *event_info)
{
  File_Group *group = (File_Group*)eina_inarray_nth(files, file_idx);
  char *tag;
  
  tag = data;
  
  if (elm_check_state_get(obj)) {
    eina_hash_add(tags_filter, tag, tag);
  }
  else {
    assert(eina_hash_find(tags_filter, data));
    eina_hash_del_by_key(tags_filter, data);
  }
  
  if (!group_in_filters(group, tags_filter))
      step_image_do(NULL, NULL);
}

static void on_rating_changed(void *data, Evas_Object *obj, void *event_info)
{
  File_Group *group = (File_Group*)eina_inarray_nth(files, file_idx);
  
  group->tag_rating = elm_segment_control_item_index_get(elm_segment_control_item_selected_get(obj));
  
  save_sidecar(group);
}

static void on_filter_rating_changed(void *data, Evas_Object *obj, void *event_info)
{
  File_Group *group = (File_Group*)eina_inarray_nth(files, file_idx);
  
  tags_filter_rating = elm_segment_control_item_index_get(elm_segment_control_item_selected_get(obj));
  
  if (!group_in_filters(group, tags_filter))
    step_image_do(NULL, NULL);
}

static Evas_Object *_tag_gen_cont_get(void *data, Evas_Object *obj, const char *part)
{ 
  Evas_Object *check;
  Tags_List_Item_Data *tag = data;
  
  if (strcmp(part, "elm.swallow.icon"))
    return NULL;
  
  check = elm_check_add(win);
  elm_object_focus_allow_set(check, EINA_FALSE);

  elm_object_part_text_set(check, "default", tag->tag);
  if (eina_hash_find(tag->group->tags, tag->tag))
    elm_check_state_set(check, EINA_TRUE);

  evas_object_smart_callback_add(check, "changed", on_tag_changed, tag);
   
  return check;
}

static Evas_Object *_tag_filter_cont_get(void *data, Evas_Object *obj, const char *part)
{
  Evas_Object *check;
  
  if (strcmp(part, "elm.swallow.icon"))
    return NULL;
  
  check = elm_check_add(win);
  elm_object_focus_allow_set(check, EINA_FALSE);
  
  elm_object_part_text_set(check, "default", data);
  if (eina_hash_find(tags_filter, data))
    elm_check_state_set(check, EINA_TRUE);

  evas_object_smart_callback_add(check, "changed", on_tag_filter_changed, data);
  
  return check;
}

/*static void on_export_type_sel(void *data, Evas_Object *obj, void *event_info)
{
  
}*/

static Evas_Object *export_box_add(Evas_Object *parent)
{
  Evas_Object *btn;
  
  export_box = elm_box_add(parent);
  evas_object_size_hint_weight_set(export_box, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(export_box, EVAS_HINT_FILL, EVAS_HINT_FILL);
  
  //hoversel - which files
  /*sel = elm_hoversel_add(parent);
  evas_object_size_hint_weight_set(sel, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(sel, EVAS_HINT_FILL, 0);
  elm_hoversel_item_add(sel, "all files", NULL, ELM_ICON_NONE, &on_export_type_sel, NULL);
  elm_hoversel_item_add(sel, "filtered files", NULL, ELM_ICON_NONE, &on_export_type_sel, NULL);
  elm_hoversel_item_add(sel, "image", NULL, ELM_ICON_NONE, &on_export_type_sel, NULL);
  elm_box_pack_end(main_box, sel);*/
  
  //file endings to process
  export_extensions = elm_entry_add(export_box);
  evas_object_size_hint_weight_set(export_extensions, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(export_extensions, EVAS_HINT_FILL, 0);
  elm_box_pack_end(export_box, export_extensions);
  evas_object_show(export_extensions);
  
  export_path = elm_entry_add(export_box);
  evas_object_size_hint_weight_set(export_path, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(export_path, EVAS_HINT_FILL, 0);
  elm_object_text_set(export_path, "caren@technik-stinkt.de:/home/caren/fotos_upload/");
  elm_box_pack_end(export_box, export_path);
  evas_object_show(export_path);
  
  /*sel = elm_hoversel_add(parent);
  evas_object_size_hint_weight_set(sel, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(sel, EVAS_HINT_FILL, 0);
  elm_hoversel_item_add(sel, "all files", NULL, ELM_ICON_NONE, &on_export_type_sel, NULL);
  elm_box_pack_end(main_box, sel);
  evas_object_show(sel);*/
  
  //file-path where to
  /*fs = elm_fileselector_entry_add(parent);
  elm_fileselector_entry_folder_only_set(fs, EINA_TRUE);
  evas_object_size_hint_weight_set(fs, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(fs, EVAS_HINT_FILL, 0);
  elm_object_text_set(fs, "target directory");
  elm_box_pack_end(main_box, fs);
  evas_object_show(fs);*/
   
  //file-name manipulation
  
  //label/list - files to process with current settings and output names
  
  //?list of file waiting?number of files/file tree with option to abort single/all files of jobs/all jobs
  
  //execute button
  btn = elm_button_add(export_box);
  evas_object_size_hint_weight_set(btn, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(btn, EVAS_HINT_FILL, 0);
  elm_object_text_set(btn, "execute");
  evas_object_smart_callback_add(btn, "clicked", &on_exe_images_rsync, NULL);
  evas_object_show(btn);
  elm_box_pack_end(export_box, btn);
 
  
  return export_box;
}

EAPI_MAIN int
elm_main(int argc, char **argv)
{
  int help;
  int cache_strategy, cache_metric, cache_size;
  char *file;
  Evas_Object *hbox, *vbox, *frame, *hpane, *seg_filter_rating, *entry, *btn;
  Eina_List *filters = NULL;
  Eina_List *list_iter;
  Filter *last;
  Filter *f;
  Filter_Chain *fc;
  select_filter_func = NULL;
  int winsize;
  
  bench_start();
  
  labelbuf = malloc(1024);
  bench = NULL;
  scale_goal = 1.0;
  
  ecore_init();
  eio_init();
  elm_init(argc, argv);
  
  xmp_init();
  
  elm_config_scroll_thumbscroll_enabled_set(EINA_TRUE);
  files = NULL;
  file_idx = -1;
  
  max_workers = ecore_thread_max_get();
  
  lime_init();
  
  mat_cache = mat_cache_new();
  delgrid();
  
  thread_ids = calloc(sizeof(int)*(max_workers+1), 1);
  
  if (parse_cli(argc, argv, &filters, &bench, &cache_size, &cache_metric, &cache_strategy, &file, &dir, &winsize, &verbose, &help))
    return EXIT_FAILURE;
  
  if (help) {
    print_help();
    return EXIT_SUCCESS;
  }
  
  //known_tags = eina_hash_stringshared_new(NULL);
  //tags_filter = eina_hash_stringshared_new(NULL);
  known_tags = eina_hash_string_superfast_new(NULL);
  tags_filter = eina_hash_string_superfast_new(NULL);
  
  strcpy(image_path, dir);
  image_file = image_path + strlen(image_path);
  
  print_init_info(bench, cache_size, cache_metric, cache_strategy, file, dir);
  
  if (bench && bench[0].scale != -1)
    fit = bench[0].scale;
  else
    fit = 1;
  
  if (bench)
    elm_config_engine_set("buffer");
  
  win = elm_win_add(NULL, "scalefree viewer", ELM_WIN_BASIC);
  elm_win_modal_set(win, EINA_FALSE);
  evas_object_smart_callback_add(win, "delete,request", on_done, NULL);
  
  bg =  elm_bg_add(win);
  evas_object_size_hint_weight_set(bg, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  elm_win_resize_object_add(win, bg);
  evas_object_show(bg);
  
  hpane = elm_panes_add(win);
  evas_object_size_hint_weight_set(hpane, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(hpane, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_win_resize_object_add(win, hpane);
  //elm_object_tree_focus_allow_set(hpane, EINA_FALSE);
  evas_object_show(hpane);
  
  main_vbox = elm_box_add(win);
  evas_object_size_hint_weight_set(main_vbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(main_vbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_object_part_content_set(hpane, "left", main_vbox);
  evas_object_show(main_vbox);
  
  vbox_bottom = elm_box_add(win);
  evas_object_size_hint_weight_set(vbox_bottom, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(vbox_bottom, EVAS_HINT_FILL, 0);
  elm_box_pack_end(main_vbox, vbox_bottom);
  evas_object_show(vbox_bottom);

  file_slider = elm_slider_add(win);
  elm_box_pack_end(vbox_bottom, file_slider);
  evas_object_size_hint_weight_set(file_slider, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(file_slider, EVAS_HINT_FILL, 0);
  elm_slider_min_max_set(file_slider, 0, 0);
  
  hbox = elm_box_add(win);
  elm_object_tree_focus_allow_set(hbox, EINA_FALSE);
  evas_object_size_hint_weight_set(hbox, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(hbox, EVAS_HINT_FILL, 0);
  elm_box_horizontal_set(hbox, EINA_TRUE);
  elm_box_pack_end(vbox_bottom, hbox);
  evas_object_show(hbox);
  
  elm_button_add_pack(hbox, "exit", &on_done);
  elm_button_add_pack(hbox, "+", &on_zoom_in);
  elm_button_add_pack(hbox, "-", &on_zoom_out);
  elm_button_add_pack(hbox, "prev", &on_prev_image);
  elm_button_add_pack(hbox, "next", &on_next_image);
  elm_button_add_pack(hbox, "fit", &on_fit_image);
  elm_button_add_pack(hbox, "1:1", &on_origscale_image);
  elm_button_add_pack(hbox, "delete", &on_delete_image);
  
  pos_lbl_buf = malloc(1024);
  pos_label = elm_label_add(win);
  elm_box_pack_end(vbox_bottom, pos_label);
  evas_object_show(pos_label);

  lime_cache_set(cache_size, cache_strategy | cache_metric);
  
  load = lime_filter_new("load");
  filters = eina_list_prepend(filters, load);
  sink = lime_filter_new("memsink");
  lime_setting_int_set(sink, "add alpha", 1);
  filters = eina_list_append(filters, sink);
  
  scroller = elm_scroller_add(win);
  evas_object_size_hint_weight_set(scroller, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(scroller, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_box_pack_start(main_vbox, scroller);
  evas_object_smart_callback_add(scroller, "scroll", &on_scroller_move, NULL);
  evas_object_smart_callback_add(scroller, "scroll,drag,stop", &on_scroller_move, NULL);
  evas_object_event_callback_add(scroller, EVAS_CALLBACK_RESIZE, _scroller_resize_cb, NULL);
  //evas_object_event_callback_add(scroller, EVAS_CALLBACK_MOVE, _scroller_resize_cb, NULL);
  evas_object_event_callback_add(scroller, EVAS_CALLBACK_SHOW, _scroller_resize_cb, NULL);
  
  elm_object_event_callback_add(win, &shortcut_elm, NULL);
  //evas_object_key_grab(win, "space", 0, 0, EINA_TRUE);
  //evas_object_key_grab(win, "minus", 0, 0, EINA_TRUE);
  //evas_object_key_grab(win, "BackSpace", 0, 0, EINA_TRUE);
  //evas_object_key_grab(win, "Delete", 0, 0, EINA_TRUE);
  //evas_object_key_grab(win, "Escape", 0, 0, EINA_TRUE);
  //evas_object_key_grab(win, "o", 0, 0, EINA_TRUE);
  //evas_object_key_grab(win, "f", 0, 0, EINA_TRUE);
  //evas_object_event_callback_add(win, EVAS_CALLBACK_KEY_DOWN, shortcut, NULL);
  
  tab_box = elm_box_add(win);
  evas_object_size_hint_weight_set(tab_box, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(tab_box, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_object_part_content_set(hpane, "right", tab_box);
  elm_panes_content_right_size_set(hpane, 0.2);
  evas_object_show(tab_box);
  
  group_list =  elm_list_add(win);
  evas_object_size_hint_weight_set(group_list, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(group_list, EVAS_HINT_FILL, EVAS_HINT_FILL);

  tab_group = group_list;
  
  tab_tags = elm_box_add(win);
  evas_object_size_hint_weight_set(tab_tags, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(tab_tags, EVAS_HINT_FILL, EVAS_HINT_FILL);
  
  hbox = elm_box_add(win);
  evas_object_size_hint_weight_set(hbox, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(hbox, EVAS_HINT_FILL, 0);
  elm_box_pack_end(tab_tags, hbox);
  elm_box_horizontal_set(hbox, EINA_TRUE);
  evas_object_show(hbox);
  
  entry = elm_entry_add(win);
  evas_object_size_hint_weight_set(entry, EVAS_HINT_EXPAND, EVAS_HINT_FILL);
  evas_object_size_hint_align_set(entry, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_box_pack_end(hbox, entry);
  evas_object_show(entry);
  
  btn = elm_button_add(win);
  elm_box_pack_end(hbox, btn);
  elm_object_text_set(btn, "add");
  evas_object_smart_callback_add(btn, "clicked", &on_new_tag, entry);
  evas_object_show(btn);
  
  tags_list_itc = elm_genlist_item_class_new();
  tags_list_itc->item_style = "default";
  tags_list_itc->func.text_get = NULL;
  tags_list_itc->func.content_get = _tag_gen_cont_get;
  tags_list_itc->func.state_get = NULL;
  tags_list_itc->func.del = NULL;
  
  tags_filter_itc = elm_genlist_item_class_new();
  tags_filter_itc->item_style = "default";
  tags_filter_itc->func.text_get = NULL;
  tags_filter_itc->func.content_get = _tag_filter_cont_get;
  tags_filter_itc->func.state_get = NULL;
  tags_filter_itc->func.del = NULL;
  
  tags_list =  elm_genlist_add(win);
  elm_object_tree_focus_allow_set(tags_list, EINA_FALSE);
  elm_box_pack_start(tab_tags, tags_list);
  elm_genlist_select_mode_set(tags_list, ELM_OBJECT_SELECT_MODE_NONE);
  evas_object_size_hint_weight_set(tags_list, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(tags_list, EVAS_HINT_FILL, EVAS_HINT_FILL);
  evas_object_show(tags_list);
  
  seg_rating = elm_segment_control_add(win);
  elm_segment_control_item_selected_set(elm_segment_control_item_add(seg_rating, NULL, "0"), EINA_TRUE);
  elm_segment_control_item_add(seg_rating, NULL, "1");
  elm_segment_control_item_add(seg_rating, NULL, "2");
  elm_segment_control_item_add(seg_rating, NULL, "3");
  elm_segment_control_item_add(seg_rating, NULL, "4");
  elm_segment_control_item_add(seg_rating, NULL, "5");
  evas_object_size_hint_weight_set(seg_rating, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(seg_rating, EVAS_HINT_FILL, 0);
  elm_box_pack_end(tab_tags, seg_rating);
  evas_object_show(seg_rating);
  evas_object_smart_callback_add(seg_rating, "changed", on_rating_changed, NULL);
  
  tags_filter_list =  elm_genlist_add(win);
  elm_object_tree_focus_allow_set(tags_filter_list, EINA_FALSE);
  elm_box_pack_end(tab_tags, tags_filter_list);
  elm_genlist_select_mode_set(tags_filter_list, ELM_OBJECT_SELECT_MODE_NONE);
  evas_object_size_hint_weight_set(tags_filter_list, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(tags_filter_list, EVAS_HINT_FILL, EVAS_HINT_FILL);
  evas_object_show(tags_filter_list);
  
  seg_filter_rating = elm_segment_control_add(win);
  elm_segment_control_item_selected_set(elm_segment_control_item_add(seg_filter_rating, NULL, "0"), EINA_TRUE);
  elm_segment_control_item_add(seg_filter_rating, NULL, "1");
  elm_segment_control_item_add(seg_filter_rating, NULL, "2");
  elm_segment_control_item_add(seg_filter_rating, NULL, "3");
  elm_segment_control_item_add(seg_filter_rating, NULL, "4");
  elm_segment_control_item_add(seg_filter_rating, NULL, "5");
  evas_object_size_hint_weight_set(seg_filter_rating, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(seg_filter_rating, EVAS_HINT_FILL, 0);
  elm_box_pack_end(tab_tags, seg_filter_rating);
  evas_object_show(seg_filter_rating);
  evas_object_smart_callback_add(seg_filter_rating, "changed", on_filter_rating_changed, NULL);
  
  tab_filter = elm_box_add(win);
  elm_object_tree_focus_allow_set(tab_filter, EINA_FALSE);
  evas_object_size_hint_weight_set(tab_filter, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(tab_filter, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_box_pack_end(tab_box, tab_filter);
  evas_object_show(tab_filter);
  
  tab_current = tab_filter;
  
  tab_export = export_box_add(win);
  
  tab_settings = settings_box_add(win);
  
  Evas_Object *tb = elm_toolbar_add(win);
  evas_object_size_hint_weight_set(tb, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(tb, EVAS_HINT_FILL, 0);
  elm_toolbar_item_append (tb, NULL, "filter", on_tab_select, tab_filter);
  elm_toolbar_item_append (tb, NULL, "group", on_tab_select, tab_group);
  elm_toolbar_item_append (tb, NULL, "tags", on_tab_select, tab_tags);
  elm_toolbar_item_append (tb, NULL, "export", on_tab_select, tab_export);
  elm_toolbar_item_append (tb, NULL, "settings", on_tab_select, tab_settings);
  elm_box_pack_start(tab_box, tb);
  evas_object_show(tb);
  
  hbox = elm_box_add(win);
  evas_object_size_hint_weight_set(hbox, EVAS_HINT_EXPAND, 0);
  evas_object_size_hint_align_set(hbox, EVAS_HINT_FILL, 0);
  elm_box_horizontal_set(hbox, EINA_TRUE);
  elm_box_pack_end(tab_filter, hbox);
  evas_object_show(hbox);
  
  elm_button_add_pack(hbox, "insert before", &on_insert_before);
 
  select_filter = elm_hoversel_add(win);
  evas_object_size_hint_weight_set(select_filter, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(select_filter, EVAS_HINT_FILL, EVAS_HINT_FILL);
  evas_object_size_hint_min_set (select_filter, 100, 0);
  elm_box_pack_end(hbox, select_filter);
  elm_hoversel_item_add(select_filter, "contrast", NULL, ELM_ICON_NONE, &on_select_filter_select, &filter_core_contrast);
  elm_hoversel_item_add(select_filter, "exposure", NULL, ELM_ICON_NONE, &on_select_filter_select, &filter_core_exposure);
  elm_hoversel_item_add(select_filter, "gauss", NULL, ELM_ICON_NONE, &on_select_filter_select, &filter_core_gauss);
  elm_hoversel_item_add(select_filter, "sharpen", NULL, ELM_ICON_NONE, &on_select_filter_select, &filter_core_sharpen);
  elm_hoversel_item_add(select_filter, "denoise", NULL, ELM_ICON_NONE, &on_select_filter_select, &filter_core_denoise);
  elm_hoversel_item_add(select_filter, "crop", NULL, ELM_ICON_NONE, &on_select_filter_select, &filter_core_crop);
  elm_hoversel_item_add(select_filter, "rotate", NULL, ELM_ICON_NONE, &on_select_filter_select, &filter_core_simplerotate);
  elm_object_text_set(select_filter, "contrast");
  select_filter_func = filter_core_contrast.filter_new_f;
  evas_object_show(select_filter);
  
  elm_button_add_pack(hbox, "insert after", &on_insert_after);
  
  frame = elm_frame_add(win);
  elm_object_text_set(frame, "filter chain");
  evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
  elm_box_pack_end(tab_filter, frame);
  evas_object_show(frame);
  
  filter_list = elm_list_add(win);
  elm_object_content_set(frame, filter_list);
  evas_object_show(filter_list);
  
  last = NULL;
  EINA_LIST_FOREACH(filters, list_iter, f) {
    //filter chain
    fc = fc_new(f);
    filter_chain = eina_list_append(filter_chain, fc);
    
    //create gui, but not for first and last filters
    if (list_iter != filters && list_iter != eina_list_last(filters))
      fc->item = elm_list_item_prepend(filter_list, f->fc->name, NULL, NULL, &_on_filter_select, eina_list_last(filter_chain));
    
    //filter graph
    if (last)
      filter_connect(last, 0, f, 0);
    
    last = f;
  }
  elm_list_go(filter_list);
  
  files = eina_inarray_new(sizeof(File_Group), 32);
  
  if (file)
    eina_inarray_push(files, file_group_new(file));
  
  eio_file_direct_ls(dir, &_ls_filter_cb, &_ls_main_cb,&_ls_done_cb, &_ls_error_cb,NULL);
  
  /*test_filter_config(load);
   s ize = *(*Dim*)filter_core_by_type(sink, MT_IMGSIZE);
   
   printf("image size: %dx%d\n", size.width, size.height);*/
  
  
  // now we are done, show the window
  
  // run the mainloop and process events and callbacks
  //fill_area(0,0,1024,1024,MAX_FAST_SCALEDOWN,1);
  
  evas_object_size_hint_min_set(win, 128, 128);
  
  evas_object_show(win);
  elm_scroller_region_show(scroller, 0, 0, 1, 1);
  
  ecore_event_handler_add(ECORE_EXE_EVENT_DEL, &_rsync_term, NULL);
  
  if (winsize)
    evas_object_resize(win, winsize, winsize);
  else if (bench)
    evas_object_resize(win, 1024, 1024);
  else
	  elm_win_maximized_set(win, EINA_TRUE);
  
  bench_time_mark(BENCHMARK_INIT);
  elm_run();
  bench_time_mark(BENCHMARK_PROCESSING);
  if (verbose)
    cache_stats_print();
  if (bench)
    bench_report();
  lime_shutdown();
  
  xmp_terminate();
  
  eio_shutdown();
  elm_shutdown();
  ecore_shutdown();
  
  return 0;
}
ELM_MAIN()