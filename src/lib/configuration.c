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

#include "configuration.h"

#include "filter_convert.h"
#include "filter_loadjpeg.h"
#include "filter_loadtiff.h"
#include "filter_interleave.h"
#include "filter_fliprot.h"

#define DEBUG_OUT_GRAPH 

#define MAX_CONS_TRIES 4

//TODO replace by non-global filter-located variable! this here is bad for multiple filter graphs!
struct _Config {
   int configured;
   int refcount;
   Eina_Array *applied_metas;
   Eina_Array *new_fs;
   Eina_Inarray *succ_inserts;
   Eina_Array *config_meta_allocs;
   Eina_Array *config_allocs;
};

typedef struct {
  Meta *tune;
  Eina_Array *allowed_values;
  //if a value is removed this has to be propagated back!
  Eina_Array *my_node;
  Eina_Array *other_node;
  Eina_Array *other_restr;
  int remain;
} Tune_Restriction;

typedef struct {
  int len;
  int filters[MAX_CONS_TRIES];
} Config_Chain;

void lime_filter_config_ref(Filter *f)
{
  Config * c = filter_chain_last_filter(f)->c;
  
  assert(c);
    
  c->refcount++;
}

void lime_filter_config_unref(Filter *f)
{
  Config * c = filter_chain_last_filter(f)->c;
  
  assert(c);
    
  assert(c->refcount);
  
  c->refcount--;
}

void meta_dep_set_data_calc(Meta *m, void *dep_data)
{
  m->dep->data = dep_data;
  meta_data_calc(m);
}

Meta *meta_copy(Meta *m, Config *c)
{
  int i;
  Meta *copy = calloc(sizeof(Meta), 1);
  
  eina_array_push(c->config_meta_allocs, copy);
  
  //printf("copying %p\n", m);
  
  copy->type = m->type;
  copy->name = m->name;
  copy->filter = m->filter;
  //TODO tunings merging etc
  // Meta *dep; //type: Meta, tuning this meta depends on
  //Meta *replace; //the node that appears in the output instead of this input-node
  //Meta_Array *childs; //type: Meta
  //TODO select merging etc
  if (m->select && ea_count(m->select)) {
    copy->select = eina_array_new(ea_count(m->select));
    for(i=0;i<ea_count(m->select);i++)
      ea_push(copy->select, ea_data(m->select, i));
  }
  copy->data = m->data;
  //copy->data_calc = m->data_calc;
  // Meta_Array *parents;
  //TODO new filter
  //Filter *filter;
  copy->meta_data_calc_cb = m->meta_data_calc_cb;
  //tunes can refer to themselves, loop is detected like this
  if (m->dep) {
    copy->dep = m->dep;
  }
  
  return copy;
}

Meta *meta_copy_tree(Meta *m, Eina_Array *copied_ar, Eina_Array *copy_ar, Config *c)
{
  int i, i_cp, is_copy;
  Meta *copy = meta_copy(m, c);
  
  ea_push(copied_ar, m);
  ea_push(copy_ar, copy);
  
  
  if (m->childs) {
    copy->childs = meta_array_new();
    for(i=0;i<m->childs->count;i++) {
      is_copy = 0;
      for(i_cp=0;i_cp<ea_count(copied_ar);i_cp++)
	if (m->childs->data[i] == ea_data(copied_ar, i_cp)) {
	  is_copy = 1;
	  meta_array_append(copy->childs, ea_data(copy_ar, i_cp));

	}
	
	if (!is_copy)
	  meta_array_append(copy->childs, meta_copy_tree(m->childs->data[i], copied_ar, copy_ar, c));

    }
  }
  
  return copy;
}
 
 
void metas_pair_recursive(Eina_Array *matches, Meta *source, Meta *sink) 
{
  int i;
  
  if (source->type == sink->type) {
    eina_array_push(matches, source);
    return;
  }
  
  if (source->childs)
    for(i=0;i<source->childs->count;i++)
      metas_pair_recursive(matches, source->childs->data[i], sink);
}
 
 
int metas_not_kompat(Meta *source, Meta *sink)
{
  int i, j;
  
  //printf("cmp sink:"); meta_print(sink); printf(" src:"); meta_print(source); printf("\n");
  
  if (source->type != sink->type)
    return -1;
  
  //printf("cmp types\n");
  
  //single data
  if (sink->data && source->data) {
    return meta_def_list[sink->type].cmp_data(source->data,sink->data);
  }
  //sink single, check source
  else if (sink->data) {
    if (!source->select)
      return -1;
  
    for(i=0;i<ea_count(source->select);i++)
      if (meta_def_list[sink->type].cmp_data(ea_data(source->select,i),sink->data) == 0)
	return 0;
    return -1;
  }
  else if (sink->select) {
    if (source->data) {
      for(i=0;i<ea_count(sink->select);i++)
	if (meta_def_list[sink->type].cmp_data(ea_data(sink->select,i),source->data) == 0)
	  return 0;
      return -1;
    }
    else if (source->select) {
      //printf("both seleects\n");
      for(i=0;i<ea_count(source->select);i++)
	for(j=0;j<ea_count(sink->select);j++)
	  if (meta_def_list[sink->type].cmp_data(ea_data(source->select,i),ea_data(sink->select,j)) == 0)
	    return 0;
      return -1;
    }
    else
      return -1;
  }
  
  return 0;
}
 
