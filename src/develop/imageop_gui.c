/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "develop/imageop_gui.h"
#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "gui/color_picker_proxy.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <time.h>

typedef struct dt_module_param_t
{
  dt_iop_module_t *module;
  int param_offset;
} dt_module_param_t;

static void generic_slider_callback(GtkWidget *slider, dt_module_param_t *data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = data->module;
  dt_iop_params_t *p = (dt_iop_params_t *)self->params;

  /* todo: 
     call generic callback to update module gui
     save previous value
     if configure_callback defined, call with param and prev value
     same configure_callback can also be called from gui_update (with param=null)
  */

  *(float*)(p + data->param_offset) = dt_bauhaus_slider_get(slider);

  dt_iop_color_picker_reset(self, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

GtkWidget *dt_bauhaus_slider_new_from_params_box(dt_iop_module_t *self, const char *param, const char *post)
{
  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_introspection_field_t *f = self->so->get_f(param);

  GtkWidget *slider = NULL;
  gchar *str;

  if (f && f->header.type == DT_INTROSPECTION_TYPE_FLOAT)
  {
    float min = f->Float.Min;
    float max = f->Float.Max;
    float defval = *(float*)self->so->get_p(p, param);
    int digits = 2;
    float step;

    float top = fmaxf(fabsf(min),fabsf(max));
    if (top>=100)
    {
      step = 1.f;
    }
    else
    {
      step = top / 100;
      float log10step = log10f(step);
      float fdigits = floorf(log10step+.1);
      step = powf(10.f,fdigits);
      if (log10step - fdigits > .5)
        step *= 5;
      if (fdigits < -2.f) 
        digits = -fdigits;
    }

    slider = dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, step, defval, digits, 1);

    if (*f->Float.header.description)
    {
      dt_bauhaus_widget_set_label(slider, NULL, gettext(f->Float.header.description));
    }
    else
    {
      str = dt_util_str_replace(f->Float.header.field_name, "_", " ");
    
      dt_bauhaus_widget_set_label(slider, NULL, gettext(str));

      g_free(str);
    }

    if (min < 0 || (post && *post))
    {
      str = g_strdup_printf("%%%s.0%df%s", (min < 0 ? "+" : ""), digits, post);

      dt_bauhaus_slider_set_format(slider, str);
    
      g_free(str);
    }

    dt_module_param_t *module_param = (dt_module_param_t *)g_malloc(sizeof(dt_module_param_t));
    module_param->module = self;
    module_param->param_offset = self->so->get_p(p, param) - p;
    g_signal_connect_data(G_OBJECT(slider), "value-changed", G_CALLBACK(generic_slider_callback), module_param, (GClosureNotify)g_free, 0);

    // todo: add tooltip
  }
  else
  {
    str = g_strdup_printf(_("'%s' is not a parameter"), param);

    slider = GTK_WIDGET(GTK_LABEL(gtk_label_new(str)));

    g_free(str);
  }

  gtk_box_pack_start(GTK_BOX(self->widget), slider, TRUE, TRUE, 0);

  return slider;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
