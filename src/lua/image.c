/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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

#include "lua/image.h"
#include "lua/types.h"
#include "lua/glist.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "metadata_gen.h"

/***********************************************************************
  handling of dt_image_t
 **********************************************************************/

static const dt_image_t*checkreadimage(lua_State*L,int index) {
  dt_lua_image_t imgid;
  luaA_to(L,dt_lua_image_t,&imgid,index);
  return dt_image_cache_read_get(darktable.image_cache,imgid);
}

static void releasereadimage(lua_State*L,const dt_image_t* image) {
  dt_image_cache_read_release(darktable.image_cache,image);
}

static dt_image_t*checkwriteimage(lua_State*L,int index) {
  const dt_image_t* my_readimage=checkreadimage(L,index);
  return dt_image_cache_write_get(darktable.image_cache,my_readimage);
}

static void releasewriteimage(lua_State*L,dt_image_t* image) {
  dt_image_cache_write_release(darktable.image_cache,image,DT_IMAGE_CACHE_SAFE);
  releasereadimage(L,image);
}

void dt_lua_image_push(lua_State * L,int imgid) {
  // check that id is valid
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    luaL_error(L,"invalid id for image : %d",imgid);
    return;
  }
  sqlite3_finalize(stmt);
  luaA_push(L,dt_lua_image_t,&imgid);
}

typedef enum {
  PATH,
  DUP_INDEX,
  IS_LDR,
  IS_HDR,
  IS_RAW,
  RATING,
  ID,
  CREATOR,
  PUBLISHER,
  TITLE,
  DESCRIPTION,
  RIGHTS,
  LAST_IMAGE_FIELD
} image_fields;
const char *image_fields_name[] = {
  "path",
  "duplicate_index",
  "is_ldr",
  "is_hdr",
  "is_raw",
  "rating",
  "id",
  "creator",
  "publisher",
  "title",
  "description",
  "rights",
  NULL
};

static int image_index(lua_State *L){
  const char* membername = lua_tostring(L, -1);
  const dt_image_t * my_image=checkreadimage(L,-2);
  if(luaA_struct_has_member_name(L,dt_image_t,membername)) {
    const int result = luaA_struct_push_member_name(L, dt_image_t, my_image, membername);
    releasereadimage(L,my_image);
    return result;
  }
  switch(luaL_checkoption(L,-1,NULL,image_fields_name)) {
    case PATH:
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
            "select folder from images, film_rolls where "
            "images.film_id = film_rolls.id and images.id = ?1", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
        } else {
          sqlite3_finalize(stmt);
          releasereadimage(L,my_image);
          return luaL_error(L,"should never happen");
        }
        sqlite3_finalize(stmt);
        break;
      }
    case DUP_INDEX:
      {
        // get duplicate suffix
        int version = 0;
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
            "select count(id) from images where filename in "
            "(select filename from images where id = ?1) and film_id in "
            "(select film_id from images where id = ?1) and id < ?1",
            -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
        if(sqlite3_step(stmt) == SQLITE_ROW)
          version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        lua_pushinteger(L,version);
        break;
      }
    case IS_LDR:
      lua_pushboolean(L,dt_image_is_ldr(my_image));
      break;
    case IS_HDR:
      lua_pushboolean(L,dt_image_is_hdr(my_image));
      break;
    case IS_RAW:
      lua_pushboolean(L,dt_image_is_raw(my_image));
      break;
    case RATING:
      {
        int score = my_image->flags & 0x7;
        if(score >6) score=5;
        if(score ==6) score=-1;

        lua_pushinteger(L,score);
        break;
      }
    case ID:
      lua_pushinteger(L,my_image->height);
      break;
    case CREATOR:
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_CREATOR);
        if(sqlite3_step(stmt) != SQLITE_ROW) {
          lua_pushstring(L,"");
        } else {
          lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        break;

      }
    case PUBLISHER:
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_PUBLISHER);
        if(sqlite3_step(stmt) != SQLITE_ROW) {
          lua_pushstring(L,"");
        } else {
          lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        break;

      }
    case TITLE:
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_TITLE);
        if(sqlite3_step(stmt) != SQLITE_ROW) {
          lua_pushstring(L,"");
        } else {
          lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        break;

      }
    case DESCRIPTION:
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_DESCRIPTION);
        if(sqlite3_step(stmt) != SQLITE_ROW) {
          lua_pushstring(L,"");
        } else {
          lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        break;

      }
    case RIGHTS:
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_RIGHTS);
        if(sqlite3_step(stmt) != SQLITE_ROW) {
          lua_pushstring(L,"");
        } else {
          lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        break;

      }
    default:
      releasereadimage(L,my_image);
      return luaL_error(L,"should never happen %s",lua_tostring(L,-1));

  }
  releasereadimage(L,my_image);
  return 1;
}