//vergleiche die zwei Bäume auf kompatibilität (expandiert nur sink!)
int metas_not_kompat_rec_all(Meta *source, Meta *sink, Eina_Array *match_source, Eina_Array *match_sink)
{
  int i, j;
  int cmp;
  int found;
  int pre_count = ea_count(match_source);
  
  cmp = metas_not_kompat(source, sink);
  
  if (cmp) {
    //printf("type no match!\n");
    return -1;
  }

  
  if (sink->childs) 
    for(i=0;i<ma_count(sink->childs);i++) {
      //printf("checking child %d\n", i);
      found = 0;
      for(j=0;j<ma_count(source->childs);j++)
	if (metas_not_kompat_rec_all(ma_data(source->childs,j),ma_data(sink->childs,i), match_source, match_sink)==0) {
	  found = 1;
	  break;
	}
      if (!found) {
	//printf("rec no  match\n");
	while (ea_count(match_source) > pre_count) {
	  ea_pop(match_source);
	  ea_pop(match_sink);
	}
	return -1;
      }
    }
    
  /*if (sink->type == MT_CHANNEL)
    ch_sink_source[(intptr_t)sink->data] = source;*/
  ea_push(match_source, source);
  ea_push(match_sink, sink);
    

  //printf("match!\n");
  return 0;
}
 
Eina_Array *get_sink_source_matches(Meta *source, Meta *sink, Eina_Array *match_source, Eina_Array *match_sink)
{
  int i;
  
  Eina_Array *matches_possible = eina_array_new(8);
  Eina_Array *matches_compat = eina_array_new(8);
  
  //recursive-discover matches
  metas_pair_recursive(matches_possible, source, sink);
  
  if (!ea_count(matches_possible)) {
    eina_array_free(matches_possible);
    eina_array_free(matches_compat);
    return NULL;
  }
  
  //vergleiche matches, und schmeiss inkompatible weg
  for(i=0;i<ea_count(matches_possible);i++)
    if (metas_not_kompat_rec_all(ea_data(matches_possible,i), sink, match_source, match_sink) == 0)
      eina_array_push(matches_compat, ea_data(matches_possible,i));
    
  if (!ea_count(matches_compat)) {
    eina_array_free(matches_possible);
    eina_array_free(matches_compat);
    return NULL;
  }
  
 /*for(i=0;i<ea_count(matches_compat);i++) {
    printf("compatible match: ");
    meta_print(ea_data(matches_compat, i));
    printf("-> ");
    meta_print(sink);
    printf("\n");
  }*/
  
  eina_array_free(matches_possible);
  
  return matches_compat;
}


FILE *vizp_start(char *path)
{
  FILE *file = fopen(path, "w");
  
  fprintf(file, "digraph g {\n"
  "overlap = false;\n"
  "node [shape = record];\n");
  
  return file;
}

void vizp_stop(FILE *f)
{
  fprintf(f, "}\n");
  fclose(f);
}

/*void vizp_fg(Filtergraph *g, char *path)
{
  int i;
  int out;
  FILE *file = fopen(path, "w");
  Fg_Node *node;
  Con *con;
  
  fprintf(file, "digraph g {\n"
  "overlap = false;\n"
  "node [shape = record];\n");
  
  for(i=0;i<ea_count(g->nodes);i++) {
    node = eina_array_data_get(g->nodes, i);
    vizp_filter(file, node->f);
    if (node->con_trees_out)
      for(out=0;out<ea_count(node->con_trees_out);out++) {
	con = eina_array_data_get(node->con_trees_out, out);
	fprintf(file, "\"%p\" -> \"%p\" [dir=none]\n", con->source, con->sink);
      }
    }
  
  fprintf(file, "}\n");
  fclose(file);
    
}*/



//vergleiche die zwei Bäume auf kompatibilität (expandiert nur sink!)
int metas_not_kompat_rec(Meta *source, Meta *sink, Meta **ch_sink_source)
{
  int i, j;
  int cmp;
  int found;
  
  cmp = metas_not_kompat(source, sink);
  
  if (cmp) {
    //printf("type no match!\n");
    return -1;
  }

  
  if (sink->childs) 
    for(i=0;i<ma_count(sink->childs);i++) {
      //printf("checking child %d\n", i);
      found = 0;
      for(j=0;j<ma_count(source->childs);j++)
	if (metas_not_kompat_rec(ma_data(source->childs,j),ma_data(sink->childs,i), ch_sink_source)==0) {
	  found = 1;
	  break;
	}
      if (!found) {
	//printf("rec no  match\n");
	return -1;
      }
    }
    
  if (sink->type == MT_CHANNEL)
    ch_sink_source[(intptr_t)sink->data] = source;
    

  //printf("match!\n");
  return 0;
}



