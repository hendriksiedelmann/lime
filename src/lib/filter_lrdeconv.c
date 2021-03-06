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

//TODO contains stuff from rawtherapee, mention this!

#include "filter_lrdeconv.h"

#include <math.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_spline.h>
#include <signal.h>

#include <cv.h>

#include "tvdeconv/tvreg.h"
#include "tvdeconv/cliio.h"

#include "opencv_helpers.h"

static const int bord = 8;
static const int mul_one = 1024;

typedef struct {
  int iterations;
  float radius;
  float sharpen;
  float gamma1;
  float gamma2;
  float damp;
  void *blur_b, *estimate_b, *fac_b;
  uint8_t lut[65536];
} _Common;

typedef struct {
  _Common *common;
  void *blur_b, *estimate_b, *fac_b;
} _Data;

static void simplegauss(Rect area, uint16_t *in, uint16_t *out, float rad)
{ 
  cv_gauss(area.width, area.height, CV_16UC1, in, out, rad);
}

static void lrdiv(Rect area, uint16_t *observed, uint16_t *blur, uint16_t *out)
{
  int i, j;
  //h blur
  for(j=0;j<area.height;j++)
    for(i=0;i<area.width;i++) {
      int pos = j*area.width+i;
      if (blur[pos])
        out[pos] = imin(observed[pos]*mul_one/blur[pos], 65535);
      else
        out[pos] = 65535;
    }
}

static void lrdampdiv2(Rect area, uint16_t *observed, uint16_t *blur, uint16_t *out, float damping)
{
  int i, j;
  //h blur
  for(j=0;j<area.height;j++)
    for(i=0;i<area.width;i++) {
      int pos = j*area.width+i;
      float fac;
      if (observed[pos] && blur[pos]) {
        fac = fabs(observed[pos]/blur[pos]-1.0);
        fac += fabs(blur[pos]/observed[pos]-1.0);
        fac = fac + 1.0 - damping*0.02;
        if (fac <= 0.0) fac = 0.0;
        if (fac >= 1.0) fac = 1.0;
      }
      else fac = 1.0;
      
      if (blur[pos])
        out[pos] = fac*imin(observed[pos]*mul_one/blur[pos], 65535)+(1.0-fac)*mul_one;
      else
        out[pos] = mul_one;
    }
}


static inline int32_t floatToRawIntBits(float d) {
  union {
    float f;
    int32_t i;
  } tmp;
  tmp.f = d;
  return tmp.i;
}

static inline float intBitsToFloat(int32_t i) {
  union {
    float f;
    int32_t i;
  } tmp;
  tmp.i = i;
  return tmp.f;
}

static inline int xisinff(float x) { return x == INFINITY || x == -INFINITY; }

static inline float mlaf(float x, float y, float z) { return x * y + z; }

static inline int ilogbp1f(float d) {
  int m = d < 5.421010862427522E-20f;
  d = m ? 1.8446744073709552E19f * d : d;
  int q = (floatToRawIntBits(d) >> 23) & 0xff;
  q = m ? q - (64 + 0x7e) : q - 0x7e;
  return q;
}

static inline float ldexpkf(float x, int q) {
  float u;
  int m;
  m = q >> 31;
  m = (((m + q) >> 6) - m) << 4;
  q = q - (m << 2);
  u = intBitsToFloat(((int32_t)(m + 0x7f)) << 23);
  u = u * u;
  x = x * u * u;
  u = intBitsToFloat(((int32_t)(q + 0x7f)) << 23);
  return x * u;
}

static inline float xlogf(float d) {
  float x, x2, t, m;
  int e;

  e = ilogbp1f(d * 0.7071f);
  m = ldexpkf(d, -e);

  x = (m-1.0f) / (m+1.0f);
  x2 = x * x;

  t = 0.2371599674224853515625f;
  t = mlaf(t, x2, 0.285279005765914916992188f);
  t = mlaf(t, x2, 0.400005519390106201171875f);
  t = mlaf(t, x2, 0.666666567325592041015625f);
  t = mlaf(t, x2, 2.0f);

  x = x * t + 0.693147180559945286226764f * e;

  if (xisinff(d)) x = INFINITY;
  if (d < 0) x = NAN;
  if (d == 0) x = -INFINITY;

  return x;
}