static int image_newindex(lua_State *L){
  const char* membername = lua_tostring(L, -2);
  dt_image_t * my_image=checkwriteimage(L,-3);
  if(luaA_struct_has_member_name(L,dt_image_t,membername)) {
    if(luaA_type_has_to_func(luaA_struct_typeof_member_name(L,dt_image_t,membername))) {
      luaA_struct_to_member_name(L, dt_image_t, my_image, membername,-1);
    } else {
      releasewriteimage(L,my_image);
      luaL_error(L,"%s is read only",membername);
    }
    releasewriteimage(L,my_image);
    return 0;
  }
  switch(luaL_checkoption(L,-2,NULL,image_fields_name)) {
    case RATING:
      {
        int my_score = luaL_checkinteger(L,-1);
        if(my_score > 5) {
          releasewriteimage(L,my_image);
          return luaL_error(L,"rating too high : %d",my_score);
        }
        if(my_score == -1) my_score = 6;
        if(my_score < -1) {
          releasewriteimage(L,my_image);
          return luaL_error(L,"rating too low : %d",my_score);
        }
        my_image->flags &= ~0x7;
        my_image->flags |= my_score;
        break;
      }

    case CREATOR:
      dt_metadata_set(my_image->id,"Xmp.dc.creator",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case PUBLISHER:
      dt_metadata_set(my_image->id,"Xmp.dc.publisher",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case TITLE:
      dt_metadata_set(my_image->id,"Xmp.dc.title",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case DESCRIPTION:
      dt_metadata_set(my_image->id,"Xmp.dc.description",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case RIGHTS:
      dt_metadata_set(my_image->id,"Xmp.dc.title",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case PATH:
    case DUP_INDEX:
    case IS_LDR:
    case IS_HDR:
    case IS_RAW:
    case ID:
    default:
      releasewriteimage(L,my_image);
      return luaL_error(L,"unknown index for image : ",lua_tostring(L,-2));

  }
  releasewriteimage(L,my_image);
  return 0;
}

static int image_eq(lua_State*L) {
  dt_lua_image_t imgid1;
  luaA_to(L,dt_lua_image_t,&imgid1,-1);
  dt_lua_image_t imgid2;
  luaA_to(L,dt_lua_image_t,&imgid2,-2);
  lua_pushboolean(L,imgid1==imgid2);
  return 1;
}
static int image_tostring(lua_State *L) {
  const dt_image_t * my_image=checkreadimage(L,-1);
  char image_name[PATH_MAX];
  dt_image_full_path(my_image->id,image_name,PATH_MAX);
  dt_image_path_append_version(my_image->id,image_name,PATH_MAX);
  lua_pushstring(L,image_name);
  releasereadimage(L,my_image);
  return 1;
}
///////////////
// toplevel and common
///////////////

static int image_table(lua_State*L) {
	lua_newtable(L);
	sqlite3_stmt *stmt;
	DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images", -1, &stmt, NULL);
	while(sqlite3_step(stmt) == SQLITE_ROW) {
		int imgid = sqlite3_column_int(stmt,0);
		dt_lua_image_push(L,imgid);
		luaL_ref(L,-2);
	}
	sqlite3_finalize(stmt);
	return 1;
}

int dt_lua_init_image(lua_State * L) {
  luaA_struct(L,dt_image_t);
  luaA_struct_member(L,dt_image_t,exif_exposure,float);
  luaA_struct_member(L,dt_image_t,exif_aperture,float);
  luaA_struct_member(L,dt_image_t,exif_iso,float);
  luaA_struct_member(L,dt_image_t,exif_focal_length,float);
  luaA_struct_member(L,dt_image_t,exif_focus_distance,float);
  luaA_struct_member(L,dt_image_t,exif_crop,float);
  luaA_struct_member(L,dt_image_t,exif_maker,char_32);
  luaA_struct_member(L,dt_image_t,exif_model,char_32);
  luaA_struct_member(L,dt_image_t,exif_lens,char_52);
  luaA_struct_member(L,dt_image_t,exif_datetime_taken,char_20);
  luaA_struct_member(L,dt_image_t,filename,const char_filename_length);
  luaA_struct_member(L,dt_image_t,width,const int32_t);
  luaA_struct_member(L,dt_image_t,height,const int32_t);
  luaA_struct_member(L,dt_image_t,longitude,double);
  luaA_struct_member(L,dt_image_t,latitude,double);

  dt_lua_init_type(L,dt_lua_image_t);
  dt_lua_register_type_callback_list(L,dt_lua_image_t,image_index,image_newindex,image_fields_name);
  dt_lua_register_type_callback_type(L,dt_lua_image_t,image_index,image_newindex,dt_image_t);
  luaL_getmetatable(L,"dt_lua_image_t");
  lua_pushcfunction(L,image_eq);
	lua_setfield(L,-2,"__eq");
  lua_pushcfunction(L,image_tostring);
	lua_setfield(L,-2,"__tostring");
  lua_pop(L,1);

  /* darktable.images() */
  dt_lua_push_darktable_lib(L);
  lua_pushcfunction(L,image_table);
  lua_setfield(L,-2,"images");
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