void restr_remove_value(Tune_Restriction *restr, int idx)
{
  int i, j;
  Meta *my_node, *other_node;
  Tune_Restriction *other_restr;
  void *my_data;

  restr->remain--;

  if (restr->remain == 0) {
    printf("FIXME: tuning impossible!\n");
  }
  
  my_data = eina_array_data_get(restr->allowed_values, idx);
  restr->tune->data = my_data;
  eina_array_data_set(restr->allowed_values, idx, NULL);
  
  //für alle my_nodes
  //calculate my_node->data_calc
  //search for corresponding tuning of other_node->dep
  //call restr_remove_value for corresponding other_node
  
  for(i=0;i<ea_count(restr->my_node);i++) {
    my_node = ea_data(restr->my_node, i);
    other_node = ea_data(restr->other_node, i);
    other_restr = ea_data(restr->other_restr, i);
    
    restr->tune->data = my_data;
    meta_data_calc(my_node);
    
    assert(other_restr->tune == other_node->dep);
    
    for(j=0;j<ea_count(other_restr->allowed_values);j++) 
      if (ea_data(other_restr->allowed_values, j)) {
	other_restr->tune->data = ea_data(other_restr->allowed_values, j);
	meta_data_calc(other_node);
	
	assert(my_node->data);
	assert(other_node->data);
	
	if (!metas_not_kompat(my_node, other_node)) {
	  assert(other_restr->remain > 1);
	  
	  restr_remove_value(other_restr, j);
	  
	  break;
	}
      }
    my_node->data = NULL;
    other_node->data = NULL;
    //other_restr->tune->data = NULL;
  }
  
  if (restr->remain == 1) {
    for(i=0;i<ea_count(restr->allowed_values);i++)
      if (ea_data(restr->allowed_values, i)) {
	restr->tune->data = ea_data(restr->allowed_values, i);
      }
  }
  else
    restr->tune->data = NULL;
}

Tune_Restriction *restr_new(Meta *from, Config *c)
{
  int i;
  Tune_Restriction *restr = calloc(sizeof(Tune_Restriction), 1);
  
  eina_array_push(c->config_allocs, restr);
  
  restr->tune = from;
  
  assert(from->select);
  assert(from->data == NULL);
  assert(ea_count(from->select));
  
  restr->allowed_values = eina_array_new(ea_count(from->select));
  for(i=0;i<ea_count(from->select);i++)
    ea_push(restr->allowed_values, ea_data(from->select, i));
  
  restr->my_node = eina_array_new(8);
  restr->other_node = eina_array_new(8);
  restr->other_restr = eina_array_new(8);
  restr->remain = ea_count(from->select);
  
  return restr;
}

void tunes_restrict(Meta *a, Meta *b, Eina_Array *restrictions, Config *c)
{
  int i, j;
  int is_compatible;
  Tune_Restriction *restr;
  Tune_Restriction *found_a = NULL, *found_b = NULL;
  
  if (!a->dep && !b->dep)
    return;
  
  assert(a->dep != b->dep);
  
  for(i=0;i<ea_count(restrictions);i++) {
    restr = ea_data(restrictions, i);
    
    if (restr->tune == a->dep)
      found_a = restr;
    if (restr->tune == b->dep)
      found_b = restr;
  }
  
  //create no yet existing tune-restrictions
  if (a->dep && !found_a) {
    found_a = restr_new(a->dep,c );
    ea_push(restrictions, found_a);
  }
  
  if (b->dep && !found_b) {
    found_b = restr_new(b->dep, c);
    ea_push(restrictions, found_b);
  }
  
  if (!b->dep) {
    //restrict a's dep to b's only allowed value

    for(i=0;i<ea_count(found_a->allowed_values);i++) 
      if (ea_data(found_a->allowed_values, i)) {
	meta_dep_set_data_calc(a, ea_data(found_a->allowed_values, i));
    
	if (metas_not_kompat(a, b))
	  restr_remove_value(found_a, i);
    }
    
    a->data = NULL;
    //a->dep->data = NULL;
    
    return;
  }
  
  if (!a->dep) {
    //restrict b's dep to a's only allowed value

    for(i=0;i<ea_count(found_b->allowed_values);i++) 
      if (ea_data(found_b->allowed_values, i)) {
	meta_dep_set_data_calc(b, ea_data(found_b->allowed_values, i));
    
	if (metas_not_kompat(a, b))
	  restr_remove_value(found_b, i);
    }
    
    b->data = NULL;
    //b->dep->data = NULL;
    
    return;
  }
  
  ea_push(found_a->my_node, a);
  ea_push(found_a->other_node, b);
  ea_push(found_a->other_restr, found_b);
  
  ea_push(found_b->my_node, b);
  ea_push(found_b->other_node, a);
  ea_push(found_b->other_restr, found_a);
    
  //FIXME those two as function!
  for(i=0;i<ea_count(found_a->allowed_values);i++) {
    if (!ea_data(found_a->allowed_values, i))
      continue;
    
    is_compatible = 0;
    meta_dep_set_data_calc(a, ea_data(found_a->allowed_values, i));
    
    for(j=0;j<ea_count(found_b->allowed_values);j++) {
      if (!ea_data(found_b->allowed_values, j))
	continue;
      
      meta_dep_set_data_calc(b, ea_data(found_b->allowed_values, j));
      if (!metas_not_kompat(a, b)) {
	is_compatible = 1;
	break;
      }
    }
    
    if (!is_compatible)
      restr_remove_value(found_a, i);
  }
  
  a->data = NULL;
  a->dep->data = NULL;
  
  for(i=0;i<ea_count(found_b->allowed_values);i++) {
    if (!ea_data(found_b->allowed_values, i))
      continue;
    
    is_compatible = 0;
    meta_dep_set_data_calc(b, ea_data(found_b->allowed_values, i));
    
    for(j=0;j<ea_count(found_a->allowed_values);j++) {
      if (!ea_data(found_a->allowed_values, j))
	continue;
      
      meta_dep_set_data_calc(a, ea_data(found_a->allowed_values, j));
      if (!metas_not_kompat(a, b)) {
	is_compatible = 1;
	break;
      }
    }
    
    if (!is_compatible)
      restr_remove_value(found_b, i);
  }
  
  b->data = NULL;
  b->dep->data = NULL;
}