static void lrdampdiv(Rect area, uint16_t *observed, uint16_t *blur, uint16_t *out, float damp)
{
  int i, j;
  float O, U, I;
  float fac = -2.0/(damp*damp);
  //h blur
  for(j=0;j<area.height;j++)
    for(i=0;i<area.width;i++) {
      int pos = j*area.width+i;
      O = observed[pos]*(1.0/65536.0);
      I = blur[pos]*(1.0/65536.0);
      if (blur[pos]) {
        U = (O * xlogf(I/O) - I + O) * fac;
        //printf("\n %f %f %f (%f) \n", U, I/O, xlogf(I/O), (O * xlogf(I/O) - I + O));
        U = fmin(U,1.0f);
        U = U*U*U*U*(5.0-U*4.0);
        out[pos] = ((O - I) / I * U + 1.0)*mul_one;
        //printf("%f %f %f = %f \n", O, U, I, ((O - I) / I * U + 1.0));
      }
      else
        out[pos] = 0;
    }
}


static void lrmul(Rect area, uint16_t *a, uint16_t *b, uint16_t *out)
{
  int i, j;
  
  for(j=0;j<bord/2;j++)
    for(i=0;i<area.width;i++) {
      int pos = j*area.width+i;
      out[pos] = a[pos];
    }
  for(j=area.height-bord/2;j<area.height;j++)
    for(i=0;i<area.width;i++) {
      int pos = j*area.width+i;
      out[pos] = a[pos];
    }
  for(j=bord/2;j<area.height-bord/2;j++)
    for(i=0;i<bord/2;i++) {
      int pos = j*area.width+i;
      out[pos] = a[pos];
    }
  for(j=bord/2;j<area.height-bord/2;j++)
    for(i=area.width-bord/2;i<area.width;i++) {
      int pos = j*area.width+i;
      out[pos] = a[pos];
    }
  
  for(j=bord/2;j<area.height-bord/2;j++)
    for(i=bord/2;i<area.width-bord/2;i++) {
      int pos = j*area.width+i;
      out[pos] = imin(a[pos]*b[pos]/mul_one, 65535);
    }
}

static void catch_sigfpe(int signo) {
    printf("encountered SIGFPE\n");
}

