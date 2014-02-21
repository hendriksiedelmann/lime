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

#include "filters.h"

Eina_Hash *lime_filters;

#include "filter_convert.h"
#include "filter_gauss.h"
#include "filter_contrast.h"
#include "filter_downscale.h"
#include "filter_memsink.h"
#include "filter_loadtiff.h"
#include "filter_load.h"
#include "filter_savetiff.h"
#include "filter_comparator.h"
#include "filter_sharpen.h"
#include "filter_denoise.h"
#include "filter_pretend.h"
#include "filter_crop.h"
#include "filter_simplerotate.h"
#include "filter_interleave.h"
#include "filter_savejpeg.h"

void lime_filters_init(void)
{
  //TODO dynamic loading from filters as dynamic library! on-demand? (by short-name?)
  lime_filters = eina_hash_string_small_new(&free);

  eina_hash_add(lime_filters, filter_core_gauss.shortname, &filter_core_gauss);
  eina_hash_add(lime_filters, filter_core_compare.shortname, &filter_core_compare);
  eina_hash_add(lime_filters, filter_core_down.shortname, &filter_core_down);
  eina_hash_add(lime_filters, filter_core_memsink.shortname, &filter_core_memsink);
  eina_hash_add(lime_filters, filter_core_convert.shortname, &filter_core_convert);
  eina_hash_add(lime_filters, filter_core_loadtiff.shortname, &filter_core_loadtiff);
  eina_hash_add(lime_filters, filter_core_contrast.shortname, &filter_core_contrast);
  eina_hash_add(lime_filters, filter_core_exposure.shortname, &filter_core_exposure);
  eina_hash_add(lime_filters, filter_core_load.shortname, &filter_core_load);
  eina_hash_add(lime_filters, filter_core_savetiff.shortname, &filter_core_savetiff);
  eina_hash_add(lime_filters, filter_core_sharpen.shortname, &filter_core_sharpen);
  eina_hash_add(lime_filters, filter_core_denoise.shortname, &filter_core_denoise);
  eina_hash_add(lime_filters, filter_core_pretend.shortname, &filter_core_pretend);
  eina_hash_add(lime_filters, filter_core_crop.shortname, &filter_core_crop);
  eina_hash_add(lime_filters, filter_core_simplerotate.shortname, &filter_core_simplerotate);
  eina_hash_add(lime_filters, filter_core_savejpeg.shortname, &filter_core_savejpeg);
}

Filter *lime_filter_new(const char *shortname)
{
  Filter_Core *f = eina_hash_find(lime_filters, shortname);
  
  if (!f) {
    printf("%d -%s--%s-\n",strcmp(shortname, filter_core_load.shortname), shortname, filter_core_load.shortname);
    return NULL;
  }
  
  assert(f->filter_new_f);
  
  printf("have filter %s\n", shortname);
  
  return f->filter_new_f();
}

void lime_filter_add(Filter_Core *fc)
{
  eina_hash_add(lime_filters, fc->shortname, fc);
}

Filter_Core *lime_filtercore_find(const char *name)
{
  return eina_hash_find(lime_filters, name);
}

Eina_List *lime_filter_chain_deserialize(char *str)
{
  int i;
  Meta *m;
  Filter_Core *fc;
  Filter *f, *last_f = NULL;
  Eina_List *filters = NULL;
  
  str = strdup(str);
  
  char *last = str + strlen(str);
  char *next = str;
  char *cur = str;
  char *setting;
  char *tmp;
  while (cur) {
    next = strchr(cur, ':');
    if (next)
      *next = '\0';
    f = lime_filter_new(cur);
    
    if (!f) {
      //printf("no filter for %s\n", cur);
      return NULL;
    }
    
    //FIXME
    if (next && next+1 < last)
      cur = next+1;
    else
      cur = NULL;
    
    //f = fc->filter_new_f();
    
    if (last_f)
      lime_filter_connect(last_f, f);
    
    last_f = f;
    
    filters = eina_list_append(filters, f);
    
    //settings
    if (cur) {
      next = strchr(cur, '=');
      if (strchr(cur, ',') && next > strchr(cur, ','))
        break;
      while (next) {
        *next = '\0';
        
          setting = cur;
          assert(next+1 < last);
          cur = next+1;
  
        if (!ea_count(f->settings))
          return NULL;
  
        for(i=0;i<ea_count(f->settings);i++) {
          m = ea_data(f->settings, i);
          if (!strncmp(setting, m->name, strlen(setting))) {
            setting = m->name;
            break;
          }
        }
            
        switch (lime_setting_type_get(f, setting)) {
          case MT_INT :
            lime_setting_int_set(f, setting, atoi(cur));
            break;
          case MT_FLOAT :
            lime_setting_float_set(f, setting, atof(cur));
            break;
	  case MT_STRING :
            //printf("FIXME escaping in deserialization (\',\',\':\',\' \')!!!");
            tmp = strdup(cur);
            if (strchr(tmp, ':'))
              *strchr(tmp, ':') = '\0';
            if (strchr(tmp, ','))
              *strchr(tmp, ',') = '\0';
            lime_setting_string_set(f, setting, tmp);
            free(tmp);
            break;
          default :
            printf("FIXME implement type %s settings parsing\n", mt_type_str(lime_setting_type_get(f, setting)));
        }
          
          next = strchr(cur, ':');
          if (next && next+1 < last && (!strchr(cur, ',') || next < strchr(cur, ','))) {
            cur = next+1;
            next = strchr(cur, '=');
          }
          else
            next = NULL;
        
        
      }
      
    }
      
    if (cur)
      cur = strchr(cur, ',');
    if (cur) {
      cur++;
      if (cur >= last)
        cur = NULL;
    }
  }
  
  free(str);
  
  return filters;
}