/*
Eingabe: source, sink sind verbunden, Liste der weiteren Verbindungen.
Neuer out-Baum wird mit out_tree_contruct rekursiv konstruiert:

wie finden wir kopierte knoten: Mit copied, copy!
replace-knoten werden immer rekursiv kopiert, source-knoten einzeln!
Annahme: kompatible teilbäume sind immer(!) verbunden
*/
Meta *out_tree_construct(Meta *source, Eina_Array *src_con, Eina_Array *sink_con, Eina_Array *copied, Eina_Array *copy, Config *c)
{
  //betrachte Kinder von source
    //gibt es entsprechende verbindung zu sink?
      //nein: Kind-Knoten an out anhängen
      //ja: ist replace schon als Kind von out vorhanden?
	//nein: replace-Baum als Kind anhängen
      //dann: out_tree_contruct(kind, sink-entsprechung, replace-entsprchung, out-entspr,...)
      
  int i;
  Meta *replace;
  Meta *sink;
  Meta *out;
  
  /*if (!source->childs)
    return;*/
  
  //out->childs = meta_array_new();
    
  //for(i=0;i<source->childs->count;i++) {
    replace = NULL;
    out = NULL;
    sink = NULL;
    
    //durchsuche src_con nach source, setzte replace auf entsprechendes replace wenn gefunden
    for(i=0;i<ea_count(src_con);i++)
      if (ea_data(src_con, i) == source) {
	sink = ea_data(sink_con, i);
	replace = sink->replace;
	
	//assert(replace);
  if (!replace) {
    printf("DEBUG: CONFIG: no replace for meta\n");
    meta_print(sink);
    printf("\n");
  }
      }
    
    if (!replace) {
      out = meta_copy(source, c);
    }
    else {
      assert(sink);
      
      //untersuche ob replace schon in out vorhanden ist
      for(i=0;i<ea_count(copied);i++)
	if (replace == ea_data(copied, i)) {
	  out = ea_data(copy, i);
	  //printf("tunes merge: %p %p %p\n", source, sink, out);
	  //tunes_merge(source, sink, out, tunes_copied, tunes_copy);
	  //return out;
	}
	  //out = ea_data(copy, i);
	
      //müssen out_child erst mal kopieren
      if (!out)
	out = meta_copy_tree(replace, copied, copy, c);
      //printf("tunes merge (tree): %p %p %p\n", source, sink, out);
      //tunes_merge(source, sink, out, tunes_copied, tunes_copy);
    }
    
    //meta_array_append(out->childs, out_child);
    
    //assert(sink);
    
    if (source->childs) {
      if (out->childs)
	meta_array_del(out->childs);
      out->childs = meta_array_new();
      for(i=0;i<ma_count(source->childs);i++) {
	Meta *child = out_tree_construct(source->childs->data[i], src_con, sink_con, copied, copy, c);
	if (child)
	  meta_array_append(out->childs, child);
	
      }

    }
    
    
  //}
    
  
  //assert(sink);
  
  /*if (sink) {
     printf("tunes merge (after): %p %p %p\n", source, sink, out);
    //tunes_merge(source, sink, out, tunes_copied, tunes_copy);
  }*/
  
  return out;
}

/*
 * 
brauchen Zuordnung meta-tuning (über ref!). Beim Out-Baum entspricht dabei die Auswahl dem array mit den jeweiligen tuning-Einträgen der bisherigen Filter.
*/
/*int chain_configure(Filter *f)
{
  curr = erste filter
  outtree = erster filter out-tree
  
  while(outtree) {
    vergleiche outtree und intree von next(nurr)
    bekommen liste von src-tuning -> src-meta -> sink->meta sink->tuning
    tunings können auch jeweils NULL sein
    wir können mehrere Verbindungen bekommen, aber verbinden jeweils die gleichen tunings
    
    jetzt probieren wir beide tunings durch, neues out-tuning sind die src-tunings die ein kompatibles sink-tuning haben, jeweils mit dem entsprechenden sink-tuning fürs select angehangen.
    
    neue out-tree wird konstruiert, kombination aus alter out-tree, durch in-tree konsumierte einträge, und jeweils ersetzte neue out-tree.
  }
}*/

//TODO use filter callback, don't actually change any metas!
void apply_sink_souce_matches(Eina_Array *match_source, Eina_Array *match_sink, Eina_Array *applied_metas)
{
  int i;
  Meta *source, *sink;
  
  for(i=0;i<ea_count(match_source);i++) {
    source = ea_data(match_source, i);
    sink = ea_data(match_sink, i);
    
    if (!sink->data && !sink->select) {
      sink->data = source->data;
      ea_push(applied_metas, sink);
    }
  }
}

