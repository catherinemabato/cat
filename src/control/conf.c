/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/calculator.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/conf.h"
#include "conf_gen.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

typedef struct dt_conf_dreggn_t
{
  GSList *result;
  const char *match;
} dt_conf_dreggn_t;

static void _free_confgen_value(void *value)
{
  dt_confgen_value_t *s = (dt_confgen_value_t *)value;
  g_free(s->def);
  g_free(s->min);
  g_free(s->max);
  g_free(s);
}

/** return slot for this variable or newly allocated slot. */
static inline char *dt_conf_get_var(const char *name)
{
  char *str;

  dt_pthread_mutex_lock(&darktable.conf->mutex);

  str = (char *)g_hash_table_lookup(darktable.conf->override_entries, name);
  if(str) goto fin;

  str = (char *)g_hash_table_lookup(darktable.conf->table, name);
  if(str) goto fin;

  // not found, try defaults
  str = (char *)dt_confgen_get(name, DT_DEFAULT);
  if(str)
  {
    char *str_new = g_strdup(str);
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str_new);
    str = str_new;
    goto fin;
  }

  // FIXME: why insert garbage?
  // still no luck? insert garbage:
  str = (char *)g_malloc0(sizeof(int32_t));
  g_hash_table_insert(darktable.conf->table, g_strdup(name), str);

fin:
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return str;
}

/* set the value only if it hasn't been overridden from commandline
 * return 1 if key/value is still the one passed on commandline. */
static int dt_conf_set_if_not_overridden(const char *name, char *str)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);

  char *over = (char *)g_hash_table_lookup(darktable.conf->override_entries, name);
  const int is_overridden = (over && !strcmp(str, over));
  if(!is_overridden)
  {
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  }

  dt_pthread_mutex_unlock(&darktable.conf->mutex);

  return is_overridden;
}