static void _worker(Filter *f, Eina_Array *in, Eina_Array *out, Rect *area, int thread_id)
{ 
  int i, j;
  uint16_t *output;
  uint16_t *input;
  _Data *data = ea_data(f->data, thread_id);
  uint16_t *blur, *estimate, *fac;
  Tiledata *in_td, *out_td;
  Rect in_area;
  const int size = sizeof(uint16_t)*(DEFAULT_TILE_SIZE+2*bord)*(DEFAULT_TILE_SIZE+2*bord);
  
  assert(in && ea_count(in) == 1);
  assert(out && ea_count(out) == 1);
  
  hack_tiledata_fixsize(2, ea_data(out, 0));
  
  in_td = ((Tiledata*)ea_data(in, 0));
  in_area = in_td->area;
  input = in_td->data;
  out_td = ((Tiledata*)ea_data(out, 0));
  output = out_td->data;
  
  if (signal(SIGFPE, catch_sigfpe) == SIG_ERR) {
    fputs("An error occurred while setting a signal handler.\n", stderr);
    return EXIT_FAILURE;
  }
  
  if (area->corner.scale) {
    memcpy(output,input,sizeof(uint16_t)*in_td->area.width*in_td->area.height);
    return;
  }

    
  {
    image u, fi, kernel;
    num Lambda = data->common->damp;
    const char noise_str[64];
    const char kernel_str[64];
    tvregopt *Opt = NULL;
    int Success;
    
    sprintf(kernel_str,"gaussian:%f", data->common->radius);
    sprintf(noise_str,"poisson");
    
    
    fi.Width = in_area.width;
    fi.Height = in_area.height;
    fi.NumChannels = 1;
    fi.Data = malloc(sizeof(num)*fi.Width*fi.Height);
    
    u = fi;
    u.Data = malloc(sizeof(num)*fi.Width*fi.Height);
    
    kernel.Data = NULL;
    
    for(i=0;i<fi.Width*fi.Height;i++)
      fi.Data[i] = input[i]*1.0/65536.0;
    
    if(!(Opt = TvRegNewOpt()))
    {
        fputs("Out of memory.\n", stderr);
        return;
    }
    
    if(!(TvRegSetNoiseModel(Opt, noise_str)))
    {
        fprintf(stderr, "Unknown noise model, \"%s\".\n", noise_str);
        TvRegFreeOpt(Opt);
        return;
    }
    //FIXMe free kernel!
    if (!ReadKernel(&kernel, kernel_str))
    {
      fprintf(stderr, "read kernel failed, \"%s\".\n", kernel_str);
      TvRegFreeOpt(Opt);
      return;
    }
    
    memcpy(u.Data, fi.Data, sizeof(num)*((size_t)fi.Width)
        *((size_t)fi.Height)*fi.NumChannels);
    TvRegSetKernel(Opt, kernel.Data, kernel.Width, kernel.Height);
    TvRegSetLambda(Opt, Lambda);
    TvRegSetMaxIter(Opt, data->common->iterations);
    TvRegSetGamma1(Opt, data->common->gamma1);
    TvRegSetGamma2(Opt, data->common->gamma2);
    TvRegSetTol(Opt, 0.0);
    
    TvRegPrintOpt(Opt);
    if(!(Success = TvRestore(u.Data, fi.Data,
        fi.Width, fi.Height, fi.NumChannels, Opt)))
        fputs("Error in computation.\n", stderr);
    
    TvRegFreeOpt(Opt);
    
    for(j=0;j<area->height;j++)
      for(i=0;i<area->width;i++)
          output[j*area->width+i] = clip_u16(u.Data[(j+bord)*in_area.width+i+bord]*65536.0);
    
    free(u.Data);
    free(fi.Data);
      
    return;
  }
  
  if (!data->blur_b) {
    data->blur_b = malloc(size);
    data->estimate_b = malloc(size);
    data->fac_b = malloc(size);
  }
  blur = data->blur_b,
  estimate = data->estimate_b,
  fac = data->fac_b;
  void *fac2 = malloc(size);
  
  if (area->corner.scale) {
    memcpy(output,input,sizeof(uint16_t)*in_td->area.width*in_td->area.height);
    free(fac2);
    return;
  }
  
  uint16_t *newestimate = malloc(size);
  
  memcpy(estimate, input, sizeof(uint16_t)*in_area.width*in_area.height);
  
  if (data->common->radius)
    for(i=0;i<data->common->iterations;i++) {
      simplegauss(in_area, estimate, blur, data->common->radius);
      if (data->common->damp == 0.0)
        lrdiv(in_area, input, blur, fac);
      else
        //lrdampdiv(in_area, input, blur, fac, data->common->damp/500.0);
        lrdampdiv2(in_area, input, blur, fac, data->common->damp);
      simplegauss(in_area, fac, fac2, data->common->radius);
      lrmul(in_area, estimate, fac2, newestimate);
      memcpy(estimate, newestimate, size);
    }
  
  free(newestimate);
  
  if (data->common->sharpen == 0.0) {
    for(j=0;j<area->height;j++)
      for(i=0;i<area->width;i++) {
          output[j*area->width+i] = estimate[(j+bord)*in_area.width+i+bord];
      }
  }
  else {
    //16bit sharpen
    float s = data->common->sharpen*0.01;
    uint16_t *buf_out, *buf_in1, *buf_in2, *buf_in3;
    for(j=0;j<area->height;j++) {
      buf_out = tileptr16(out_td, area->corner.x, area->corner.y+j);
      buf_in1 = estimate+(j+bord-1)*in_area.width+bord;
      buf_in2 = estimate+(j+bord+0)*in_area.width+bord;
      buf_in3 = estimate+(j+bord+1)*in_area.width+bord;
      for(i=0;i<area->width;i++) {
        *buf_out =  clip_u16(((1.0+4*s)*buf_in2[0] - s*(buf_in1[0] + buf_in2[-1] + buf_in2[1] + buf_in3[0])));
        buf_out++;
        buf_in1++;
        buf_in2++;
        buf_in3++;
      }
    }
  }
  
  free(fac2);
}

static void _area_calc(Filter *f, Rect *in, Rect *out)
{ 
  if (in->corner.scale) {
    *out = *in;
  }
  else {
    out->corner.scale = in->corner.scale;
    out->corner.x = in->corner.x-bord;
    out->corner.y = in->corner.y-bord;
    out->width = in->width+2*bord;
    out->height = in->height+2*bord;
  }
}

static int _del(Filter *f)
{
  int i;
  _Data *data;
  
  for(i=0;i<ea_count(f->data);i++) {
    data = ea_data(f->data, i);
    if (data->blur_b) {
      free(data->blur_b);
      free(data->estimate_b);
      free(data->fac_b);
    }
    free(data);
  }
  
  return 0;
}

static void *_data_new(Filter *f, void *data)
{
  _Data *newdata = calloc(sizeof(_Data), 1);
  
  newdata->common = ((_Data*)data)->common;
  
  return newdata;
}