void _ea_metas_data_zero(Eina_Array *metas)
{
  if (!metas)
    return;
  
  while(ea_count(metas))
    ((Meta*)ea_pop(metas))->data = NULL;
}

int test_filter_config_real(Filter *f, int write_graph, Config *c)
{
  int pos = 0;
  int i;
  FILE *file;
  Con *con;
  Meta *out;
  Meta *old_out;
  FILE *filters;
  Eina_Array *match_source = eina_array_new(8);
  Eina_Array *match_sink = eina_array_new(8);
  Eina_Array *copied = eina_array_new(8);
  Eina_Array *copy = eina_array_new(8);
  Eina_Array *matches_compat;
  Eina_Array *restrictions = eina_array_new(8);
  Eina_Array *trash = eina_array_new(32);
  
  assert(f->node->con_trees_out != NULL);
  assert(ea_count(f->node->con_trees_out) == 1);
  
  if (f->input_fixed)
    if (f->input_fixed(f)) {
      eina_array_free(match_source);
      eina_array_free(match_sink);
      for(i=0;i<ea_count(copied);i++)
	free(ea_data(copied, i));
      eina_array_free(copied);
      eina_array_free(copy);
      eina_array_free(restrictions);
      //for(i=0;i<ea_count(trash);i++)
	//free(ea_data(trash, i));
      eina_array_free(trash);
      return 0;
    }
  
  con = ea_data(f->node->con_trees_out, 0);
    
  if (write_graph) {
    file = vizp_start("cons.dot");
    vizp_filter(file, f);
  
    filters = vizp_start("filters.dot");
  vizp_filter(filters, con->source->filter);
    
  }
  
  out = ea_data(con->source->filter->out, 0);
  
  while (con) {
    if (write_graph)
      vizp_filter(filters, con->sink->filter);
    
    eina_array_free(match_source);
    match_source = eina_array_new(8);
    eina_array_free(match_sink);
    match_sink = eina_array_new(8);
    while(ea_count(copied))
      eina_array_push(trash, eina_array_pop(copied));
    eina_array_free(copied);
    copied = eina_array_new(8);
    eina_array_free(copy);
    copy = eina_array_new(8);
    
    matches_compat = get_sink_source_matches(out, con->sink, match_source, match_sink);
    
    if (!matches_compat) {
      //printf("failed\n");
      _ea_metas_data_zero(c->applied_metas);
      eina_array_free(match_source);
      eina_array_free(match_sink);
      for(i=0;i<ea_count(copied);i++)
	free(ea_data(copied, i));
      eina_array_free(copied);
      eina_array_free(copy);
      eina_array_free(restrictions);
      //for(i=0;i<ea_count(trash);i++)
	//free(ea_data(trash, i));
      eina_array_free(trash);
      return pos;
    }
    
    //sonst mehrfachandwendung des filters je kanal!
    assert(ea_count(matches_compat) == 1);
  
    assert(ea_count(con->source->filter->out)==1);
    assert(ea_count(con->sink->filter->in)==1);
    
    //copy source meta->data to empty input metas
    apply_sink_souce_matches(match_source, match_sink, c->applied_metas);
    if (con->sink->filter->input_fixed)
      if (con->sink->filter->input_fixed(con->sink->filter)) {
	_ea_metas_data_zero(c->applied_metas);
	eina_array_free(matches_compat);
	eina_array_free(match_source);
	eina_array_free(match_sink);
	for(i=0;i<ea_count(copied);i++)
	  free(ea_data(copied, i));
	eina_array_free(copied);
	eina_array_free(copy);
	eina_array_free(restrictions);
	//for(i=0;i<ea_count(trash);i++)
	  //free(ea_data(trash, i));
	eina_array_free(trash);
	return pos;
      }
   
    old_out = out;
    if (con->sink->filter->node->con_trees_out && ea_count(con->sink->filter->node->con_trees_out))
      out = out_tree_construct(out, match_source, match_sink, copied, copy, c);
    else
      out = NULL;
    
    for(i=0;i<ea_count(match_source);i++)
      tunes_restrict(ea_data(match_source, i), ea_data(match_sink, i), restrictions, c);
    
    //FIXME clean on reconfiguration!
    con->sink->filter->node->con_ch_in = eina_array_new(4);
    
    for(i=0;i<ea_count(match_source);i++)
      if (((Meta*)ea_data(match_source, i))->type == MT_CHANNEL) {
	assert(ea_count(con->sink->filter->node->con_ch_in) == (uintptr_t)((Meta*)ea_data(match_source, i))->data-1);
	ea_push(con->sink->filter->node->con_ch_in, ea_data(match_source, i));
      }
    
    if (write_graph) {
    vizp_filter(file, con->sink->filter);
    for(i=0;i<ea_count(match_source);i++)
      fprintf(file, "\"%p\":type ->\"%p\" [color = blue]\n", ea_data(match_source, i), ea_data(match_sink, i));
    for(i=0;i<ea_count(matches_compat);i++)
      fprintf(file, "\"%p\":type ->\"%p\" [color = red]\n", ea_data(matches_compat, i), con->sink);

    if (out) {
      fprintf(file, "subgraph cluster_%p {\n"
		  "label = \"virtual output of %s\";\n"
		   "node [style=filled];\n", out, con->sink->filter->fc->name);
      vizp_meta(file, out);
      fprintf(file, "}\n");
      fprintf(file, "\"%p\":type ->\"%p\" [color = green]\n", old_out, out);
      fprintf(file, "\"%p\":type ->\"%p\" [color = green]\n", con->sink->filter, out);
    }
    }


    if (con->sink->filter->node->con_trees_out && ea_count(con->sink->filter->node->con_trees_out) == 1)
      con = ea_data(con->sink->filter->node->con_trees_out, 0);
    else
      con = NULL;
    
    pos++;
    eina_array_free(matches_compat);
  }
  
  int j;
  Tune_Restriction *restr;
  
  //fix tunings
  for(i=0;i<ea_count(restrictions);i++) {
    restr = ea_data(restrictions, i);
    for(j=ea_count(restr->allowed_values)-1;j>=0;j--)
      if (ea_data(restr->allowed_values, j) && restr->remain > 1)
	restr_remove_value(restr, j);
  }
  
  if (write_graph) {
  //for the graph painting
  for(i=0;i<ea_count(restrictions);i++) {
    restr = ea_data(restrictions, i);
    for(j=0;j<ea_count(restr->allowed_values);j++)
      eina_array_data_set(restr->tune->select, j, ea_data(restr->allowed_values, j));
    vizp_meta(file, restr->tune);
  }
  
  vizp_stop(file);
  vizp_stop(filters);
  }
  
  con = ea_data(f->node->con_trees_out, 0);
  if (con->source->filter->tunes_fixed) {
    //printf("fixed_tunings %s\n",con->source->filter->name);
    con->source->filter->tunes_fixed(con->source->filter);
  }
  while (con) {
    if (con->sink->filter->tunes_fixed) {
      //printf("fixed_tunings %s\n",con->sink->filter->name);
      con->sink->filter->tunes_fixed(con->sink->filter);
    }
    if (con->sink->filter->node->con_trees_out && ea_count(con->sink->filter->node->con_trees_out) == 1)
      con = ea_data(con->sink->filter->node->con_trees_out, 0);
    else
      con = NULL;
  }
  
  eina_array_free(match_source);
  eina_array_free(match_sink);
  for(i=0;i<ea_count(copied);i++)
    free(ea_data(copied, i));
  eina_array_free(copied);
  eina_array_free(copy);
  eina_array_free(restrictions);
  //for(i=0;i<ea_count(trash);i++)
    //free(ea_data(trash, i));
  eina_array_free(trash);
  
  return -1;
}