void dt_conf_set_int(const char *name, int val)
{
  char *str = g_strdup_printf("%d", val);
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_int64(const char *name, int64_t val)
{
  char *str = g_strdup_printf("%" PRId64, val);
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_float(const char *name, float val)
{
  char *str = (char *)g_malloc(G_ASCII_DTOSTR_BUF_SIZE);
  g_ascii_dtostr(str, G_ASCII_DTOSTR_BUF_SIZE, val);
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_bool(const char *name, int val)
{
  char *str = g_strdup_printf("%s", val ? "TRUE" : "FALSE");
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_string(const char *name, const char *val)
{
  char *str = g_strdup(val);
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

int dt_conf_get_int(const char *name)
{
  const char *str = dt_conf_get_var(name);
  float new_value = dt_calculator_solve(1, str);
  if(isnan(new_value))
  {
    //we've got garbage, check default
    const char *def_val = dt_confgen_get(name, DT_DEFAULT);
    if(def_val)
    {
      new_value = dt_calculator_solve(1, def_val);
      if(isnan(new_value))
        new_value = 0.0;
      else
      {
        char *fix_badval = g_strdup(def_val);
        if(dt_conf_set_if_not_overridden(name, fix_badval))
          g_free(fix_badval);
      }
    }
    else
    {
      new_value = 0.0;
    }
  }

  int val;
  if(new_value > 0)
    val = new_value + 0.5;
  else
    val = new_value - 0.5;
  return val;
}

int64_t dt_conf_get_int64(const char *name)
{
  const char *str = dt_conf_get_var(name);
  float new_value = dt_calculator_solve(1, str);
  if(isnan(new_value))
  {
    //we've got garbage, check default
    const char *def_val = dt_confgen_get(name, DT_DEFAULT);
    if(def_val)
    {
      new_value = dt_calculator_solve(1, def_val);
      if(isnan(new_value))
        new_value = 0.0;
      else
      {
        char *fix_badval = g_strdup(def_val);
        if(dt_conf_set_if_not_overridden(name, fix_badval))
          g_free(fix_badval);
      }
    }
    else
    {
      new_value = 0.0;
    }
  }

  int64_t val;
  if(new_value > 0)
    val = new_value + 0.5;
  else
    val = new_value - 0.5;
  return val;
}

float dt_conf_get_float(const char *name)
{
  const char *str = dt_conf_get_var(name);
  float new_value = dt_calculator_solve(1, str);
  if(isnan(new_value))
  {
    //we've got garbage, check default
    const char *def_val = dt_confgen_get(name, DT_DEFAULT);
    if(def_val)
    {
      new_value = dt_calculator_solve(1, def_val);
      if(isnan(new_value))
        new_value = 0.0;
      else
      {
        char *fix_badval = g_strdup(def_val);
        if(dt_conf_set_if_not_overridden(name, fix_badval))
          g_free(fix_badval);
      }
    }
    else
    {
      new_value = 0.0;
    }
  }
  return new_value;
}

int dt_conf_get_and_sanitize_int(const char *name, int min, int max)
{
  int val = dt_conf_get_int(name);
  val = CLAMPS(val, min, max);
  dt_conf_set_int(name, val);
  return val;
}

int64_t dt_conf_get_and_sanitize_int64(const char *name, int64_t min, int64_t max)
{
  int64_t val = dt_conf_get_int64(name);
  val = CLAMPS(val, min, max);
  dt_conf_set_int64(name, val);
  return val;
}

float dt_conf_get_and_sanitize_float(const char *name, float min, float max)
{
  float val = dt_conf_get_float(name);
  val = CLAMPS(val, min, max);
  dt_conf_set_float(name, val);
  return val;
}

int dt_conf_get_bool(const char *name)
{
  const char *str = dt_conf_get_var(name);
  const int val = (str[0] == 'T') || (str[0] == 't');
  return val;
}

gchar *dt_conf_get_string(const char *name)
{
  const char *str = dt_conf_get_var(name);
  return g_strdup(str);
}

static char *_sanitize_confgen(const char *name, const char *value)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(!item) return g_strdup(value);

  char result[100] = { 0 };
  g_strlcpy(result, value, sizeof(result));

  switch(item->type)
  {
    case DT_INT:
    {
      float v = dt_calculator_solve(1, value);

      const int min = item->min ? (int)dt_calculator_solve(1, item->min) : INT_MIN;
      const int max = item->max ? (int)dt_calculator_solve(1, item->max) : INT_MAX;
      // if garbadge, use default
      const int val = isnan(v) ? dt_confgen_get_int(name, DT_DEFAULT) : (int)v;
      g_snprintf(result, sizeof(result), "%d", CLAMP(val, min, max));
    }
    break;
    case DT_INT64:
    {
      float v = dt_calculator_solve(1, value);

      const int64_t min = item->min ? (int64_t)dt_calculator_solve(1, item->min) : INT64_MIN;
      const int64_t max = item->max ? (int64_t)dt_calculator_solve(1, item->max) : INT64_MAX;
      // if garbadge, use default
      const int64_t val = isnan(v) ? dt_confgen_get_int64(name, DT_DEFAULT) : (int64_t)v;
      g_snprintf(result, sizeof(result), "%ld", CLAMP(val, min, max));
    }
    break;
    case DT_FLOAT:
    {
      float v = dt_calculator_solve(1, value);

      const float min = item->min ? (float)dt_calculator_solve(1, item->min) : -FLT_MAX;
      const float max = item->max ? (float)dt_calculator_solve(1, item->max) : FLT_MAX;
      // if garbadge, use default
      const float val = isnan(v) ? dt_confgen_get_float(name, DT_DEFAULT) : v;
      g_snprintf(result, sizeof(result), "%f", CLAMP(val, min, max));
    }
    break;
    case DT_BOOL:
    {
      if(strcmp(value, "true") && strcmp(value, "false"))
        g_snprintf(result, sizeof(result), "%s", dt_confgen_get(name, DT_DEFAULT));
    }
    break;
    default:
      break;
  }

  return g_strdup(result);
}

void dt_conf_init(dt_conf_t *cf, const char *filename, GSList *override_entries)
{
  cf->x_confgen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _free_confgen_value);

  dt_confgen_init();

  cf->table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  cf->override_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  dt_pthread_mutex_init(&darktable.conf->mutex, NULL);

  // init conf filename
  g_strlcpy(darktable.conf->filename, filename, sizeof(darktable.conf->filename));

#define LINE_SIZE 1023

  char line[LINE_SIZE + 1];

  FILE *f = NULL;
  gboolean defaults = FALSE;

  // check for user config
  f = g_fopen(filename, "rb");

  // if file has been found, parse it

  if(f)
  {
    while(!feof(f))
    {
      const int read = fscanf(f, "%" STR(LINE_SIZE) "[^\r\n]\r\n", line);
      if(read > 0)
      {
        char *c = line;
        char *end = line + strlen(line);
        // check for '=' which is separator between the conf name and value
        while(*c != '=' && c < end) c++;

        if(*c == '=')
        {
          *c = '\0';

          char *name = g_strdup(line);
          // ensure that numbers are properly clamped if min/max
          // defined and if not and garbage is read then the default
          // value is returned.
          char *value = _sanitize_confgen(name, (const char *)(c + 1));

          g_hash_table_insert(darktable.conf->table, name, value);
        }
      }
    }
    fclose(f);
  }
  else
  {
    // this is first run, remember we init
    defaults = TRUE;

    // we initialize the conf table with default values
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init (&iter, darktable.conf->x_confgen);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = (const char *)key;
      const dt_confgen_value_t *entry = (dt_confgen_value_t *)value;
      g_hash_table_insert(darktable.conf->table, g_strdup(name), g_strdup(entry->def));
    }
  }

  // for the very first time after a fresh install
  // execute performance configuration no matter what
  if(defaults)
    dt_configure_performance();

  if(override_entries)
  {
    GSList *p = override_entries;
    while(p)
    {
      dt_conf_string_entry_t *entry = (dt_conf_string_entry_t *)p->data;
      g_hash_table_insert(darktable.conf->override_entries, entry->key, entry->value);
      p = g_slist_next(p);
    }
  }

#undef LINE_SIZE

  return;
}

static void dt_conf_print(const gchar *key, const gchar *val, FILE *f)
{
  fprintf(f, "%s=%s\n", key, val);
}

void dt_conf_cleanup(dt_conf_t *cf)
{
  FILE *f = g_fopen(cf->filename, "wb");
  if(f)
  {
    GList *keys = g_hash_table_get_keys(cf->table);
    GList *sorted = g_list_sort(keys, (GCompareFunc)g_strcmp0);

    GList *iter = sorted;

    while(iter)
    {
      const gchar *key = (const gchar *)iter->data;
      const gchar *val = (const gchar *)g_hash_table_lookup(cf->table, key);
      dt_conf_print(key, val, f);
      iter = g_list_next(iter);
    }

    g_list_free(sorted);
    fclose(f);
  }
  g_hash_table_unref(cf->table);
  g_hash_table_unref(cf->override_entries);
  g_hash_table_unref(cf->x_confgen);
  dt_pthread_mutex_destroy(&darktable.conf->mutex);
}

/** check if key exists, return 1 if lookup succeeded, 0 if failed..*/
int dt_conf_key_exists(const char *key)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int res = (g_hash_table_lookup(darktable.conf->table, key) != NULL)
                  || (g_hash_table_lookup(darktable.conf->override_entries, key) != NULL);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return res;
}

static void _conf_add(char *key, char *val, dt_conf_dreggn_t *d)
{
  if(strncmp(key, d->match, strlen(d->match)) == 0)
  {
    dt_conf_string_entry_t *nv = (dt_conf_string_entry_t *)g_malloc(sizeof(dt_conf_string_entry_t));
    nv->key = g_strdup(key + strlen(d->match) + 1);
    nv->value = g_strdup(val);
    d->result = g_slist_append(d->result, nv);
  }
}

/** get all strings in */
GSList *dt_conf_all_string_entries(const char *dir)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  dt_conf_dreggn_t d;
  d.result = NULL;
  d.match = dir;
  g_hash_table_foreach(darktable.conf->table, (GHFunc)_conf_add, &d);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return d.result;
}

