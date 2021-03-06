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

#include <sys/time.h>
#include <sys/resource.h>
#include <malloc.h>

#include "cache.h"
#include "math.h"

#include "filters.h"

struct _Cache;
typedef struct _Cache Cache;

struct _Cache {
  Eina_Hash *table;
  uint64_t generation;
  uint64_t mem, mem_peak;
  uint64_t mem_max;
  uint64_t uncached,uncached_peak;
  uint64_t buffers, buffers_peak;
  uint64_t app, app_peak;
  Tile **tiles;
  int count;
  int count_max;
  int strategy;
  Eina_Hash *stats;
};

#define CACHE_ITERS 128

static Cache *cache = NULL;

typedef struct {
  uint64_t hits;
  uint64_t misses;
  Filter_Core *fc;
  uint64_t tiles;
  uint64_t time;
  uint64_t time_count;
  uint64_t time_kib;
} Cache_Stat;

int cache_tile_cmp(const void *key1, int key1_length, const void *key2, int key2_length)
{
  if (((Tilehash*)key1)->tilehash < ((Tilehash*)key2)->tilehash)
    return -1;
  
  if (((Tilehash*)key1)->tilehash > ((Tilehash*)key2)->tilehash)
    return 1;
  
  return 0;
}

int cache_tile_tilehash(const void *key, int key_length)
{
  const Tilehash *tilehash = key;
  
  return tilehash->tilehash;
}

float tile_score_dist(Tile *tile, Tile *newtile)
{
  int minx, miny;
  Pos a = tile->area.corner;
  Pos b = newtile->area.corner;
  Pos a2 = tile->area.corner;
  Pos b2 = newtile->area.corner;
  int mult_a = 2u << a.scale;
  int mult_b = 2u << b.scale;
  
  a2.x += tile->area.width;
  a2.y += tile->area.height;
  b2.x += newtile->area.width;
  b2.y += newtile->area.height;
  
  //right corner1 smaller left corner2
  if (a2.x*mult_a < b.x*mult_b)
    minx = b.x*mult_b - a2.x*mult_a;
  //left corner1 larger right corner2
  else if (a.x*mult_a > b2.x*mult_b)
    minx = a.x*mult_a - b2.x*mult_b;
  else
    minx = 0;
  
  //upper corner1 smaller lower corner2
  if (a2.y*mult_a < b.y*mult_b)
    miny = b.y*mult_b - a2.y*mult_a;
  //lower corner1 larger upper corner2
  else if (a.y*mult_a > b2.y*mult_b)
    miny = a.y*mult_a - b2.y*mult_b;
  else
    miny = 0;
    
  if (!minx && !miny)
    return INT_MAX;
  
  return 1.0/sqrt(minx*minx+miny*miny);
}

int select_rand(Tile *newtile, Eina_Array *metrics, int *delpos, Tile **del)
{
  Tile *old;
  int pos;
  int start;
  
  pos = rand() % cache->count_max;
  start = pos;
  old = NULL;
  while (!old) {
    pos++;
    if (pos == cache->count_max)
      pos = 0;
    if (pos == start)
      return -1;
    old = cache->tiles[pos];
    if (old && tile_wanted(old)) {
      old = NULL;
    }
  }
  *del = old;
  *delpos = pos;
  
  return 0;
}

double tile_score_time(Tile *tile, Tile *newtile)
{
  return tile->time;
}

double tile_score_lru(Tile *tile, Tile *newtile)
{
  return 1.0/(newtile->generation-tile->generation);
}

double tile_score_depth(Tile *tile, Tile *newtile)
{
  //return 1.0/(tile->depth*tile->depth);
  if (tile->depth == 1)
    return 100;
  else
    return 1.0/tile->depth;
}

double tile_score_scale(Tile *tile, Tile *newtile)
{
  return tile->area.corner.scale;
}


//normalized hit-rate: hit-rate per #cached items
float tile_score_hitrate_norm(Tile *tile, Tile *newtile)
{
  Cache_Stat *stat;
  
  stat = (Cache_Stat*)eina_hash_find(cache->stats, tile->fc);
  
  if (!stat || !stat->tiles)
    return 1000000000.0;
  
  return stat->hits*(stat->hits+stat->misses)/stat->tiles;
}