void meta_undo_tunings_rec(Meta *m)
{
  int i;
  
  if (m->dep) {
    m->dep->data = NULL;
    m->data = NULL;
  }
  
  if (m->childs)
    for(i=0;i<ma_count(m->childs);i++)
      meta_undo_tunings_rec(ma_data(m->childs, i));
}

void _f_undo_tunings(Filter *f)
{
  int j;
  
  if (f->in) {
    for(j=0;j<ea_count(f->in);j++)
      meta_undo_tunings_rec(ea_data(f->in, j));
  }
  
  if (f->out) {
    for(j=0;j<ea_count(f->out);j++)
      meta_undo_tunings_rec(ea_data(f->out, j));
  }
}

void _f_undo_tunings_chain(Filter *f)
{
  while (f) {
    _f_undo_tunings(f);
    if (f->node->con_trees_out && ea_count(f->node->con_trees_out))
      f = ((Con*)ea_data(f->node->con_trees_out, 0))->sink->filter;
    else
      f = NULL;
  }
}

void ea_insert(Eina_Array *ar, int idx, void *data)
{
  int i;
  
  void *last;
  
  for(i=idx;i<ea_count(ar);i++) {
    last = ea_data(ar, i);
    eina_array_data_set(ar, i, data);
    data = last;
  }
  
  ea_push(ar, data);
}

void _filter_insert_connect(int *tried_f, int tried_len, Eina_Array *insert_f, Eina_Array *insert_cons, Eina_Array *new_fs, Filter *source, Filter *sink)
{
  int i;
  Filter *sel_filter;
  Filter *(*filter_new_func)(void);
  
  for(i=0;i<tried_len;i++) {
    filter_new_func = ea_data(insert_f, tried_f[i]);
    sel_filter = filter_new_func();
    //printf("con %s-%s (%d)\n", source->name, sel_filter->name, i);
    ea_push(new_fs, sel_filter);
    ea_push(insert_cons, filter_connect_real(source, 0, sel_filter, 0));
    source = sel_filter;
  }
    
  //printf("con %s-%s (%d)\n", source->name, sink->name, tried_len);
  ea_push(insert_cons, filter_connect_real(source, 0, sink, 0));
}

void succ_insert_load(Eina_Inarray *succ_inserts, int *tried_f, int *tried_len, int n, Config *c)
{
  int i;
  
  Config_Chain *config = eina_inarray_nth(succ_inserts, n);
  
  *tried_len = config->len;
  
  for(i=0;i<config->len;i++)
    tried_f[i] = config->filters[i];
}