static Filter *_new(void)
{
  Filter *f = filter_new(&filter_core_lrdeconv);
  
  Meta *in, *out, *setting, *bd_in, *bd_out, *tune_color, *bound;
  _Data *data = calloc(sizeof(_Data), 1);
  f->mode_buffer = filter_mode_buffer_new();
  f->mode_buffer->worker = _worker;
  f->mode_buffer->area_calc = _area_calc;
  f->mode_buffer->threadsafe = 0;
  f->mode_buffer->data_new = &_data_new;
  f->del = _del;
  ea_push(f->data, data);
  f->fixme_outcount = 1;
  
  data->common = calloc(sizeof(_Common), 1);
  data->common->iterations = 30;
  data->common->radius = 1.0;
  data->common->gamma1 = 5.0;
  data->common->gamma2 = 8.0;
  
  //tune color-space
  tune_color = meta_new_select(MT_COLOR, f, eina_array_new(3));
  pushint(tune_color->select, CS_LAB_L);
  tune_color->replace = tune_color;
  tune_color->dep = tune_color;
  eina_array_push(f->tune, tune_color);
  
  //tune bitdepth
  bd_out = meta_new_select(MT_BITDEPTH, f, eina_array_new(2));
  pushint(bd_out->select, BD_U16);
  //pushint(bd_out->select, BD_U8);
  bd_out->dep = bd_out;
  eina_array_push(f->tune, bd_out);
  
  bd_in = meta_new_select(MT_BITDEPTH, f, eina_array_new(2));
  pushint(bd_in->select, BD_U16);
  //pushint(bd_in->select, BD_U8);
  bd_in->replace = bd_out;
  bd_in->dep = bd_in;
  eina_array_push(f->tune, bd_in);
  
  //output
  out = meta_new_channel(f, 1);
  meta_attach(out, tune_color);
  meta_attach(out, bd_out);
  eina_array_push(f->out, out);
  
  //input
  in = meta_new_channel(f, 1);
  meta_attach(in, tune_color);
  meta_attach(in, bd_in);
  eina_array_push(f->in, in);
  in->replace = out;
  
  //setting
  setting = meta_new_data(MT_FLOAT, f, &data->common->radius);
  meta_name_set(setting, "radius");
  eina_array_push(f->settings, setting);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 0.0;
  meta_name_set(bound, "PARENT_SETTING_MIN");
  meta_attach(setting, bound);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 2.5;
  meta_name_set(bound, "PARENT_SETTING_MAX");
  meta_attach(setting, bound);
  
  //setting
  setting = meta_new_data(MT_FLOAT, f, &data->common->damp);
  meta_name_set(setting, "damping");
  eina_array_push(f->settings, setting);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 1.0;
  meta_name_set(bound, "PARENT_SETTING_MIN");
  meta_attach(setting, bound);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 10000.0;
  meta_name_set(bound, "PARENT_SETTING_MAX");
  meta_attach(setting, bound);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 10.0;
  meta_name_set(bound, "PARENT_SETTING_STEP");
  meta_attach(setting, bound);
  
  //setting
  setting = meta_new_data(MT_INT, f, &data->common->iterations);
  meta_name_set(setting, "iterations");
  eina_array_push(f->settings, setting);
  
  bound = meta_new_data(MT_INT, f, malloc(sizeof(int)));
  *(int*)bound->data = 1;
  meta_name_set(bound, "PARENT_SETTING_MIN");
  meta_attach(setting, bound);
  
  bound = meta_new_data(MT_INT, f, malloc(sizeof(int)));
  *(int*)bound->data = 100;
  meta_name_set(bound, "PARENT_SETTING_MAX");
  meta_attach(setting, bound);
  
  //setting
  setting = meta_new_data(MT_FLOAT, f, &data->common->sharpen);
  meta_name_set(setting, "sharpen");
  eina_array_push(f->settings, setting);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 0.0;
  meta_name_set(bound, "PARENT_SETTING_MIN");
  meta_attach(setting, bound);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 100.0;
  meta_name_set(bound, "PARENT_SETTING_MAX");
  meta_attach(setting, bound);
  
  //setting
  setting = meta_new_data(MT_FLOAT, f, &data->common->gamma1);
  meta_name_set(setting, "gamma1");
  eina_array_push(f->settings, setting);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 0.0;
  meta_name_set(bound, "PARENT_SETTING_MIN");
  meta_attach(setting, bound);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 100.0;
  meta_name_set(bound, "PARENT_SETTING_MAX");
  meta_attach(setting, bound);
  
  //setting
  setting = meta_new_data(MT_FLOAT, f, &data->common->gamma2);
  meta_name_set(setting, "gamma2");
  eina_array_push(f->settings, setting);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 0.0;
  meta_name_set(bound, "PARENT_SETTING_MIN");
  meta_attach(setting, bound);
  
  bound = meta_new_data(MT_FLOAT, f, malloc(sizeof(float)));
  *(float*)bound->data = 1000.0;
  meta_name_set(bound, "PARENT_SETTING_MAX");
  meta_attach(setting, bound);
  return f;
}

Filter_Core filter_core_lrdeconv = {
  "Lucy–Richardson Deconvolution",
  "deconvolution",
  "Deconvolve/sharpen using Lucy–Richardson Deconvolution",
  &_new
};