int select_rand_napx(Tile *newtile, Eina_Array *metrics, int *delpos, Tile **del)
{
  Tile *old;
  int pos;
  int tries;
  double maxscore;
  double score;
  double (*score_func)(Tile *tile, Tile *newtile);
  int i;
  int start;
  
  *del = NULL;
  
  for (tries=0;tries<CACHE_ITERS;tries++) {
    pos = rand() % cache->count_max;
    start = pos;
    old = NULL;
    while (!old) {
      pos++;
      if (pos == cache->count_max)
	pos = 0;
      if (pos == start)
	return -1;
      old = cache->tiles[pos];
      if (old && tile_wanted(old)) {
	old = NULL;
      }
    }
    score = 1.0;
    for(i=0;i<ea_count(metrics);i++) {
      score_func = ea_data(metrics, i);
      score *= score_func(old, newtile);
    }
    
    if (!*del || score < maxscore) {
      maxscore = score;
      *del = old;
      *delpos = pos;
    }
  }
  
  return 0;
}

//FIXME this is currently broken!
int select_rand_prob(Tile *newtile, Eina_Array *metrics, int *delpos, Tile **del)
{
  int i;
  Tile *old;
  int pos;
  int tries;
  float scoresum;
  float randsum;
  float score;
  Tile *candidates[CACHE_ITERS];
  int cand_pos[CACHE_ITERS];
  double (*score_func)(Tile *tile, Tile *newtile);
  int start;
  
  scoresum = 0.0;
  
  for (tries=0;tries<CACHE_ITERS;tries++) {
    pos = rand() % cache->count_max;
    start = pos;
    old = NULL;
    while (!old) {
      pos++;
      if (pos == cache->count_max)
	pos = 0;
      if (pos == start)
	return -1;
      old = cache->tiles[pos];
      if (old && tile_wanted(old)) {
	old = NULL;
      }
    }
    candidates[tries] = old;
    cand_pos[tries] = pos;
    score = 1.0;
    for(i=0;i<ea_count(metrics);i++) {
      score_func = ea_data(metrics, i);
      score *= score_func(old, newtile);
    }
    scoresum += score;
  }
  randsum = scoresum*(float)rand()/(float)RAND_MAX;
  scoresum = 0.0;
  for(tries=0;tries<CACHE_ITERS;tries++) {
    scoresum += score_func(candidates[tries], newtile);
    if (scoresum >= randsum)
      break;
  }
  *del = candidates[tries];
  *delpos = cand_pos[tries];
  
  return 0;
}

void cache_stats_update(Tile *tile, int hit, int miss, int time, int count)
{
  Filter_Core *fc = tile->fc;
  Rect *area = &tile->area;
  Cache_Stat *stat;
  
  if (!cache)
    lime_cache_set(100, 0);

  stat = (Cache_Stat*)eina_hash_find(cache->stats, fc);
  
  if (stat) {
      stat->hits += hit;
      stat->misses += miss;
      if (time) {
	stat->time += time;
	stat->time_count++;
	stat->time_kib += area->width*area->height/1024;
      }
      stat->tiles += count;
  }
  else {
    stat = calloc(sizeof(Cache_Stat), 1);
    stat->fc = fc;
    stat->hits += hit;
    stat->misses += miss;
    if (time) {
      stat->time += time;
      stat->time_count++;
    }
    stat->tiles += count;
    eina_hash_direct_add(cache->stats, fc, stat);
  }
}

void cache_stats_print(void)
{
  Cache_Stat *stat;
  Eina_Iterator *iter;
  
  iter = eina_hash_iterator_data_new(cache->stats);
  
  printf("[CACHE] stats:\n");
  
  EINA_ITERATOR_FOREACH(iter, stat) {
    printf("       req to %12.12s hr: %4.1f%% (%llu/%llu) tiles: %4llu time: %4.3fms per tile, %4.3fms per MP, from %llu iters, sum: %5fms\n",
	   stat->fc->name, 100.0*stat->hits/(stat->misses+stat->hits), stat->hits, stat->misses, stat->tiles, 0.000001*stat->time/stat->time_count, .000001*stat->time/stat->time_kib*1024, stat->time_count, .000001*stat->time);
  }
}

void cache_uncached_add(int mem)
{
  cache->uncached += mem;
}

void cache_uncached_sub(int mem)
{
  cache->uncached -= mem;
}

void *cache_buffer_alloc(int mem)
{
  cache->buffers += mem;
  if (cache->buffers > cache->buffers_peak)
    cache->buffers_peak = cache->buffers;
  
  return malloc(mem);
}

//FIXME not threadsafe but used in worker threads!
void *cache_buffer_alloc_mt(int mem)
{
  void *m;
  
  lime_lock();
  m = cache_buffer_alloc(mem);
  lime_unlock();
  
  return m;
}

void cache_buffer_del(void *data, int mem)
{
  cache->buffers -= mem;
  free(data);
}


void *cache_app_alloc(int mem)
{
  cache->app += mem;
  
  if (cache->app > cache->app_peak)
    cache->app_peak = cache->app;
  
  return malloc(mem);
}