int _filter_count_up(int *tried_f, int *tried_len, Eina_Array *insert_f, int err_pos, int max_len, int *try_cache, Config *c)
{
  //printf("f: %d %d (len: %d err: %d)\n", tried_f[0], tried_f[1], *tried_len, err_pos);
  
  if (*try_cache) {
    (*try_cache)--;
  
    if (*try_cache)
      succ_insert_load(c->succ_inserts, tried_f, tried_len, *try_cache, c);
    else {
      tried_f[0] = 0;
      *tried_len = 1;
    }

    return 0;
  }
  
  //err_pos is position of the connection. per default the sink of the connection will
  //be counted up. Only if we're after the last filter we'll attribute this error to 
  //the source
  if (err_pos >= *tried_len)
    err_pos--;
  
  //TODO: all filters after err_pos should still be 0!
  
  //count up 
  tried_f[err_pos]++;
  
  //overflow
  while (tried_f[err_pos] == ea_count(insert_f)) {
    tried_f[err_pos] = 0;
    err_pos--;
    if (err_pos < 0 ) {
      if (*tried_len < max_len) {
	tried_f[*tried_len] = 0;
	(*tried_len)++;
	return 0;
      }
      else
	return -1;
    }
      
    tried_f[err_pos]++;
  }
  
  return 0;
}

Config *config_new(void)
{
  Config *c = calloc(sizeof(Config), 1);
  
  c->applied_metas = eina_array_new(8);
  c->succ_inserts = eina_inarray_new(sizeof(Config_Chain), 8);
  c->config_meta_allocs = eina_array_new(1024);
  c->config_allocs = eina_array_new(1024);
  c->new_fs = eina_array_new(8);
  
  return c;
}

void config_del(Config *c)
{
  eina_array_free(c->applied_metas);
  eina_inarray_free(c->succ_inserts);
  eina_array_free(c->config_meta_allocs);
  eina_array_free(c->config_allocs);
  eina_array_free(c->new_fs);
  
  free(c);
}

int _cons_fix_err(Filter *start_f, Eina_Array *cons, Eina_Array *insert_f, int err_pos_start, Config *c)
{
  int i;
  //int n;
  int try_cache;
  Eina_Array *insert_cons = eina_array_new(8);
  int tried_f[MAX_CONS_TRIES];
  int tried_len;
  Con *con_insert, *con_failed;
  int err_pos;
  Filter *source_f, *sink_f;
  int failed = 0;
  //Config_Chain succ_chain;
  
  _ea_metas_data_zero(c->applied_metas);
  _f_undo_tunings_chain(start_f);
  
  //delete original connection
  con_failed = ea_data(cons, err_pos_start);
  source_f = con_failed->source->filter;
  sink_f = con_failed->sink->filter;
  con_del_real(con_failed);
  
  //printf("failed between %s-%s\n", source_f->name, sink_f->name);
  
  try_cache = eina_inarray_count(c->succ_inserts);
  if (try_cache)
    succ_insert_load(c->succ_inserts, tried_f, &tried_len, 0, c);
  else {
    tried_f[0] = 0;
    tried_len = 1; 
  }
  
  _filter_insert_connect(tried_f, tried_len, insert_f, insert_cons, c->new_fs, source_f, sink_f);
  
  //err_pos now gives error position in insert_cons
  err_pos = test_filter_config_real(start_f, 0, c)-err_pos_start;
  
  while (err_pos >= 0 && err_pos < ea_count(insert_cons) && !failed) {      
    _f_undo_tunings_chain(start_f);
    
    for(i=0;i<ea_count(insert_cons);i++)
      con_del_real(ea_data(insert_cons, i));
    eina_array_flush(insert_cons);
    
    if (_filter_count_up(tried_f, &tried_len, insert_f, err_pos, MAX_CONS_TRIES, &try_cache, c))
      failed = 1;
    _filter_insert_connect(tried_f, tried_len, insert_f, insert_cons, c->new_fs, source_f, sink_f);
    
    err_pos = test_filter_config_real(start_f, 0, c)-err_pos_start;
  }
  
  //FIXME need to check input/output tree/sort by size/???
  /*if (!failed) {
    int found = 0;
    for(n=0;n<eina_inarray_count(succ_inserts);n++) {
      Config_Chain *tmp = eina_inarray_nth(succ_inserts, n);
      if (tmp->len == tried_len) {
	for(i=0;i<tried_len;i++)
	  if(tmp->filters[i] != tried_f[i])
	    break;
	  if (i == tried_len) {
	    found = 1;
	    break;
	  }
      }
    }
    
    if (!found) {
      succ_chain.len = tried_len;
      
      for(i=0;i<tried_len;i++)
	succ_chain.filters[i] = tried_f[i];
      
      eina_inarray_push(succ_inserts, &succ_chain);
    }
  }*/
  
  con_insert = ea_data(insert_cons, 0);
  eina_array_data_set(cons, err_pos_start, con_insert);
  
  for(i=1;i<ea_count(insert_cons);i++)
    ea_insert(cons, err_pos_start+i, ea_data(insert_cons, i));
  
  eina_array_free(insert_cons);
  
  if (failed)
    return -2;

  return err_pos+err_pos_start;
}

/* 
 * needs source filter
 * - resets metas (from tunings/inputs)
 * - undo tunings (recursive/slow! - FIXME)
 * - delete connections
 */