void dt_conf_string_entry_free(gpointer data)
{
  dt_conf_string_entry_t *nv = (dt_conf_string_entry_t *)data;
  g_free(nv->key);
  g_free(nv->value);
  nv->key = NULL;
  nv->value = NULL;
  g_free(nv);
}

gboolean dt_confgen_exists(const char *name)
{
  return g_hash_table_lookup(darktable.conf->x_confgen, name) != NULL;
}

dt_confgen_type_t dt_confgen_type(const char *name)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
    return item->type;
  else
    return DT_STRING;
}

gboolean dt_confgen_value_exists(const char *name, dt_confgen_value_kind_t kind)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  switch(kind)
  {
     case DT_DEFAULT:
       return item->def != NULL;
     case DT_MIN:
       return item->min != NULL;
     case DT_MAX:
       return item->max != NULL;
  }
  return FALSE;
}

const char *dt_confgen_get(const char *name, dt_confgen_value_kind_t kind)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
    switch(kind)
    {
       case DT_DEFAULT:
         return item->def;
       case DT_MIN:
         return item->min;
       case DT_MAX:
         return item->max;
    }
  }

  return "";
}

int dt_confgen_get_int(const char *name, dt_confgen_value_kind_t kind)
{
  const char *str = dt_confgen_get(name, kind);
  const float value = dt_calculator_solve(1, str);
  return (int)value;
}

int64_t dt_confgen_get_int64(const char *name, dt_confgen_value_kind_t kind)
{
  const char *str = dt_confgen_get(name, kind);
  const float value = dt_calculator_solve(1, str);
  return (int64_t)value;
}

gboolean dt_confgen_get_bool(const char *name, dt_confgen_value_kind_t kind)
{
  const char *str = dt_confgen_get(name, kind);
  return !strcmp(str, "true");
}

float dt_confgen_get_float(const char *name, dt_confgen_value_kind_t kind)
{
  const char *str = dt_confgen_get(name, kind);
  const float value = dt_calculator_solve(1, str);
  return value;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