void cache_app_del(void *data, int mem)
{
  cache->app -= mem;
  free(data);
}

void cache_mem_add(int mem)
{
  cache->mem += mem;
  
  if (cache->mem > cache->mem_peak)
    cache->mem_peak = cache->mem;
}

void cache_mem_sub(int mem)
{
  cache->mem -= mem;
}

/*void cache_tile_channelmem_add(Tile *tile)
{
  int i;
  
  assert(tile->area.width*tile->area.height*ea_count(tile->channels) != 0);
    
  for(i=0;i<ea_count(tile->channels);i++) {
    assert(((Tiledata *)ea_data(tile->channels, i))->area.width == tile->area.width);
    if (((Tiledata *)ea_data(tile->channels, i))->data)
      cache_mem_add(tile->area.width*tile->area.height*((Tiledata *)ea_data(tile->channels, i))->size);
  }
}*/

/*void cache_tile_channelmem_sub(Tile *tile)
{
  int i;
  
  assert(tile->area.width*tile->area.height*ea_count(tile->channels) != 0);
    
  for(i=0;i<ea_count(tile->channels);i++) {
    assert(((Tiledata *)ea_data(tile->channels, i))->area.width == tile->area.width);
    if (((Tiledata *)ea_data(tile->channels, i))->data)
      cache_mem_sub(tile->area.width*tile->area.height*((Tiledata *)ea_data(tile->channels, i))->size);
  }
}*/

int chache_tile_cleanone(Tile *tile)
{
  Tile *del;
  int pos;
  Eina_Array *metrics = eina_array_new(4);
  int (*select_func)(Tile *newtile, Eina_Array *metrics, int *delpos, Tile **del);

  if ((cache->strategy & CACHE_MASK_F) == CACHE_F_RAND)
    select_func = &select_rand;
  else if ((cache->strategy & CACHE_MASK_F) == CACHE_F_PROB)
    select_func = &select_rand_prob;
  else
    select_func = &select_rand_napx;
  
  if (cache->strategy & CACHE_MASK_M & CACHE_M_DIST)
    ea_push(metrics, &tile_score_dist);
  if (cache->strategy & CACHE_MASK_M  & CACHE_M_TIME)
    ea_push(metrics, &tile_score_time);
  if (cache->strategy & CACHE_MASK_M  & CACHE_M_HITN)
    ea_push(metrics, &tile_score_hitrate_norm);
  if (cache->strategy & CACHE_MASK_M  & CACHE_M_LRU)
    ea_push(metrics, &tile_score_lru);
  if (cache->strategy & CACHE_MASK_M  & CACHE_M_DEEP)
    ea_push(metrics, &tile_score_depth);
  if (cache->strategy & CACHE_MASK_M  & CACHE_M_SCALE)
    ea_push(metrics, &tile_score_scale);
    
  if (select_func(tile, metrics, &pos, &del)) {
    printf("DEBUG: could not find a tile to clean!\n");
    return -1;
  }
  
  assert (del->channels);
  
  cache->count--;
  eina_hash_del(cache->table, &del->hash, del);
  assert(del->fc);
  cache_stats_update(del, 0, 0, 0, -1);
  tile_del(del);
  cache->tiles[pos] = NULL;
  eina_array_free(metrics);
  
  return 0;
}

int get_my_pss(void)
{
  size_t size = 0;
  char buf[1024];
  
  FILE *f = fopen("/proc/self/smaps", "r");
  
  if (!f)
    return 0;
  
  while (fgets(buf, 1024, f )) {
    if (!strncmp(buf, "Pss:", 4)) {
      size += atoi(buf+4);
    }
  }
  
  fclose(f);
  
  return size/1024;
}

uint64_t check_cache_size(void)
{
  int i, j;
  uint64_t size = 0;
  
  for(i=0;i<cache->count_max;i++) {
    Tile *t = cache->tiles[i];
    if (!t)
      continue;
    if (t->channels) {
      for(j=0;j<ea_count(t->channels);j++) {
        if (((Tiledata *)ea_data(t->channels, j))->data) {
          size += t->area.width*t->area.height*((Tiledata *)ea_data(t->channels, j))->size;
          if (malloc_usable_size(((Tiledata *)ea_data(t->channels, j))->data) - t->area.width*t->area.height*((Tiledata *)ea_data(t->channels, j))->size > 4096)
          {
            printf("%d > %d\n", malloc_usable_size(((Tiledata *)ea_data(t->channels, j))->data),t->area.width*t->area.height*((Tiledata *)ea_data(t->channels, j))->size);
            abort();
          }
        }
      }
    }
  }
  return size;
}