void filter_deconfigure(Filter *f)
{
  Con *con;
  Filter *sink_f;
  Config *c = filter_chain_last_filter(f)->c;
  
  if (!c || !c->configured)
    return;
  
  c->configured = 0;
  
  _ea_metas_data_zero(c->applied_metas);
  _f_undo_tunings_chain(f);
  
  if (c->config_meta_allocs)
    while (eina_array_count(c->config_meta_allocs))
      meta_del(eina_array_pop(c->config_meta_allocs));
  if (c->config_allocs)
    while (eina_array_count(c->config_allocs))
      free(eina_array_pop(c->config_allocs));
  
  if (!f->node->con_trees_in || ! ea_count(f->node->con_trees_in))
    return;
  
  con = ea_data(f->node->con_trees_in, 0);
  assert(!con || con->sink->filter == f);
  
  while (con) {
     sink_f = con->sink->filter;
     
     con_del_real(con);
    
    if (sink_f->node->con_trees_out && ea_count(sink_f->node->con_trees_out))
      con = ea_data(sink_f->node->con_trees_out, 0);
    else
      con = NULL;
  }
  if (c->new_fs)
    while(ea_count(c->new_fs))
      filter_del(ea_pop(c->new_fs));
    
  //config_del(c);
  //filter_chain_last_filter(f)->c = NULL;
}

void _config_reset_internal(Filter *f)
{
  f = filter_chain_last_filter(f);
  
  filter_deconfigure(f);
  
  f = filter_chain_last_filter(f);
  if (f->c) {
    config_del(f->c);
    f->c = NULL;
  }
}

void lime_config_reset(Filter *f)
{  
  pthread_mutex_lock(&f->lock);
  
  Config *c = filter_chain_last_filter(f)->c;
  
  if (!c) {
    pthread_mutex_unlock(&f->lock);
    return;
  }
  
  if (c->refcount) {
    pthread_mutex_unlock(&f->lock);
    printf("FIXME config still has lingering ref (count %d)!\n", c->refcount);
    return;
  }
  _config_reset_internal(f);
  pthread_mutex_unlock(&f->lock);
}

//insert nop filters if necessary
int lime_config_test(Filter *f_sink)
{
  Eina_Array *insert_f;
  int err_pos_start;
  Eina_Array *cons;
  Con *con_orig;
  Filter *f = f_sink;
  Config *c = filter_chain_last_filter(f)->c;
  
  if (c && c->configured) {
    return 0;
  }
  pthread_mutex_lock(&filter_chain_last_filter(f_sink)->lock);
  if (!c) {
    c = config_new();
    filter_chain_last_filter(f)->c = c;
  }
  else {
    assert(!c->refcount);
    _config_reset_internal(f);
    c = config_new();
    filter_chain_last_filter(f)->c = c;
  }

  insert_f =  eina_array_new(4);
  cons = eina_array_new(8);
  
  while (f->node_orig->con_trees_in && ea_count(f->node_orig->con_trees_in)) {
    f = ((Con*)ea_data(f->node_orig->con_trees_in, 0))->source->filter;
  }
  
  Filter *source_f, *sink_f;
  
  if (!f->node_orig->con_trees_out || !ea_count(f->node_orig->con_trees_out)) {
    printf("expected connection from filter %s!\n",f->fc->name);
    abort();
  }
  con_orig = ea_data(f->node_orig->con_trees_out, 0);
  
  while (con_orig) {
    source_f = con_orig->source->filter;
    sink_f = con_orig->sink->filter;
     
    ea_push(cons, filter_connect_real(source_f, 0, sink_f, 0));
    
    if (sink_f->node_orig->con_trees_out && ea_count(sink_f->node_orig->con_trees_out))
      con_orig = ea_data(sink_f->node_orig->con_trees_out, 0);
    else
      con_orig = NULL;
  }
  
  
  ea_push(insert_f, filter_core_interleave.filter_new_f);
  ea_push(insert_f, filter_core_loadjpeg.filter_new_f);
  ea_push(insert_f, filter_core_convert.filter_new_f);
  ea_push(insert_f, filter_core_loadtiff.filter_new_f);
  ea_push(insert_f, filter_core_fliprot.filter_new_f);
  
  err_pos_start = test_filter_config_real(f, 0, c);
  
  while (err_pos_start != -1) {
    err_pos_start = _cons_fix_err(f, cons, insert_f, err_pos_start, c);
    if (err_pos_start == -2) {
      _config_reset_internal(f_sink);
      eina_array_free(cons);
      eina_array_free(insert_f);
      pthread_mutex_unlock(&filter_chain_last_filter(f_sink)->lock);
      return -1;
    }
  }
  
  c->configured = 1;
  
  filter_hash_recalc(f);
  
  /*printf("[CONFIG] actual filter chain:\n");
  f = f_sink;
  while (f->node->con_trees_in && ea_count(f->node->con_trees_in)) {
    printf("         %s\n", f->fc->name);
    f = ((Con*)ea_data(f->node->con_trees_in, 0))->source->filter;
  }
  printf("         %s\n", f->fc->name);*/
  
  eina_array_free(cons);
  eina_array_free(insert_f);
  pthread_mutex_unlock(&filter_chain_last_filter(f_sink)->lock);
  return 0;
}