void cache_tile_add(Tile *tile)
{
  int i;
  int pos;
  int size;
  
  if (!cache)
    lime_cache_set(100, 0);
  
  tile->generation = cache->generation++;
  tile->cached = 1;
  
  assert(tile->fc);
  cache_stats_update(tile, 0, 0, 0, 1);
  
  eina_hash_direct_add(cache->table, &tile->hash, tile);
  if (tile->channels) {
    for(i=0;i<ea_count(tile->channels);i++) {
      if (((Tiledata *)ea_data(tile->channels, i))->data) {
	cache_uncached_sub(tile->area.width*tile->area.height*((Tiledata *)ea_data(tile->channels, i))->size);
	cache_mem_add(tile->area.width*tile->area.height*((Tiledata *)ea_data(tile->channels, i))->size);
      }
    }
  }
  cache->count++;
  
  //size = get_my_pss();
  //printf("memory usage: %dMB cache: %.1f(%.1f)MB rendering: %.1f(%.1f)MB buffers: %.1f(%.1f)MB app(img): %.1f(%.1f)MB rest: %.1f\n", size, cache->mem/1048576.0,cache->mem_peak/1048576.0, cache->uncached/1048576.0,0.0, cache->buffers/1048576.0,cache->buffers_peak/1048576.0, cache->app/1048576.0,cache->app_peak/1048576.0, (size*1048576.0-cache->mem-cache->uncached-cache->buffers-cache->app)/1048576.0);
  //printf("checking cache size: %f\n", check_cache_size()/(1024.0*1024.0));
  //malloc_stats();
  
  //need to delete some tile
  while (cache->mem >= cache->mem_max || cache->count >= cache->count_max/2)
    if (chache_tile_cleanone(tile)) {
      printf("unable to cope with cache size. ignoring!\n");
      break;
    }
    
  //find free pos in tiles array
  pos = rand() % cache->count_max;
  while (cache->tiles[pos]) {
    pos++;
    if (pos == cache->count_max)
      pos = 0;
  }
  cache->tiles[pos] = tile;
  
  //printf("cache usage: %.3fMB %d/%d entries\n", (double)cache->mem/1024/1024,cache->count,cache->count_max);
}

Tile *cache_tile_get(Tilehash *hash)
{
  if (!cache)
    return NULL;
  
  Tile *tile = eina_hash_find(cache->table, hash);
  
  if (tile)
    tile->generation = cache->generation++;
  
  return tile;
}

void lime_cache_flush(void)
{
  int i;
  
  for(i=0;i<cache->count_max;i++) {
    Tile *t = cache->tiles[i];
    if (!t)
      continue;
    //WARNING this will free required memory!!!
    //t->want = 0;
    //t->refs = NULL;
    //WARNING end
    tile_del(t);
    cache->tiles[i] = NULL;
  }
}

int lime_cache_set(int mem_max, int strategy)
{
  
  if (!(strategy & CACHE_MASK_M)) {
    strategy |= CACHE_M_LRU;
    strategy |= CACHE_M_DEEP;
    strategy |= CACHE_M_SCALE;
    //strategy |= CACHE_M_TIME;
  }
  
  if (cache) {
    if (mem_max > cache->mem_max) {
      if (32*mem_max > cache->count_max)
        cache->count_max = 32*mem_max;
      cache->tiles = realloc(cache->tiles, sizeof(Tile*)*cache->count_max);
      memset(cache->tiles+cache->mem_max*sizeof(Tile*), 0, mem_max - cache->mem_max);
      cache->strategy = strategy;
      cache->mem_max = mem_max*1024*1024;
      return 0;
    }
    else {
      cache->mem_max = mem_max*1024*1024;
      cache->strategy = strategy;
      printf("FIXME: cache size will not be shrinking immediately!\n");
      return -1;
    }
  }
  
  cache = calloc(sizeof(Cache), 1);
  
  //this allows us to use mmap for all tiles - so memory gets freed immediately - but might be a good idea to keep a few tiles in a free list for reuse (maybe ~1MB = 16 default sized tiles?)
  mallopt(M_MMAP_THRESHOLD, DEFAULT_TILE_SIZE*DEFAULT_TILE_SIZE);
  
  cache->table = eina_hash_new(NULL, &cache_tile_cmp, &cache_tile_tilehash, NULL, 8);
  cache->count_max = 32*mem_max;
  cache->tiles = calloc(sizeof(Tile*)*cache->count_max, 1);
  cache->mem_max = mem_max*1024*1024;
  cache->strategy = strategy;
  cache->stats = eina_hash_pointer_new(&free);  
  
  return 0;
}