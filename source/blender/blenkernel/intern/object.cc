/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include <cmath>
#include <cstdio>
#include <cstring>

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_constraint_types.h"
#include "DNA_defaults.h"
#include "DNA_dynamicpaint_types.h"
#include "DNA_effect_types.h"
#include "DNA_fluid_types.h"
#include "DNA_gpencil_legacy_types.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_key_types.h"
#include "DNA_lattice_types.h"
#include "DNA_light_types.h"
#include "DNA_lightprobe_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_meta_types.h"
#include "DNA_movieclip_types.h"
#include "DNA_nla_types.h"
#include "DNA_object_fluidsim_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_sequence_types.h"
#include "DNA_shader_fx_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_world_types.h"

#include "BLI_blenlib.h"
#include "BLI_bounds.hh"
#include "BLI_kdtree.h"
#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_math_vector_types.hh"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_DerivedMesh.h"
#include "BKE_action.h"
#include "BKE_anim_data.h"
#include "BKE_anim_path.h"
#include "BKE_anim_visualization.h"
#include "BKE_animsys.h"
#include "BKE_armature.h"
#include "BKE_asset.h"
#include "BKE_bpath.h"
#include "BKE_camera.h"
#include "BKE_collection.h"
#include "BKE_constraint.h"
#include "BKE_crazyspace.h"
#include "BKE_curve.h"
#include "BKE_curves.hh"
#include "BKE_deform.h"
#include "BKE_displist.h"
#include "BKE_duplilist.h"
#include "BKE_editmesh.h"
#include "BKE_editmesh_cache.h"
#include "BKE_effect.h"
#include "BKE_fcurve.h"
#include "BKE_fcurve_driver.h"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_global.h"
#include "BKE_gpencil_geom_legacy.h"
#include "BKE_gpencil_legacy.h"
#include "BKE_gpencil_modifier_legacy.h"
#include "BKE_grease_pencil.hh"
#include "BKE_icons.h"
#include "BKE_idprop.h"
#include "BKE_idtype.h"
#include "BKE_image.h"
#include "BKE_key.h"
#include "BKE_lattice.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_lib_remap.h"
#include "BKE_light.h"
#include "BKE_lightprobe.h"
#include "BKE_linestyle.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_mball.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
#include "BKE_multires.h"
#include "BKE_node.hh"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_particle.h"
#include "BKE_pointcache.h"
#include "BKE_pointcloud.h"
#include "BKE_pose_backup.h"
#include "BKE_rigidbody.h"
#include "BKE_scene.h"
#include "BKE_shader_fx.h"
#include "BKE_softbody.h"
#include "BKE_speaker.h"
#include "BKE_subdiv_ccg.h"
#include "BKE_subsurf.h"
#include "BKE_vfont.h"
#include "BKE_volume.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "DRW_engine.h"

#include "BLO_read_write.h"
#include "BLO_readfile.h"

#include "SEQ_sequencer.h"

#ifdef WITH_PYTHON
#  include "BPY_extern.h"
#endif

#include "CCGSubSurf.h"
#include "atomic_ops.h"

using blender::float3;
using blender::MutableSpan;
using blender::Span;

static CLG_LogRef LOG = {"bke.object"};

/**
 * NOTE(@sergey): Vertex parent modifies original #BMesh which is not safe for threading.
 * Ideally such a modification should be handled as a separate DAG update
 * callback for mesh data-block, but for until it is actually supported use
 * simpler solution with a mutex lock.
 */
#define VPARENT_THREADING_HACK

#ifdef VPARENT_THREADING_HACK
static ThreadMutex vparent_lock = BLI_MUTEX_INITIALIZER;
#endif

static void copy_object_pose(Object *obn, const Object *ob, const int flag);

static void object_init_data(ID *id)
{
  Object *ob = (Object *)id;
  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(ob, id));

  MEMCPY_STRUCT_AFTER(ob, DNA_struct_default_get(Object), id);

  ob->type = OB_EMPTY;

  ob->trackflag = OB_POSY;
  ob->upflag = OB_POSZ;

  /* Animation Visualization defaults */
  animviz_settings_init(&ob->avs);
}

static void object_copy_data(Main *bmain, ID *id_dst, const ID *id_src, const int flag)
{
  Object *ob_dst = (Object *)id_dst;
  const Object *ob_src = (const Object *)id_src;

  /* Do not copy runtime data. */
  BKE_object_runtime_reset_on_copy(ob_dst, flag);

  /* We never handle user-count here for own data. */
  const int flag_subdata = flag | LIB_ID_CREATE_NO_USER_REFCOUNT;

  if (ob_src->totcol) {
    ob_dst->mat = (Material **)MEM_dupallocN(ob_src->mat);
    ob_dst->matbits = (char *)MEM_dupallocN(ob_src->matbits);
    ob_dst->totcol = ob_src->totcol;
  }
  else if (ob_dst->mat != nullptr || ob_dst->matbits != nullptr) {
    /* This shall not be needed, but better be safe than sorry. */
    BLI_assert_msg(
        0, "Object copy: non-nullptr material pointers with zero counter, should not happen.");
    ob_dst->mat = nullptr;
    ob_dst->matbits = nullptr;
  }

  if (ob_src->iuser) {
    ob_dst->iuser = (ImageUser *)MEM_dupallocN(ob_src->iuser);
  }

  if (ob_src->runtime.bb) {
    ob_dst->runtime.bb = (BoundBox *)MEM_dupallocN(ob_src->runtime.bb);
  }

  BLI_listbase_clear(&ob_dst->shader_fx);
  LISTBASE_FOREACH (ShaderFxData *, fx, &ob_src->shader_fx) {
    ShaderFxData *nfx = BKE_shaderfx_new(fx->type);
    STRNCPY(nfx->name, fx->name);
    BKE_shaderfx_copydata_ex(fx, nfx, flag_subdata);
    BLI_addtail(&ob_dst->shader_fx, nfx);
  }

  if (ob_src->pose) {
    copy_object_pose(ob_dst, ob_src, flag_subdata);
    /* backwards compat... non-armatures can get poses in older files? */
    if (ob_src->type == OB_ARMATURE) {
      const bool do_pose_id_user = (flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0;
      BKE_pose_rebuild(bmain, ob_dst, (bArmature *)ob_dst->data, do_pose_id_user);
    }
  }

  BKE_constraints_copy_ex(&ob_dst->constraints, &ob_src->constraints, flag_subdata, true);

  ob_dst->mode = ob_dst->type != OB_GPENCIL_LEGACY ? OB_MODE_OBJECT : ob_dst->mode;
  ob_dst->sculpt = nullptr;

  if (ob_src->pd) {
    ob_dst->pd = (PartDeflect *)MEM_dupallocN(ob_src->pd);
    if (ob_dst->pd->rng) {
      ob_dst->pd->rng = (RNG *)MEM_dupallocN(ob_src->pd->rng);
    }
  }
  BKE_rigidbody_object_copy(bmain, ob_dst, ob_src, flag_subdata);

  BLI_listbase_clear(&ob_dst->modifiers);
  BLI_listbase_clear(&ob_dst->greasepencil_modifiers);
  /* NOTE: Also takes care of soft-body and particle systems copying. */
  BKE_object_modifier_stack_copy(ob_dst, ob_src, true, flag_subdata);

  BLI_listbase_clear((ListBase *)&ob_dst->drawdata);
  BLI_listbase_clear(&ob_dst->pc_ids);

  ob_dst->avs = ob_src->avs;
  ob_dst->mpath = animviz_copy_motionpath(ob_src->mpath);

  /* Do not copy object's preview
   * (mostly due to the fact renderers create temp copy of objects). */
  if ((flag & LIB_ID_COPY_NO_PREVIEW) == 0 && false) { /* XXX TODO: temp hack. */
    BKE_previewimg_id_copy(&ob_dst->id, &ob_src->id);
  }
  else {
    ob_dst->preview = nullptr;
  }

  if (ob_src->lightgroup) {
    ob_dst->lightgroup = (LightgroupMembership *)MEM_dupallocN(ob_src->lightgroup);
  }
  if (ob_src->light_linking) {
    ob_dst->light_linking = (LightLinking *)MEM_dupallocN(ob_src->light_linking);
  }

  if ((flag & LIB_ID_COPY_SET_COPIED_ON_WRITE) != 0) {
    if (ob_src->lightprobe_cache) {
      /* Reference the original object data. */
      ob_dst->lightprobe_cache = (LightProbeObjectCache *)MEM_dupallocN(ob_src->lightprobe_cache);
      ob_dst->lightprobe_cache->shared = true;
    }
  }
  else {
    /* Do not copy lightprobe's cache. */
    ob_dst->lightprobe_cache = nullptr;
  }
}

static void object_free_data(ID *id)
{
  Object *ob = (Object *)id;

  DRW_drawdata_free((ID *)ob);

  /* BKE_<id>_free shall never touch to ID->us. Never ever. */
  BKE_object_free_modifiers(ob, LIB_ID_CREATE_NO_USER_REFCOUNT);
  BKE_object_free_shaderfx(ob, LIB_ID_CREATE_NO_USER_REFCOUNT);

  MEM_SAFE_FREE(ob->mat);
  MEM_SAFE_FREE(ob->matbits);
  MEM_SAFE_FREE(ob->iuser);
  MEM_SAFE_FREE(ob->runtime.bb);

  if (ob->pose) {
    BKE_pose_free_ex(ob->pose, false);
    ob->pose = nullptr;
  }
  if (ob->mpath) {
    animviz_free_motionpath(ob->mpath);
    ob->mpath = nullptr;
  }

  BKE_constraints_free_ex(&ob->constraints, false);

  BKE_partdeflect_free(ob->pd);
  BKE_rigidbody_free_object(ob, nullptr);
  BKE_rigidbody_free_constraint(ob);

  sbFree(ob);

  BKE_sculptsession_free(ob);

  BLI_freelistN(&ob->pc_ids);

  /* Free runtime curves data. */
  if (ob->runtime.curve_cache) {
    BKE_curve_bevelList_free(&ob->runtime.curve_cache->bev);
    if (ob->runtime.curve_cache->anim_path_accum_length) {
      MEM_freeN((void *)ob->runtime.curve_cache->anim_path_accum_length);
    }
    MEM_freeN(ob->runtime.curve_cache);
    ob->runtime.curve_cache = nullptr;
  }

  BKE_previewimg_free(&ob->preview);

  MEM_SAFE_FREE(ob->lightgroup);
  MEM_SAFE_FREE(ob->light_linking);

  BKE_lightprobe_cache_free(ob);
}

static void library_foreach_modifiersForeachIDLink(void *user_data,
                                                   Object * /*object*/,
                                                   ID **id_pointer,
                                                   int cb_flag)
{
  LibraryForeachIDData *data = (LibraryForeachIDData *)user_data;
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data, BKE_lib_query_foreachid_process(data, id_pointer, cb_flag));
}

static void library_foreach_gpencil_modifiersForeachIDLink(void *user_data,
                                                           Object * /*object*/,
                                                           ID **id_pointer,
                                                           int cb_flag)
{
  LibraryForeachIDData *data = (LibraryForeachIDData *)user_data;
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data, BKE_lib_query_foreachid_process(data, id_pointer, cb_flag));
}

static void library_foreach_shaderfxForeachIDLink(void *user_data,
                                                  Object * /*object*/,
                                                  ID **id_pointer,
                                                  int cb_flag)
{
  LibraryForeachIDData *data = (LibraryForeachIDData *)user_data;
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data, BKE_lib_query_foreachid_process(data, id_pointer, cb_flag));
}

static void library_foreach_constraintObjectLooper(bConstraint * /*con*/,
                                                   ID **id_pointer,
                                                   bool is_reference,
                                                   void *user_data)
{
  LibraryForeachIDData *data = (LibraryForeachIDData *)user_data;
  const int cb_flag = is_reference ? IDWALK_CB_USER : IDWALK_CB_NOP;
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data, BKE_lib_query_foreachid_process(data, id_pointer, cb_flag));
}

static void library_foreach_particlesystemsObjectLooper(ParticleSystem * /*psys*/,
                                                        ID **id_pointer,
                                                        void *user_data,
                                                        int cb_flag)
{
  LibraryForeachIDData *data = (LibraryForeachIDData *)user_data;
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data, BKE_lib_query_foreachid_process(data, id_pointer, cb_flag));
}

static void object_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Object *object = (Object *)id;

  /* object data special case */
  if (object->type == OB_EMPTY) {
    /* empty can have nullptr or Image */
    BKE_LIB_FOREACHID_PROCESS_ID(data, object->data, IDWALK_CB_USER);
  }
  else {
    /* when set, this can't be nullptr */
    if (object->data) {
      BKE_LIB_FOREACHID_PROCESS_ID(data, object->data, IDWALK_CB_USER | IDWALK_CB_NEVER_NULL);
    }
  }

  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, object->parent, IDWALK_CB_NEVER_SELF);
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, object->track, IDWALK_CB_NEVER_SELF);

  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, object->poselib, IDWALK_CB_USER);

  for (int i = 0; i < object->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, object->mat[i], IDWALK_CB_USER);
  }

  /* Note that `ob->gpd` is deprecated, so no need to handle it here. */
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, object->instance_collection, IDWALK_CB_USER);

  if (object->pd) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, object->pd->tex, IDWALK_CB_USER);
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, object->pd->f_source, IDWALK_CB_NOP);
  }
  /* Note that ob->effect is deprecated, so no need to handle it here. */

  if (object->pose) {
    LISTBASE_FOREACH (bPoseChannel *, pchan, &object->pose->chanbase) {
      BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
          data,
          IDP_foreach_property(pchan->prop,
                               IDP_TYPE_FILTER_ID,
                               BKE_lib_query_idpropertiesForeachIDLink_callback,
                               data));

      BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, pchan->custom, IDWALK_CB_USER);
      BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
          data,
          BKE_constraints_id_loop(
              &pchan->constraints, library_foreach_constraintObjectLooper, data));
    }
  }

  if (object->rigidbody_constraint) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(
        data, object->rigidbody_constraint->ob1, IDWALK_CB_NEVER_SELF);
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(
        data, object->rigidbody_constraint->ob2, IDWALK_CB_NEVER_SELF);
  }

  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data, BKE_modifiers_foreach_ID_link(object, library_foreach_modifiersForeachIDLink, data));
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data,
      BKE_gpencil_modifiers_foreach_ID_link(
          object, library_foreach_gpencil_modifiersForeachIDLink, data));
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data,
      BKE_constraints_id_loop(&object->constraints, library_foreach_constraintObjectLooper, data));
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
      data, BKE_shaderfx_foreach_ID_link(object, library_foreach_shaderfxForeachIDLink, data));

  LISTBASE_FOREACH (ParticleSystem *, psys, &object->particlesystem) {
    BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
        data, BKE_particlesystem_id_loop(psys, library_foreach_particlesystemsObjectLooper, data));
  }

  if (object->soft) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, object->soft->collision_group, IDWALK_CB_NOP);

    if (object->soft->effector_weights) {
      BKE_LIB_FOREACHID_PROCESS_IDSUPER(
          data, object->soft->effector_weights->group, IDWALK_CB_USER);
    }
  }

  if (object->light_linking) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(
        data, object->light_linking->receiver_collection, IDWALK_CB_USER);
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(
        data, object->light_linking->blocker_collection, IDWALK_CB_USER);
  }
}

static void object_foreach_path_pointcache(ListBase *ptcache_list,
                                           BPathForeachPathData *bpath_data)
{
  for (PointCache *cache = (PointCache *)ptcache_list->first; cache != nullptr;
       cache = cache->next) {
    if (cache->flag & PTCACHE_DISK_CACHE) {
      BKE_bpath_foreach_path_fixed_process(bpath_data, cache->path, sizeof(cache->path));
    }
  }
}

static void object_foreach_path(ID *id, BPathForeachPathData *bpath_data)
{
  Object *ob = reinterpret_cast<Object *>(id);

  LISTBASE_FOREACH (ModifierData *, md, &ob->modifiers) {
    /* TODO: Move that to #ModifierTypeInfo. */
    switch (md->type) {
      case eModifierType_Fluidsim: {
        FluidsimModifierData *fluidmd = reinterpret_cast<FluidsimModifierData *>(md);
        if (fluidmd->fss) {
          BKE_bpath_foreach_path_fixed_process(
              bpath_data, fluidmd->fss->surfdataPath, sizeof(fluidmd->fss->surfdataPath));
        }
        break;
      }
      case eModifierType_Fluid: {
        FluidModifierData *fmd = reinterpret_cast<FluidModifierData *>(md);
        if (fmd->type & MOD_FLUID_TYPE_DOMAIN && fmd->domain) {
          BKE_bpath_foreach_path_fixed_process(
              bpath_data, fmd->domain->cache_directory, sizeof(fmd->domain->cache_directory));
        }
        break;
      }
      case eModifierType_Cloth: {
        ClothModifierData *clmd = reinterpret_cast<ClothModifierData *>(md);
        object_foreach_path_pointcache(&clmd->ptcaches, bpath_data);
        break;
      }
      case eModifierType_Ocean: {
        OceanModifierData *omd = reinterpret_cast<OceanModifierData *>(md);
        BKE_bpath_foreach_path_fixed_process(bpath_data, omd->cachepath, sizeof(omd->cachepath));
        break;
      }
      case eModifierType_MeshCache: {
        MeshCacheModifierData *mcmd = reinterpret_cast<MeshCacheModifierData *>(md);
        BKE_bpath_foreach_path_fixed_process(bpath_data, mcmd->filepath, sizeof(mcmd->filepath));
        break;
      }
      default:
        break;
    }
  }

  if (ob->soft != nullptr) {
    object_foreach_path_pointcache(&ob->soft->shared->ptcaches, bpath_data);
  }

  LISTBASE_FOREACH (ParticleSystem *, psys, &ob->particlesystem) {
    object_foreach_path_pointcache(&psys->ptcaches, bpath_data);
  }
}

static void object_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Object *ob = (Object *)id;

  const bool is_undo = BLO_write_is_undo(writer);

  /* Clean up, important in undo case to reduce false detection of changed data-blocks. */
  BKE_object_runtime_reset(ob);

  if (is_undo) {
    /* For undo we stay in object mode during undo presses, so keep edit-mode disabled on save as
     * well, can help reducing false detection of changed data-blocks. */
    ob->mode &= ~OB_MODE_EDIT;
  }

  /* write LibData */
  BLO_write_id_struct(writer, Object, id_address, &ob->id);
  BKE_id_blend_write(writer, &ob->id);

  if (ob->adt) {
    BKE_animdata_blend_write(writer, ob->adt);
  }

  /* direct data */
  BLO_write_pointer_array(writer, ob->totcol, ob->mat);
  BLO_write_raw(writer, sizeof(char) * ob->totcol, ob->matbits);

  bArmature *arm = nullptr;
  if (ob->type == OB_ARMATURE) {
    arm = (bArmature *)ob->data;
  }

  BKE_pose_blend_write(writer, ob->pose, arm);
  BKE_constraint_blend_write(writer, &ob->constraints);
  animviz_motionpath_blend_write(writer, ob->mpath);

  BLO_write_struct(writer, PartDeflect, ob->pd);
  if (ob->soft) {
    /* Set deprecated pointers to prevent crashes of older Blenders */
    ob->soft->pointcache = ob->soft->shared->pointcache;
    ob->soft->ptcaches = ob->soft->shared->ptcaches;
    BLO_write_struct(writer, SoftBody, ob->soft);
    BLO_write_struct(writer, SoftBody_Shared, ob->soft->shared);
    BKE_ptcache_blend_write(writer, &(ob->soft->shared->ptcaches));
    BLO_write_struct(writer, EffectorWeights, ob->soft->effector_weights);
  }

  if (ob->rigidbody_object) {
    /* TODO: if any extra data is added to handle duplis, will need separate function then */
    BLO_write_struct(writer, RigidBodyOb, ob->rigidbody_object);
  }
  if (ob->rigidbody_constraint) {
    BLO_write_struct(writer, RigidBodyCon, ob->rigidbody_constraint);
  }

  if (ob->type == OB_EMPTY && ob->empty_drawtype == OB_EMPTY_IMAGE) {
    BLO_write_struct(writer, ImageUser, ob->iuser);
  }

  BKE_particle_system_blend_write(writer, &ob->particlesystem);
  BKE_modifier_blend_write(writer, &ob->id, &ob->modifiers);
  BKE_gpencil_modifier_blend_write(writer, &ob->greasepencil_modifiers);
  BKE_shaderfx_blend_write(writer, &ob->shader_fx);

  BLO_write_struct_list(writer, LinkData, &ob->pc_ids);

  BKE_previewimg_blend_write(writer, ob->preview);

  if (ob->lightgroup) {
    BLO_write_struct(writer, LightgroupMembership, ob->lightgroup);
  }
  if (ob->light_linking) {
    BLO_write_struct(writer, LightLinking, ob->light_linking);
  }

  if (ob->lightprobe_cache) {
    BLO_write_struct(writer, LightProbeObjectCache, ob->lightprobe_cache);
    BKE_lightprobe_cache_blend_write(writer, ob->lightprobe_cache);
  }
}

/* XXX deprecated - old animation system */
static void direct_link_nlastrips(BlendDataReader *reader, ListBase *strips)
{
  BLO_read_list(reader, strips);

  LISTBASE_FOREACH (bActionStrip *, strip, strips) {
    BLO_read_list(reader, &strip->modifiers);
  }
}

static void object_blend_read_data(BlendDataReader *reader, ID *id)
{
  Object *ob = (Object *)id;

  PartEff *paf;

  /* XXX This should not be needed - but seems like it can happen in some cases,
   * so for now play safe. */
  ob->proxy_from = nullptr;

  const bool is_undo = BLO_read_data_is_undo(reader);
  if (ob->id.tag & (LIB_TAG_EXTERN | LIB_TAG_INDIRECT)) {
    /* Do not allow any non-object mode for linked data.
     * See #34776, #42780, #81027 for more information. */
    ob->mode &= ~OB_MODE_ALL_MODE_DATA;
  }
  else if (is_undo) {
    /* For undo we want to stay in object mode during undo presses, so keep some edit modes
     * disabled.
     * TODO: Check if we should not disable more edit modes here? */
    ob->mode &= ~(OB_MODE_EDIT | OB_MODE_PARTICLE_EDIT);
  }

  BLO_read_data_address(reader, &ob->adt);
  BKE_animdata_blend_read_data(reader, ob->adt);

  BLO_read_data_address(reader, &ob->pose);
  BKE_pose_blend_read_data(reader, ob->pose);

  BLO_read_data_address(reader, &ob->mpath);
  if (ob->mpath) {
    animviz_motionpath_blend_read_data(reader, ob->mpath);
  }

  /* Only for versioning, vertex group names are now stored on object data. */
  BLO_read_list(reader, &ob->defbase);

  /* XXX deprecated - old animation system <<< */
  direct_link_nlastrips(reader, &ob->nlastrips);
  BLO_read_list(reader, &ob->constraintChannels);
  /* >>> XXX deprecated - old animation system */

  BLO_read_pointer_array(reader, (void **)&ob->mat);
  BLO_read_data_address(reader, &ob->matbits);

  /* do it here, below old data gets converted */
  BKE_modifier_blend_read_data(reader, &ob->modifiers, ob);
  BKE_gpencil_modifier_blend_read_data(reader, &ob->greasepencil_modifiers);
  BKE_shaderfx_blend_read_data(reader, &ob->shader_fx);

  BLO_read_list(reader, &ob->effect);
  paf = (PartEff *)ob->effect.first;
  while (paf) {
    if (paf->type == EFF_PARTICLE) {
      paf->keys = nullptr;
    }
    if (paf->type == EFF_WAVE) {
      WaveEff *wav = (WaveEff *)paf;
      PartEff *next = paf->next;
      WaveModifierData *wmd = (WaveModifierData *)BKE_modifier_new(eModifierType_Wave);

      wmd->damp = wav->damp;
      wmd->flag = wav->flag;
      wmd->height = wav->height;
      wmd->lifetime = wav->lifetime;
      wmd->narrow = wav->narrow;
      wmd->speed = wav->speed;
      wmd->startx = wav->startx;
      wmd->starty = wav->startx;
      wmd->timeoffs = wav->timeoffs;
      wmd->width = wav->width;

      BLI_addtail(&ob->modifiers, wmd);

      BLI_remlink(&ob->effect, paf);
      MEM_freeN(paf);

      paf = next;
      continue;
    }
    if (paf->type == EFF_BUILD) {
      BuildEff *baf = (BuildEff *)paf;
      PartEff *next = paf->next;
      BuildModifierData *bmd = (BuildModifierData *)BKE_modifier_new(eModifierType_Build);

      bmd->start = baf->sfra;
      bmd->length = baf->len;
      bmd->randomize = 0;
      bmd->seed = 1;

      BLI_addtail(&ob->modifiers, bmd);

      BLI_remlink(&ob->effect, paf);
      MEM_freeN(paf);

      paf = next;
      continue;
    }
    paf = paf->next;
  }

  BLO_read_data_address(reader, &ob->pd);
  BKE_particle_partdeflect_blend_read_data(reader, ob->pd);
  BLO_read_data_address(reader, &ob->soft);
  if (ob->soft) {
    SoftBody *sb = ob->soft;

    sb->bpoint = nullptr; /* init pointers so it gets rebuilt nicely */
    sb->bspring = nullptr;
    sb->scratch = nullptr;
    /* although not used anymore */
    /* still have to be loaded to be compatible with old files */
    BLO_read_pointer_array(reader, (void **)&sb->keys);
    if (sb->keys) {
      for (int a = 0; a < sb->totkey; a++) {
        BLO_read_data_address(reader, &sb->keys[a]);
      }
    }

    BLO_read_data_address(reader, &sb->effector_weights);
    if (!sb->effector_weights) {
      sb->effector_weights = BKE_effector_add_weights(nullptr);
    }

    BLO_read_data_address(reader, &sb->shared);
    if (sb->shared == nullptr) {
      /* Link deprecated caches if they exist, so we can use them for versioning.
       * We should only do this when `sb->shared == nullptr`, because those pointers
       * are always set (for compatibility with older Blenders). We mustn't link
       * the same point-cache twice. */
      BKE_ptcache_blend_read_data(reader, &sb->ptcaches, &sb->pointcache, false);
    }
    else {
      /* link caches */
      BKE_ptcache_blend_read_data(reader, &sb->shared->ptcaches, &sb->shared->pointcache, false);
    }
  }
  BLO_read_data_address(reader, &ob->fluidsimSettings); /* NT */

  BLO_read_data_address(reader, &ob->rigidbody_object);
  if (ob->rigidbody_object) {
    RigidBodyOb *rbo = ob->rigidbody_object;
    /* Allocate runtime-only struct */
    rbo->shared = (RigidBodyOb_Shared *)MEM_callocN(sizeof(*rbo->shared), "RigidBodyObShared");
  }
  BLO_read_data_address(reader, &ob->rigidbody_constraint);
  if (ob->rigidbody_constraint) {
    ob->rigidbody_constraint->physics_constraint = nullptr;
  }

  BLO_read_list(reader, &ob->particlesystem);
  BKE_particle_system_blend_read_data(reader, &ob->particlesystem);

  BKE_constraint_blend_read_data(reader, &ob->constraints);

  BLO_read_list(reader, &ob->hooks);
  while (ob->hooks.first) {
    ObHook *hook = (ObHook *)ob->hooks.first;
    HookModifierData *hmd = (HookModifierData *)BKE_modifier_new(eModifierType_Hook);

    BLO_read_int32_array(reader, hook->totindex, &hook->indexar);

    /* Do conversion here because if we have loaded
     * a hook we need to make sure it gets converted
     * and freed, regardless of version.
     */
    copy_v3_v3(hmd->cent, hook->cent);
    hmd->falloff = hook->falloff;
    hmd->force = hook->force;
    hmd->indexar = hook->indexar;
    hmd->object = hook->parent;
    memcpy(hmd->parentinv, hook->parentinv, sizeof(hmd->parentinv));
    hmd->indexar_num = hook->totindex;

    BLI_addhead(&ob->modifiers, hmd);
    BLI_remlink(&ob->hooks, hook);

    BKE_modifier_unique_name(&ob->modifiers, (ModifierData *)hmd);

    MEM_freeN(hook);
  }

  BLO_read_data_address(reader, &ob->iuser);
  if (ob->type == OB_EMPTY && ob->empty_drawtype == OB_EMPTY_IMAGE && !ob->iuser) {
    BKE_object_empty_draw_type_set(ob, ob->empty_drawtype);
  }

  BKE_object_runtime_reset(ob);
  BLO_read_list(reader, &ob->pc_ids);

  /* in case this value changes in future, clamp else we get undefined behavior */
  CLAMP(ob->rotmode, ROT_MODE_MIN, ROT_MODE_MAX);

  if (ob->sculpt) {
    ob->sculpt = nullptr;
    /* Only create data on undo, otherwise rely on editor mode switching. */
    if (BLO_read_data_is_undo(reader) && (ob->mode & OB_MODE_ALL_SCULPT)) {
      BKE_object_sculpt_data_create(ob);
    }
  }

  BLO_read_data_address(reader, &ob->preview);
  BKE_previewimg_blend_read(reader, ob->preview);

  BLO_read_data_address(reader, &ob->lightgroup);
  BLO_read_data_address(reader, &ob->light_linking);

  BLO_read_data_address(reader, &ob->lightprobe_cache);
  if (ob->lightprobe_cache) {
    BKE_lightprobe_cache_blend_read(reader, ob->lightprobe_cache);
  }
}

/* XXX deprecated - old animation system */
static void lib_link_nlastrips(BlendLibReader *reader, ID *id, ListBase *striplist)
{
  LISTBASE_FOREACH (bActionStrip *, strip, striplist) {
    BLO_read_id_address(reader, id, &strip->object);
    BLO_read_id_address(reader, id, &strip->act);
    BLO_read_id_address(reader, id, &strip->ipo);
    LISTBASE_FOREACH (bActionModifier *, amod, &strip->modifiers) {
      BLO_read_id_address(reader, id, &amod->ob);
    }
  }
}

/* XXX deprecated - old animation system */
static void lib_link_constraint_channels(BlendLibReader *reader, ID *id, ListBase *chanbase)
{
  LISTBASE_FOREACH (bConstraintChannel *, chan, chanbase) {
    BLO_read_id_address(reader, id, &chan->ipo);
  }
}

static void object_blend_read_lib(BlendLibReader *reader, ID *id)
{
  Object *ob = (Object *)id;

  Main *bmain = BLO_read_lib_get_main(reader);
  BlendFileReadReport *reports = BLO_read_lib_reports(reader);

  /* XXX deprecated - old animation system <<< */
  BLO_read_id_address(reader, id, &ob->ipo);
  BLO_read_id_address(reader, id, &ob->action);
  /* >>> XXX deprecated - old animation system */

  BLO_read_id_address(reader, id, &ob->parent);
  BLO_read_id_address(reader, id, &ob->track);

  /* XXX deprecated - old pose library, deprecated in Blender 3.5. */
  BLO_read_id_address(reader, id, &ob->poselib);

  /* 2.8x drops support for non-empty dupli instances. */
  if (ob->type == OB_EMPTY) {
    BLO_read_id_address(reader, id, &ob->instance_collection);
  }
  else {
    if (ob->instance_collection != nullptr) {
      ID *new_id = BLO_read_get_new_id_address(
          reader, id, ID_IS_LINKED(id), &ob->instance_collection->id);
      BLO_reportf_wrap(reports,
                       RPT_INFO,
                       TIP_("Non-Empty object '%s' cannot duplicate collection '%s' "
                            "anymore in Blender 2.80, removed instancing"),
                       ob->id.name + 2,
                       new_id->name + 2);
    }
    ob->instance_collection = nullptr;
    ob->transflag &= ~OB_DUPLICOLLECTION;
  }

  BLO_read_id_address(reader, id, &ob->proxy);
  if (ob->proxy) {
    /* paranoia check, actually a proxy_from pointer should never be written... */
    if (!ID_IS_LINKED(ob->proxy)) {
      ob->proxy->proxy_from = nullptr;
      ob->proxy = nullptr;

      if (ob->id.lib) {
        BLO_reportf_wrap(reports,
                         RPT_INFO,
                         TIP_("Proxy lost from object %s lib %s\n"),
                         ob->id.name + 2,
                         ob->id.lib->filepath);
      }
      else {
        BLO_reportf_wrap(
            reports, RPT_INFO, TIP_("Proxy lost from object %s lib <NONE>\n"), ob->id.name + 2);
      }
      reports->count.missing_obproxies++;
    }
    else {
      /* this triggers object_update to always use a copy */
      ob->proxy->proxy_from = ob;
    }
  }
  BLO_read_id_address(reader, id, &ob->proxy_group);

  void *poin = ob->data;
  BLO_read_id_address(reader, id, &ob->data);

  if (ob->data == nullptr && poin != nullptr) {
    ob->type = OB_EMPTY;

    if (ob->pose) {
      /* we can't call #BKE_pose_free() here because of library linking
       * freeing will recurse down into every pose constraints ID pointers
       * which are not always valid, so for now free directly and suffer
       * some leaked memory rather than crashing immediately
       * while bad this _is_ an exceptional case - campbell */
#if 0
      BKE_pose_free(ob->pose);
#else
      MEM_freeN(ob->pose);
#endif
      ob->pose = nullptr;
      ob->mode &= ~OB_MODE_POSE;
    }

    if (ob->id.lib) {
      BLO_reportf_wrap(reports,
                       RPT_INFO,
                       TIP_("Can't find object data of %s lib %s"),
                       ob->id.name + 2,
                       ob->id.lib->filepath);
    }
    else {
      BLO_reportf_wrap(reports, RPT_INFO, TIP_("Object %s lost data"), ob->id.name + 2);
    }
    reports->count.missing_obdata++;
  }
  for (int a = 0; a < ob->totcol; a++) {
    BLO_read_id_address(reader, id, &ob->mat[a]);
  }

  /* When the object is local and the data is library its possible
   * the material list size gets out of sync. #22663. */
  if (ob->data && ob->id.lib != ((ID *)ob->data)->lib) {
    BKE_object_materials_test(bmain, ob, (ID *)ob->data);
  }

  BLO_read_id_address(reader, id, &ob->gpd);

  /* if id.us==0 a new base will be created later on */

  /* WARNING! Also check expand_object(), should reflect the stuff below. */
  BKE_pose_blend_read_lib(reader, ob, ob->pose);
  BKE_constraint_blend_read_lib(reader, &ob->id, &ob->constraints);

  /* XXX deprecated - old animation system <<< */
  lib_link_constraint_channels(reader, &ob->id, &ob->constraintChannels);
  lib_link_nlastrips(reader, &ob->id, &ob->nlastrips);
  /* >>> XXX deprecated - old animation system */

  LISTBASE_FOREACH (PartEff *, paf, &ob->effect) {
    if (paf->type == EFF_PARTICLE) {
      BLO_read_id_address(reader, id, &paf->group);
    }
  }

  {
    FluidsimModifierData *fluidmd = (FluidsimModifierData *)BKE_modifiers_findby_type(
        ob, eModifierType_Fluidsim);

    if (fluidmd && fluidmd->fss) {
      /* XXX: deprecated - old animation system. */
      BLO_read_id_address(reader, id, &fluidmd->fss->ipo);
    }
  }

  {
    FluidModifierData *fmd = (FluidModifierData *)BKE_modifiers_findby_type(ob,
                                                                            eModifierType_Fluid);

    if (fmd && (fmd->type == MOD_FLUID_TYPE_DOMAIN) && fmd->domain) {
      /* Flag for refreshing the simulation after loading */
      fmd->domain->flags |= FLUID_DOMAIN_FILE_LOAD;
    }
    else if (fmd && (fmd->type == MOD_FLUID_TYPE_FLOW) && fmd->flow) {
      fmd->flow->flags &= ~FLUID_FLOW_NEEDS_UPDATE;
    }
    else if (fmd && (fmd->type == MOD_FLUID_TYPE_EFFEC) && fmd->effector) {
      fmd->effector->flags &= ~FLUID_EFFECTOR_NEEDS_UPDATE;
    }
  }

  /* texture field */
  if (ob->pd) {
    BKE_particle_partdeflect_blend_read_lib(reader, &ob->id, ob->pd);
  }

  if (ob->soft) {
    BLO_read_id_address(reader, id, &ob->soft->collision_group);

    BLO_read_id_address(reader, id, &ob->soft->effector_weights->group);
  }

  BKE_particle_system_blend_read_lib(reader, ob, &ob->id, &ob->particlesystem);
  BKE_modifier_blend_read_lib(reader, ob);
  BKE_gpencil_modifier_blend_read_lib(reader, ob);
  BKE_shaderfx_blend_read_lib(reader, ob);

  if (ob->rigidbody_constraint) {
    BLO_read_id_address(reader, id, &ob->rigidbody_constraint->ob1);
    BLO_read_id_address(reader, id, &ob->rigidbody_constraint->ob2);
  }

  if (ob->light_linking) {
    BLO_read_id_address(reader, id, &ob->light_linking->receiver_collection);
    BLO_read_id_address(reader, id, &ob->light_linking->blocker_collection);
  }
}

/* XXX deprecated - old animation system */
static void expand_constraint_channels(BlendExpander *expander, ListBase *chanbase)
{
  LISTBASE_FOREACH (bConstraintChannel *, chan, chanbase) {
    BLO_expand(expander, chan->ipo);
  }
}

static void expand_object_expandModifiers(void *userData,
                                          Object * /*ob*/,
                                          ID **idpoin,
                                          int /*cb_flag*/)
{
  BlendExpander *expander = (BlendExpander *)userData;
  BLO_expand(expander, *idpoin);
}

PartEff *BKE_object_do_version_give_parteff_245(Object *ob)
{
  PartEff *paf;

  paf = (PartEff *)ob->effect.first;
  while (paf) {
    if (paf->type == EFF_PARTICLE) {
      return paf;
    }
    paf = paf->next;
  }
  return nullptr;
}

static void object_blend_read_expand(BlendExpander *expander, ID *id)
{
  Object *ob = (Object *)id;

  BLO_expand(expander, ob->data);

  BLO_expand(expander, ob->parent);

  /* expand_object_expandModifier() */
  if (ob->modifiers.first) {
    BKE_modifiers_foreach_ID_link(ob, expand_object_expandModifiers, expander);
  }

  /* expand_object_expandModifier() */
  if (ob->greasepencil_modifiers.first) {
    BKE_gpencil_modifiers_foreach_ID_link(ob, expand_object_expandModifiers, expander);
  }

  /* expand_object_expandShaderFx() */
  if (ob->shader_fx.first) {
    BKE_shaderfx_foreach_ID_link(ob, expand_object_expandModifiers, expander);
  }

  BKE_pose_blend_read_expand(expander, ob->pose);
  BLO_expand(expander, ob->poselib);
  BKE_constraint_blend_read_expand(expander, &ob->constraints);

  BLO_expand(expander, ob->gpd);

  /* XXX deprecated - old animation system (for version patching only) */
  BLO_expand(expander, ob->ipo);
  BLO_expand(expander, ob->action);

  expand_constraint_channels(expander, &ob->constraintChannels);

  LISTBASE_FOREACH (bActionStrip *, strip, &ob->nlastrips) {
    BLO_expand(expander, strip->object);
    BLO_expand(expander, strip->act);
    BLO_expand(expander, strip->ipo);
  }
  /* XXX deprecated - old animation system (for version patching only) */

  for (int a = 0; a < ob->totcol; a++) {
    BLO_expand(expander, ob->mat[a]);
  }

  PartEff *paf = BKE_object_do_version_give_parteff_245(ob);
  if (paf && paf->group) {
    BLO_expand(expander, paf->group);
  }

  if (ob->instance_collection) {
    BLO_expand(expander, ob->instance_collection);
  }

  if (ob->proxy) {
    BLO_expand(expander, ob->proxy);
  }
  if (ob->proxy_group) {
    BLO_expand(expander, ob->proxy_group);
  }

  LISTBASE_FOREACH (ParticleSystem *, psys, &ob->particlesystem) {
    BLO_expand(expander, psys->part);
  }

  if (ob->pd) {
    BLO_expand(expander, ob->pd->tex);
    BLO_expand(expander, ob->pd->f_source);
  }

  if (ob->soft) {
    BLO_expand(expander, ob->soft->collision_group);

    if (ob->soft->effector_weights) {
      BLO_expand(expander, ob->soft->effector_weights->group);
    }
  }

  if (ob->rigidbody_constraint) {
    BLO_expand(expander, ob->rigidbody_constraint->ob1);
    BLO_expand(expander, ob->rigidbody_constraint->ob2);
  }

  /* Light and shadow linking. */
  if (ob->light_linking) {
    BLO_expand(expander, ob->light_linking->receiver_collection);
    BLO_expand(expander, ob->light_linking->blocker_collection);
  }
}

static void object_lib_override_apply_post(ID *id_dst, ID *id_src)
{
  /* id_dst is the new local override copy of the linked reference data. id_src is the old override
   * data stored on disk, used as source data for override operations. */
  Object *object_dst = (Object *)id_dst;
  Object *object_src = (Object *)id_src;

  ListBase pidlist_dst, pidlist_src;
  BKE_ptcache_ids_from_object(&pidlist_dst, object_dst, nullptr, 0);
  BKE_ptcache_ids_from_object(&pidlist_src, object_src, nullptr, 0);

  /* Problem with point caches is that several status flags (like OUTDATED or BAKED) are read-only
   * at RNA level, and therefore not overridable per-se.
   *
   * This code is a workaround this to check all point-caches from both source and destination
   * objects in parallel, and transfer those flags when it makes sense.
   *
   * This allows to keep baked caches across lib-overrides applies.
   *
   * NOTE: This is fairly hackish and weak, but so is the point-cache system as its whole. A more
   * robust solution would be e.g. to have a specific RNA entry point to deal with such cases
   * (maybe a new flag to allow override code to set values of some read-only properties?).
   */
  PTCacheID *pid_src, *pid_dst;
  for (pid_dst = (PTCacheID *)pidlist_dst.first, pid_src = (PTCacheID *)pidlist_src.first;
       pid_dst != nullptr;
       pid_dst = pid_dst->next, pid_src = (pid_src != nullptr) ? pid_src->next : nullptr)
  {
    /* If pid's do not match, just tag info of caches in dst as dirty and continue. */
    if (pid_src == nullptr) {
      continue;
    }
    if (pid_dst->type != pid_src->type || pid_dst->file_type != pid_src->file_type ||
        pid_dst->default_step != pid_src->default_step || pid_dst->max_step != pid_src->max_step ||
        pid_dst->data_types != pid_src->data_types || pid_dst->info_types != pid_src->info_types)
    {
      LISTBASE_FOREACH (PointCache *, point_cache_src, pid_src->ptcaches) {
        point_cache_src->flag |= PTCACHE_FLAG_INFO_DIRTY;
      }
      continue;
    }

    PointCache *point_cache_dst, *point_cache_src;
    for (point_cache_dst = (PointCache *)pid_dst->ptcaches->first,
        point_cache_src = (PointCache *)pid_src->ptcaches->first;
         point_cache_dst != nullptr;
         point_cache_dst = point_cache_dst->next,
        point_cache_src = (point_cache_src != nullptr) ? point_cache_src->next : nullptr)
    {
      /* Always force updating info about caches of applied lib-overrides. */
      point_cache_dst->flag |= PTCACHE_FLAG_INFO_DIRTY;
      if (point_cache_src == nullptr || !STREQ(point_cache_dst->name, point_cache_src->name)) {
        continue;
      }
      if ((point_cache_src->flag & PTCACHE_BAKED) != 0) {
        point_cache_dst->flag |= PTCACHE_BAKED;
      }
      if ((point_cache_src->flag & PTCACHE_OUTDATED) == 0) {
        point_cache_dst->flag &= ~PTCACHE_OUTDATED;
      }
    }
  }
  BLI_freelistN(&pidlist_dst);
  BLI_freelistN(&pidlist_src);
}

static IDProperty *object_asset_dimensions_property(Object *ob)
{
  float3 dimensions;
  BKE_object_dimensions_get(ob, dimensions);
  if (is_zero_v3(dimensions)) {
    return nullptr;
  }

  IDPropertyTemplate idprop{};
  idprop.array.len = 3;
  idprop.array.type = IDP_FLOAT;

  IDProperty *property = IDP_New(IDP_ARRAY, &idprop, "dimensions");
  memcpy(IDP_Array(property), dimensions, sizeof(dimensions));

  return property;
}

static void object_asset_pre_save(void *asset_ptr, AssetMetaData *asset_data)
{
  Object *ob = (Object *)asset_ptr;
  BLI_assert(GS(ob->id.name) == ID_OB);

  /* Update dimensions hint for the asset. */
  IDProperty *dimensions_prop = object_asset_dimensions_property(ob);
  if (dimensions_prop) {
    BKE_asset_metadata_idprop_ensure(asset_data, dimensions_prop);
  }
}

static AssetTypeInfo AssetType_OB = {
    /*pre_save_fn*/ object_asset_pre_save,
};

IDTypeInfo IDType_ID_OB = {
    /*id_code*/ ID_OB,
    /*id_filter*/ FILTER_ID_OB,
    /*main_listbase_index*/ INDEX_ID_OB,
    /*struct_size*/ sizeof(Object),
    /*name*/ "Object",
    /*name_plural*/ "objects",
    /*translation_context*/ BLT_I18NCONTEXT_ID_OBJECT,
    /*flags*/ 0,
    /*asset_type_info*/ &AssetType_OB,

    /*init_data*/ object_init_data,
    /*copy_data*/ object_copy_data,
    /*free_data*/ object_free_data,
    /*make_local*/ nullptr,
    /*foreach_id*/ object_foreach_id,
    /*foreach_cache*/ nullptr,
    /*foreach_path*/ object_foreach_path,
    /*owner_pointer_get*/ nullptr,

    /*blend_write*/ object_blend_write,
    /*blend_read_data*/ object_blend_read_data,
    /*blend_read_lib*/ object_blend_read_lib,
    /*blend_read_expand*/ object_blend_read_expand,

    /*blend_read_undo_preserve*/ nullptr,

    /*lib_override_apply_post*/ object_lib_override_apply_post,
};

void BKE_object_workob_clear(Object *workob)
{
  *workob = blender::dna::shallow_zero_initialize();

  workob->scale[0] = workob->scale[1] = workob->scale[2] = 1.0f;
  workob->dscale[0] = workob->dscale[1] = workob->dscale[2] = 1.0f;
  workob->rotmode = ROT_MODE_EUL;
}

void BKE_object_free_particlesystems(Object *ob)
{
  ParticleSystem *psys;

  while ((psys = (ParticleSystem *)BLI_pophead(&ob->particlesystem))) {
    psys_free(ob, psys);
  }
}

void BKE_object_free_softbody(Object *ob)
{
  sbFree(ob);
}

void BKE_object_free_curve_cache(Object *ob)
{
  if (ob->runtime.curve_cache) {
    BKE_displist_free(&ob->runtime.curve_cache->disp);
    BKE_curve_bevelList_free(&ob->runtime.curve_cache->bev);
    if (ob->runtime.curve_cache->anim_path_accum_length) {
      MEM_freeN((void *)ob->runtime.curve_cache->anim_path_accum_length);
    }
    BKE_nurbList_free(&ob->runtime.curve_cache->deformed_nurbs);
    MEM_freeN(ob->runtime.curve_cache);
    ob->runtime.curve_cache = nullptr;
  }
}

void BKE_object_free_modifiers(Object *ob, const int flag)
{
  ModifierData *md;
  GpencilModifierData *gp_md;

  while ((md = (ModifierData *)BLI_pophead(&ob->modifiers))) {
    BKE_modifier_free_ex(md, flag);
  }

  while ((gp_md = (GpencilModifierData *)BLI_pophead(&ob->greasepencil_modifiers))) {
    BKE_gpencil_modifier_free_ex(gp_md, flag);
  }
  /* Particle modifiers were freed, so free the particle-systems as well. */
  BKE_object_free_particlesystems(ob);

  /* Same for soft-body */
  BKE_object_free_softbody(ob);

  /* modifiers may have stored data in the DM cache */
  BKE_object_free_derived_caches(ob);
}

void BKE_object_free_shaderfx(Object *ob, const int flag)
{
  ShaderFxData *fx;

  while ((fx = (ShaderFxData *)BLI_pophead(&ob->shader_fx))) {
    BKE_shaderfx_free_ex(fx, flag);
  }
}

void BKE_object_modifier_hook_reset(Object *ob, HookModifierData *hmd)
{
  /* reset functionality */
  if (hmd->object) {
    bPoseChannel *pchan = BKE_pose_channel_find_name(hmd->object->pose, hmd->subtarget);

    if (hmd->subtarget[0] && pchan) {
      float imat[4][4], mat[4][4];

      /* Calculate the world-space matrix for the pose-channel target first,
       * then carry on as usual. */
      mul_m4_m4m4(mat, hmd->object->object_to_world, pchan->pose_mat);

      invert_m4_m4(imat, mat);
      mul_m4_m4m4(hmd->parentinv, imat, ob->object_to_world);
    }
    else {
      invert_m4_m4(hmd->object->world_to_object, hmd->object->object_to_world);
      mul_m4_m4m4(hmd->parentinv, hmd->object->world_to_object, ob->object_to_world);
    }
  }
}

void BKE_object_modifier_gpencil_hook_reset(Object *ob, HookGpencilModifierData *hmd)
{
  if (hmd->object == nullptr) {
    return;
  }
  /* reset functionality */
  bPoseChannel *pchan = BKE_pose_channel_find_name(hmd->object->pose, hmd->subtarget);

  if (hmd->subtarget[0] && pchan) {
    float imat[4][4], mat[4][4];

    /* Calculate the world-space matrix for the pose-channel target first,
     * then carry on as usual. */
    mul_m4_m4m4(mat, hmd->object->object_to_world, pchan->pose_mat);

    invert_m4_m4(imat, mat);
    mul_m4_m4m4(hmd->parentinv, imat, ob->object_to_world);
  }
  else {
    invert_m4_m4(hmd->object->world_to_object, hmd->object->object_to_world);
    mul_m4_m4m4(hmd->parentinv, hmd->object->world_to_object, ob->object_to_world);
  }
}

void BKE_object_modifier_set_active(Object *ob, ModifierData *md)
{
  LISTBASE_FOREACH (ModifierData *, md_iter, &ob->modifiers) {
    md_iter->flag &= ~eModifierFlag_Active;
  }

  if (md != nullptr) {
    BLI_assert(BLI_findindex(&ob->modifiers, md) != -1);
    md->flag |= eModifierFlag_Active;
  }
}

ModifierData *BKE_object_active_modifier(const Object *ob)
{
  /* In debug mode, check for only one active modifier. */
#ifndef NDEBUG
  int active_count = 0;
  LISTBASE_FOREACH (ModifierData *, md, &ob->modifiers) {
    if (md->flag & eModifierFlag_Active) {
      active_count++;
    }
  }
  BLI_assert(ELEM(active_count, 0, 1));
#endif

  LISTBASE_FOREACH (ModifierData *, md, &ob->modifiers) {
    if (md->flag & eModifierFlag_Active) {
      return md;
    }
  }

  return nullptr;
}

bool BKE_object_supports_modifiers(const Object *ob)
{
  return ELEM(ob->type,
              OB_MESH,
              OB_CURVES,
              OB_CURVES_LEGACY,
              OB_SURF,
              OB_FONT,
              OB_LATTICE,
              OB_POINTCLOUD,
              OB_VOLUME,
              OB_GREASE_PENCIL);
}

bool BKE_object_support_modifier_type_check(const Object *ob, int modifier_type)
{
  const ModifierTypeInfo *mti = BKE_modifier_get_info((ModifierType)modifier_type);

  /* Surface and lattice objects don't output geometry sets. */
  if (mti->modifyGeometrySet != nullptr && ELEM(ob->type, OB_SURF, OB_LATTICE)) {
    return false;
  }

  if (ELEM(ob->type, OB_POINTCLOUD, OB_CURVES)) {
    return modifier_type == eModifierType_Nodes;
  }
  if (ob->type == OB_VOLUME) {
    return mti->modifyGeometrySet != nullptr;
  }
  if (ELEM(ob->type, OB_MESH, OB_CURVES_LEGACY, OB_SURF, OB_FONT, OB_LATTICE)) {
    if (ob->type == OB_LATTICE && (mti->flags & eModifierTypeFlag_AcceptsVertexCosOnly) == 0) {
      return false;
    }

    if (!((mti->flags & eModifierTypeFlag_AcceptsCVs) ||
          (ob->type == OB_MESH && (mti->flags & eModifierTypeFlag_AcceptsMesh))))
    {
      return false;
    }

    return true;
  }

  return false;
}

static bool object_modifier_type_copy_check(ModifierType md_type)
{
  return !ELEM(md_type, eModifierType_Hook, eModifierType_Collision);
}

/**
 * Find a `psys` matching given `psys_src` in `ob_dst`
 * (i.e. sharing the same #ParticleSettings ID), or add one, and return valid `psys` from `ob_dst`.
 *
 * \note Order handling is fairly weak here. This code assumes that it is called **before** the
 * modifier using the `psys` is actually copied, and that this copied modifier will be added at the
 * end of the stack. That way we can be sure that the particle modifier will be before the one
 * using its particle system in the stack.
 */
static ParticleSystem *object_copy_modifier_particle_system_ensure(Main *bmain,
                                                                   Scene *scene,
                                                                   Object *ob_dst,
                                                                   ParticleSystem *psys_src)
{
  ParticleSystem *psys_dst = nullptr;

  /* Check if a particle system with the same particle settings
   * already exists on the destination object. */
  LISTBASE_FOREACH (ParticleSystem *, psys, &ob_dst->particlesystem) {
    if (psys->part == psys_src->part) {
      psys_dst = psys;
      break;
    }
  }

  /* If it does not exist, copy the particle system to the destination object. */
  if (psys_dst == nullptr) {
    ModifierData *md = object_copy_particle_system(bmain, scene, ob_dst, psys_src);
    psys_dst = ((ParticleSystemModifierData *)md)->psys;
  }

  return psys_dst;
}

bool BKE_object_copy_modifier(
    Main *bmain, Scene *scene, Object *ob_dst, const Object *ob_src, ModifierData *md_src)
{
  BLI_assert(ob_dst->type != OB_GPENCIL_LEGACY);

  const ModifierTypeInfo *mti = BKE_modifier_get_info((ModifierType)md_src->type);
  if (!object_modifier_type_copy_check((ModifierType)md_src->type)) {
    /* We never allow copying those modifiers here. */
    return false;
  }
  if (!BKE_object_support_modifier_type_check(ob_dst, md_src->type)) {
    return false;
  }
  if (mti->flags & eModifierTypeFlag_Single) {
    if (BKE_modifiers_findby_type(ob_dst, (ModifierType)md_src->type) != nullptr) {
      return false;
    }
  }

  ParticleSystem *psys_src = nullptr;
  ParticleSystem *psys_dst = nullptr;

  switch (md_src->type) {
    case eModifierType_Softbody:
      BKE_object_copy_softbody(ob_dst, ob_src, 0);
      break;
    case eModifierType_Skin:
      /* ensure skin-node customdata exists */
      BKE_mesh_ensure_skin_customdata((Mesh *)ob_dst->data);
      break;
    case eModifierType_Fluid: {
      FluidModifierData *fmd = (FluidModifierData *)md_src;
      if (fmd->type == MOD_FLUID_TYPE_FLOW) {
        if (fmd->flow != nullptr && fmd->flow->psys != nullptr) {
          psys_src = fmd->flow->psys;
          psys_dst = object_copy_modifier_particle_system_ensure(bmain, scene, ob_dst, psys_src);
        }
      }
      break;
    }
    case eModifierType_DynamicPaint: {
      DynamicPaintModifierData *dpmd = (DynamicPaintModifierData *)md_src;
      if (dpmd->brush != nullptr && dpmd->brush->psys != nullptr) {
        psys_src = dpmd->brush->psys;
        psys_dst = object_copy_modifier_particle_system_ensure(bmain, scene, ob_dst, psys_src);
      }
      break;
    }
    default:
      break;
  }

  ModifierData *md_dst;
  if (md_src->type == eModifierType_ParticleSystem) {
    md_dst = object_copy_particle_system(
        bmain, scene, ob_dst, ((ParticleSystemModifierData *)md_src)->psys);
  }
  else {
    md_dst = BKE_modifier_new(md_src->type);

    STRNCPY(md_dst->name, md_src->name);

    if (md_src->type == eModifierType_Multires) {
      /* Has to be done after mod creation, but *before* we actually copy its settings! */
      multiresModifier_sync_levels_ex(
          ob_dst, (MultiresModifierData *)md_src, (MultiresModifierData *)md_dst);
    }

    BKE_modifier_copydata(md_src, md_dst);

    switch (md_dst->type) {
      case eModifierType_Fluid:
        if (psys_dst != nullptr) {
          FluidModifierData *fmd_dst = (FluidModifierData *)md_dst;
          BLI_assert(fmd_dst->type == MOD_FLUID_TYPE_FLOW && fmd_dst->flow != nullptr &&
                     fmd_dst->flow->psys != nullptr);
          fmd_dst->flow->psys = psys_dst;
        }
        break;
      case eModifierType_DynamicPaint:
        if (psys_dst != nullptr) {
          DynamicPaintModifierData *dpmd_dst = (DynamicPaintModifierData *)md_dst;
          BLI_assert(dpmd_dst->brush != nullptr && dpmd_dst->brush->psys != nullptr);
          dpmd_dst->brush->psys = psys_dst;
        }
        break;
      default:
        break;
    }

    BLI_addtail(&ob_dst->modifiers, md_dst);
    BKE_modifier_unique_name(&ob_dst->modifiers, md_dst);
  }

  BKE_object_modifier_set_active(ob_dst, md_dst);

  return true;
}

bool BKE_object_copy_gpencil_modifier(Object *ob_dst, GpencilModifierData *gmd_src)
{
  BLI_assert(ob_dst->type == OB_GPENCIL_LEGACY);

  GpencilModifierData *gmd_dst = BKE_gpencil_modifier_new(gmd_src->type);
  STRNCPY(gmd_dst->name, gmd_src->name);

  const GpencilModifierTypeInfo *mti = BKE_gpencil_modifier_get_info(
      (GpencilModifierType)gmd_src->type);
  mti->copyData(gmd_src, gmd_dst);

  BLI_addtail(&ob_dst->greasepencil_modifiers, gmd_dst);
  BKE_gpencil_modifier_unique_name(&ob_dst->greasepencil_modifiers, gmd_dst);

  return true;
}

bool BKE_object_modifier_stack_copy(Object *ob_dst,
                                    const Object *ob_src,
                                    const bool do_copy_all,
                                    const int flag_subdata)
{
  if ((ob_dst->type == OB_GPENCIL_LEGACY) != (ob_src->type == OB_GPENCIL_LEGACY)) {
    BLI_assert_msg(0,
                   "Trying to copy a modifier stack between a GPencil object and another type.");
    return false;
  }

  if (!BLI_listbase_is_empty(&ob_dst->modifiers) ||
      !BLI_listbase_is_empty(&ob_dst->greasepencil_modifiers))
  {
    BLI_assert(
        !"Trying to copy a modifier stack into an object having a non-empty modifier stack.");
    return false;
  }

  LISTBASE_FOREACH (ModifierData *, md_src, &ob_src->modifiers) {
    if (!do_copy_all && !object_modifier_type_copy_check((ModifierType)md_src->type)) {
      continue;
    }
    if (!BKE_object_support_modifier_type_check(ob_dst, md_src->type)) {
      continue;
    }

    ModifierData *md_dst = BKE_modifier_copy_ex(md_src, flag_subdata);
    BLI_addtail(&ob_dst->modifiers, md_dst);
  }

  LISTBASE_FOREACH (GpencilModifierData *, gmd_src, &ob_src->greasepencil_modifiers) {
    GpencilModifierData *gmd_dst = BKE_gpencil_modifier_new(gmd_src->type);
    STRNCPY(gmd_dst->name, gmd_src->name);
    BKE_gpencil_modifier_copydata_ex(gmd_src, gmd_dst, flag_subdata);
    BLI_addtail(&ob_dst->greasepencil_modifiers, gmd_dst);
  }

  /* This could be copied from anywhere, since no other modifier actually use this data. But for
   * consistency do it together with particle systems. */
  BKE_object_copy_softbody(ob_dst, ob_src, flag_subdata);

  /* It is mandatory that this happens after copying modifiers, as it will update their `psys`
   * pointers accordingly. */
  BKE_object_copy_particlesystems(ob_dst, ob_src, flag_subdata);

  return true;
}

void BKE_object_link_modifiers(Object *ob_dst, const Object *ob_src)
{
  BKE_object_free_modifiers(ob_dst, 0);

  BKE_object_modifier_stack_copy(ob_dst, ob_src, false, 0);
}

/**
 * Copy CCG related data. Used to sync copy of mesh with reshaped original mesh.
 */
static void copy_ccg_data(Mesh *mesh_destination,
                          Mesh *mesh_source,
                          const eCustomDataType layer_type)
{
  BLI_assert(mesh_destination->totloop == mesh_source->totloop);
  CustomData *data_destination = &mesh_destination->ldata;
  CustomData *data_source = &mesh_source->ldata;
  const int num_elements = mesh_source->totloop;
  if (!CustomData_has_layer(data_source, layer_type)) {
    return;
  }
  const int layer_index = CustomData_get_layer_index(data_destination, layer_type);
  CustomData_free_layer(data_destination, layer_type, num_elements, layer_index);
  BLI_assert(!CustomData_has_layer(data_destination, layer_type));
  CustomData_add_layer(
      data_destination, eCustomDataType(layer_type), CD_SET_DEFAULT, num_elements);
  BLI_assert(CustomData_has_layer(data_destination, layer_type));
  CustomData_copy_layer_type_data(data_source, data_destination, layer_type, 0, 0, num_elements);
}

static void object_update_from_subsurf_ccg(Object *object)
{
  /* Currently CCG is only created for Mesh objects. */
  if (object->type != OB_MESH) {
    return;
  }
  /* If object does not own evaluated mesh we can not access it since it might be freed already
   * (happens on dependency graph free where order of CoW-ed IDs free is undefined).
   *
   * Good news is: such mesh does not have modifiers applied, so no need to worry about CCG. */
  if (!object->runtime.is_data_eval_owned) {
    return;
  }
  /* Object was never evaluated, so can not have CCG subdivision surface. If it were evaluated, do
   * not try to compute OpenSubDiv on the CPU as it is not needed here. */
  Mesh *mesh_eval = BKE_object_get_evaluated_mesh_no_subsurf(object);
  if (mesh_eval == nullptr) {
    return;
  }
  SubdivCCG *subdiv_ccg = mesh_eval->runtime->subdiv_ccg;
  if (subdiv_ccg == nullptr) {
    return;
  }
  /* Check whether there is anything to be reshaped. */
  if (!subdiv_ccg->dirty.coords && !subdiv_ccg->dirty.hidden) {
    return;
  }
  const int tot_level = mesh_eval->runtime->subdiv_ccg_tot_level;
  Object *object_orig = DEG_get_original_object(object);
  Mesh *mesh_orig = (Mesh *)object_orig->data;
  multiresModifier_reshapeFromCCG(tot_level, mesh_orig, subdiv_ccg);
  /* NOTE: we need to reshape into an original mesh from main database,
   * allowing:
   *
   * - Update copies of that mesh at any moment.
   * - Save the file without doing extra reshape.
   * - All the users of the mesh have updated displacement.
   *
   * However, the tricky part here is that we only know about sculpted
   * state of a mesh on an object level, and object is being updated after
   * mesh data-block is updated. This forces us to:
   *
   * - Update mesh data-block from object evaluation, which is technically
   *   forbidden, but there is no other place for this yet.
   * - Reshape to the original mesh from main database, and then copy updated
   *   layer to copy of that mesh (since copy of the mesh has decoupled
   *   custom data layers).
   *
   * All this is defeating all the designs we need to follow to allow safe
   * threaded evaluation, but this is as good as we can make it within the
   * current sculpt/evaluated mesh design. This is also how we've survived
   * with old #DerivedMesh based solutions. So, while this is all wrong and
   * needs reconsideration, doesn't seem to be a big stopper for real
   * production artists.
   */
  /* TODO(sergey): Solve this somehow, to be fully stable for threaded
   * evaluation environment.
   */
  /* NOTE: runtime.data_orig is what was before assigning mesh_eval,
   * it is orig as in what was in object_eval->data before evaluating
   * modifier stack.
   *
   * mesh_cow is a copy-on-written version of `object_orig->data`.
   */
  Mesh *mesh_cow = (Mesh *)object->runtime.data_orig;
  copy_ccg_data(mesh_cow, mesh_orig, CD_MDISPS);
  copy_ccg_data(mesh_cow, mesh_orig, CD_GRID_PAINT_MASK);
  /* Everything is now up-to-date. */
  subdiv_ccg->dirty.coords = false;
  subdiv_ccg->dirty.hidden = false;
}

void BKE_object_eval_assign_data(Object *object_eval, ID *data_eval, bool is_owned)
{
  BLI_assert(object_eval->id.tag & LIB_TAG_COPIED_ON_WRITE);
  BLI_assert(object_eval->runtime.data_eval == nullptr);
  BLI_assert(data_eval->tag & LIB_TAG_NO_MAIN);

  if (is_owned) {
    /* Set flag for debugging. */
    data_eval->tag |= LIB_TAG_COPIED_ON_WRITE_EVAL_RESULT;
  }

  /* Assigned evaluated data. */
  object_eval->runtime.data_eval = data_eval;
  object_eval->runtime.is_data_eval_owned = is_owned;

  /* Overwrite data of evaluated object, if the data-block types match. */
  ID *data = (ID *)object_eval->data;
  if (GS(data->name) == GS(data_eval->name)) {
    /* NOTE: we are not supposed to invoke evaluation for original objects,
     * but some areas are still being ported, so we play safe here. */
    if (object_eval->id.tag & LIB_TAG_COPIED_ON_WRITE) {
      object_eval->data = data_eval;
    }
  }

  /* Is set separately currently. */
  object_eval->runtime.geometry_set_eval = nullptr;
}

void BKE_object_free_derived_caches(Object *ob)
{
  MEM_SAFE_FREE(ob->runtime.bb);

  object_update_from_subsurf_ccg(ob);

  if (ob->runtime.editmesh_eval_cage &&
      ob->runtime.editmesh_eval_cage != reinterpret_cast<Mesh *>(ob->runtime.data_eval))
  {
    BKE_mesh_eval_delete(ob->runtime.editmesh_eval_cage);
  }
  ob->runtime.editmesh_eval_cage = nullptr;

  if (ob->runtime.data_eval != nullptr) {
    if (ob->runtime.is_data_eval_owned) {
      ID *data_eval = ob->runtime.data_eval;
      if (GS(data_eval->name) == ID_ME) {
        BKE_mesh_eval_delete((Mesh *)data_eval);
      }
      else {
        BKE_libblock_free_data(data_eval, false);
        BKE_libblock_free_datablock(data_eval, 0);
        MEM_freeN(data_eval);
      }
    }
    ob->runtime.data_eval = nullptr;
  }
  if (ob->runtime.mesh_deform_eval != nullptr) {
    Mesh *mesh_deform_eval = ob->runtime.mesh_deform_eval;
    BKE_mesh_eval_delete(mesh_deform_eval);
    ob->runtime.mesh_deform_eval = nullptr;
  }

  /* Restore initial pointer for copy-on-write data-blocks, object->data
   * might be pointing to an evaluated data-block data was just freed above. */
  if (ob->runtime.data_orig != nullptr) {
    ob->data = ob->runtime.data_orig;
  }

  BKE_object_to_mesh_clear(ob);
  BKE_pose_backup_clear(ob);
  BKE_object_to_curve_clear(ob);
  BKE_object_free_curve_cache(ob);

  BKE_crazyspace_api_eval_clear(ob);

  /* Clear grease pencil data. */
  if (ob->runtime.gpd_eval != nullptr) {
    BKE_gpencil_eval_delete(ob->runtime.gpd_eval);
    ob->runtime.gpd_eval = nullptr;
  }

  if (ob->runtime.geometry_set_eval != nullptr) {
    delete ob->runtime.geometry_set_eval;
    ob->runtime.geometry_set_eval = nullptr;
  }

  MEM_SAFE_FREE(ob->runtime.editmesh_bb_cage);
}

void BKE_object_free_caches(Object *object)
{
  short update_flag = 0;

  /* Free particle system caches holding paths. */
  if (object->particlesystem.first) {
    LISTBASE_FOREACH (ParticleSystem *, psys, &object->particlesystem) {
      psys_free_path_cache(psys, psys->edit);
      update_flag |= ID_RECALC_PSYS_REDO;
    }
  }

  /* Free memory used by cached derived meshes in the particle system modifiers. */
  LISTBASE_FOREACH (ModifierData *, md, &object->modifiers) {
    if (md->type == eModifierType_ParticleSystem) {
      ParticleSystemModifierData *psmd = (ParticleSystemModifierData *)md;
      if (psmd->mesh_final) {
        BKE_id_free(nullptr, psmd->mesh_final);
        psmd->mesh_final = nullptr;
        if (psmd->mesh_original) {
          BKE_id_free(nullptr, psmd->mesh_original);
          psmd->mesh_original = nullptr;
        }
        psmd->flag |= eParticleSystemFlag_file_loaded;
        update_flag |= ID_RECALC_GEOMETRY;
      }
    }
  }

  /* NOTE: If object is coming from a duplicator, it might be a temporary
   * object created by dependency graph, which shares pointers with original
   * object. In this case we can not free anything.
   */
  if ((object->base_flag & BASE_FROM_DUPLI) == 0) {
    BKE_object_free_derived_caches(object);
    update_flag |= ID_RECALC_GEOMETRY;
  }

  /* Tag object for update, so once memory critical operation is over and
   * scene update routines are back to its business the object will be
   * guaranteed to be in a known state.
   */
  if (update_flag != 0) {
    DEG_id_tag_update(&object->id, update_flag);
  }
}

bool BKE_object_is_in_editmode(const Object *ob)
{
  if (ob->data == nullptr) {
    return false;
  }

  switch (ob->type) {
    case OB_MESH:
      return ((Mesh *)ob->data)->edit_mesh != nullptr;
    case OB_ARMATURE:
      return ((bArmature *)ob->data)->edbo != nullptr;
    case OB_FONT:
      return ((Curve *)ob->data)->editfont != nullptr;
    case OB_MBALL:
      return ((MetaBall *)ob->data)->editelems != nullptr;
    case OB_LATTICE:
      return ((Lattice *)ob->data)->editlatt != nullptr;
    case OB_SURF:
    case OB_CURVES_LEGACY:
      return ((Curve *)ob->data)->editnurb != nullptr;
    case OB_GPENCIL_LEGACY:
      /* Grease Pencil object has no edit mode data. */
      return GPENCIL_EDIT_MODE((bGPdata *)ob->data);
    case OB_CURVES:
    case OB_POINTCLOUD:
    case OB_GREASE_PENCIL:
      return ob->mode == OB_MODE_EDIT;
    default:
      return false;
  }
}

bool BKE_object_is_in_editmode_vgroup(const Object *ob)
{
  return (OB_TYPE_SUPPORT_VGROUP(ob->type) && BKE_object_is_in_editmode(ob));
}

bool BKE_object_data_is_in_editmode(const Object *ob, const ID *id)
{
  const short type = GS(id->name);
  BLI_assert(OB_DATA_SUPPORT_EDITMODE(type));
  switch (type) {
    case ID_ME:
      return ((const Mesh *)id)->edit_mesh != nullptr;
    case ID_CU_LEGACY:
      return ((((const Curve *)id)->editnurb != nullptr) ||
              (((const Curve *)id)->editfont != nullptr));
    case ID_MB:
      return ((const MetaBall *)id)->editelems != nullptr;
    case ID_LT:
      return ((const Lattice *)id)->editlatt != nullptr;
    case ID_AR:
      return ((const bArmature *)id)->edbo != nullptr;
    case ID_CV:
    case ID_PT:
    case ID_GP:
      if (ob) {
        return BKE_object_is_in_editmode(ob);
      }
      return false;
    default:
      BLI_assert_unreachable();
      return false;
  }
}

char *BKE_object_data_editmode_flush_ptr_get(ID *id)
{
  const short type = GS(id->name);
  switch (type) {
    case ID_ME: {
      BMEditMesh *em = ((Mesh *)id)->edit_mesh;
      if (em != nullptr) {
        return &em->needs_flush_to_id;
      }
      break;
    }
    case ID_CU_LEGACY: {
      if (((Curve *)id)->vfont != nullptr) {
        EditFont *ef = ((Curve *)id)->editfont;
        if (ef != nullptr) {
          return &ef->needs_flush_to_id;
        }
      }
      else {
        EditNurb *editnurb = ((Curve *)id)->editnurb;
        if (editnurb) {
          return &editnurb->needs_flush_to_id;
        }
      }
      break;
    }
    case ID_MB: {
      MetaBall *mb = (MetaBall *)id;
      return &mb->needs_flush_to_id;
    }
    case ID_LT: {
      EditLatt *editlatt = ((Lattice *)id)->editlatt;
      if (editlatt) {
        return &editlatt->needs_flush_to_id;
      }
      break;
    }
    case ID_AR: {
      bArmature *arm = (bArmature *)id;
      return &arm->needs_flush_to_id;
    }
    case ID_GP:
    case ID_PT:
    case ID_CV:
      return nullptr;
    default:
      BLI_assert_unreachable();
      return nullptr;
  }
  return nullptr;
}

bool BKE_object_is_in_wpaint_select_vert(const Object *ob)
{
  if (ob->type == OB_MESH) {
    Mesh *me = (Mesh *)ob->data;
    return ((ob->mode & OB_MODE_WEIGHT_PAINT) && (me->edit_mesh == nullptr) &&
            (ME_EDIT_PAINT_SEL_MODE(me) == SCE_SELECT_VERTEX));
  }

  return false;
}

bool BKE_object_has_mode_data(const Object *ob, eObjectMode object_mode)
{
  if (object_mode & OB_MODE_EDIT) {
    if (BKE_object_is_in_editmode(ob)) {
      return true;
    }
  }
  else if (object_mode & OB_MODE_VERTEX_PAINT) {
    if (ob->sculpt && (ob->sculpt->mode_type == OB_MODE_VERTEX_PAINT)) {
      return true;
    }
  }
  else if (object_mode & OB_MODE_WEIGHT_PAINT) {
    if (ob->sculpt && (ob->sculpt->mode_type == OB_MODE_WEIGHT_PAINT)) {
      return true;
    }
  }
  else if (object_mode & OB_MODE_SCULPT) {
    if (ob->sculpt && (ob->sculpt->mode_type == OB_MODE_SCULPT)) {
      return true;
    }
  }
  else if (object_mode & OB_MODE_POSE) {
    if (ob->pose != nullptr) {
      return true;
    }
  }
  return false;
}

bool BKE_object_is_mode_compat(const Object *ob, eObjectMode object_mode)
{
  return ((ob->mode == object_mode) || (ob->mode & object_mode) != 0);
}

int BKE_object_visibility(const Object *ob, const int dag_eval_mode)
{
  if ((ob->base_flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) == 0) {
    return 0;
  }

  /* Test which components the object has. */
  int visibility = OB_VISIBLE_SELF;
  if (ob->particlesystem.first) {
    visibility |= OB_VISIBLE_INSTANCES | OB_VISIBLE_PARTICLES;
  }
  else if (ob->transflag & OB_DUPLI) {
    visibility |= OB_VISIBLE_INSTANCES;
  }

  if (blender::bke::object_has_geometry_set_instances(*ob)) {
    visibility |= OB_VISIBLE_INSTANCES;
  }

  /* Optional hiding of self if there are particles or instancers. */
  if (visibility & (OB_VISIBLE_PARTICLES | OB_VISIBLE_INSTANCES)) {
    switch ((eEvaluationMode)dag_eval_mode) {
      case DAG_EVAL_VIEWPORT:
        if (!(ob->duplicator_visibility_flag & OB_DUPLI_FLAG_VIEWPORT)) {
          visibility &= ~OB_VISIBLE_SELF;
        }
        break;
      case DAG_EVAL_RENDER:
        if (!(ob->duplicator_visibility_flag & OB_DUPLI_FLAG_RENDER)) {
          visibility &= ~OB_VISIBLE_SELF;
        }
        break;
    }
  }

  return visibility;
}

bool BKE_object_exists_check(Main *bmain, const Object *obtest)
{
  if (obtest == nullptr) {
    return false;
  }

  LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
    if (ob == obtest) {
      return true;
    }
  }

  return false;
}

/* *************************************************** */

static const char *get_obdata_defname(int type)
{
  switch (type) {
    case OB_MESH:
      return DATA_("Mesh");
    case OB_CURVES_LEGACY:
      return DATA_("Curve");
    case OB_SURF:
      return DATA_("Surf");
    case OB_FONT:
      return DATA_("Text");
    case OB_MBALL:
      return DATA_("Mball");
    case OB_CAMERA:
      return DATA_("Camera");
    case OB_LAMP:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "Light");
    case OB_LATTICE:
      return DATA_("Lattice");
    case OB_ARMATURE:
      return DATA_("Armature");
    case OB_SPEAKER:
      return DATA_("Speaker");
    case OB_CURVES:
      return DATA_("Curves");
    case OB_POINTCLOUD:
      return DATA_("PointCloud");
    case OB_VOLUME:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_ID, "Volume");
    case OB_EMPTY:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_ID, "Empty");
    case OB_GPENCIL_LEGACY:
      return DATA_("GPencil");
    case OB_LIGHTPROBE:
      return DATA_("LightProbe");
    case OB_GREASE_PENCIL:
      return DATA_("GreasePencil");
    default:
      CLOG_ERROR(&LOG, "Internal error, bad type: %d", type);
      return CTX_DATA_(BLT_I18NCONTEXT_ID_ID, "Empty");
  }
}

static void object_init(Object *ob, const short ob_type)
{
  object_init_data(&ob->id);

  ob->type = ob_type;

  if (ob->type != OB_EMPTY) {
    zero_v2(ob->ima_ofs);
  }

  if (ELEM(ob->type, OB_LAMP, OB_CAMERA, OB_SPEAKER)) {
    ob->trackflag = OB_NEGZ;
    ob->upflag = OB_POSY;
  }

  if (ob->type == OB_GPENCIL_LEGACY) {
    ob->dtx |= OB_USE_GPENCIL_LIGHTS;
  }

  if (ob->type == OB_LAMP) {
    /* Lights are invisible to camera rays and are assumed to be a
     * shadow catcher by default. */
    ob->visibility_flag |= OB_HIDE_CAMERA | OB_SHADOW_CATCHER;
  }
}

void *BKE_object_obdata_add_from_type(Main *bmain, int type, const char *name)
{
  if (name == nullptr) {
    name = get_obdata_defname(type);
  }

  switch (type) {
    case OB_MESH:
      return BKE_mesh_add(bmain, name);
    case OB_CURVES_LEGACY:
      return BKE_curve_add(bmain, name, OB_CURVES_LEGACY);
    case OB_SURF:
      return BKE_curve_add(bmain, name, OB_SURF);
    case OB_FONT:
      return BKE_curve_add(bmain, name, OB_FONT);
    case OB_MBALL:
      return BKE_mball_add(bmain, name);
    case OB_CAMERA:
      return BKE_camera_add(bmain, name);
    case OB_LAMP:
      return BKE_light_add(bmain, name);
    case OB_LATTICE:
      return BKE_lattice_add(bmain, name);
    case OB_ARMATURE:
      return BKE_armature_add(bmain, name);
    case OB_SPEAKER:
      return BKE_speaker_add(bmain, name);
    case OB_LIGHTPROBE:
      return BKE_lightprobe_add(bmain, name);
    case OB_GPENCIL_LEGACY:
      return BKE_gpencil_data_addnew(bmain, name);
    case OB_CURVES:
      return BKE_curves_add(bmain, name);
    case OB_POINTCLOUD:
      return BKE_pointcloud_add_default(bmain, name);
    case OB_VOLUME:
      return BKE_volume_add(bmain, name);
    case OB_GREASE_PENCIL:
      return BKE_grease_pencil_add(bmain, name);
    case OB_EMPTY:
      return nullptr;
    default:
      CLOG_ERROR(&LOG, "Internal error, bad type: %d", type);
      return nullptr;
  }
}

int BKE_object_obdata_to_type(const ID *id)
{
  /* Keep in sync with #OB_DATA_SUPPORT_ID macro. */
  switch (GS(id->name)) {
    case ID_ME:
      return OB_MESH;
    case ID_CU_LEGACY:
      return BKE_curve_type_get((const Curve *)id);
    case ID_MB:
      return OB_MBALL;
    case ID_LA:
      return OB_LAMP;
    case ID_SPK:
      return OB_SPEAKER;
    case ID_CA:
      return OB_CAMERA;
    case ID_LT:
      return OB_LATTICE;
    case ID_GD_LEGACY:
      return OB_GPENCIL_LEGACY;
    case ID_AR:
      return OB_ARMATURE;
    case ID_LP:
      return OB_LIGHTPROBE;
    case ID_CV:
      return OB_CURVES;
    case ID_PT:
      return OB_POINTCLOUD;
    case ID_VO:
      return OB_VOLUME;
    case ID_GP:
      return OB_GREASE_PENCIL;
    default:
      return -1;
  }
}

Object *BKE_object_add_only_object(Main *bmain, int type, const char *name)
{
  if (!name) {
    name = get_obdata_defname(type);
  }

  /* We cannot use #BKE_id_new here as we need some custom initialization code. */
  Object *ob = (Object *)BKE_libblock_alloc(bmain, ID_OB, name, bmain ? 0 : LIB_ID_CREATE_NO_MAIN);

  /* We increase object user count when linking to Collections. */
  id_us_min(&ob->id);

  /* default object vars */
  object_init(ob, type);

  return ob;
}

static Object *object_add_common(
    Main *bmain, const Scene *scene, ViewLayer *view_layer, int type, const char *name)
{
  Object *ob = BKE_object_add_only_object(bmain, type, name);
  ob->data = BKE_object_obdata_add_from_type(bmain, type, name);
  BKE_view_layer_base_deselect_all(scene, view_layer);

  DEG_id_tag_update_ex(
      bmain, &ob->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
  return ob;
}

Object *BKE_object_add(
    Main *bmain, Scene *scene, ViewLayer *view_layer, int type, const char *name)
{
  Object *ob = object_add_common(bmain, scene, view_layer, type, name);

  LayerCollection *layer_collection = BKE_layer_collection_get_active(view_layer);
  BKE_collection_viewlayer_object_add(bmain, view_layer, layer_collection->collection, ob);

  /* NOTE: There is no way to be sure that #BKE_collection_viewlayer_object_add will actually
   * manage to find a valid collection in given `view_layer` to add the new object to. */
  BKE_view_layer_synced_ensure(scene, view_layer);
  Base *base = BKE_view_layer_base_find(view_layer, ob);
  if (base != nullptr) {
    BKE_view_layer_base_select_and_set_active(view_layer, base);
  }

  return ob;
}

Object *BKE_object_add_from(
    Main *bmain, Scene *scene, ViewLayer *view_layer, int type, const char *name, Object *ob_src)
{
  Object *ob = object_add_common(bmain, scene, view_layer, type, name);
  BKE_collection_object_add_from(bmain, scene, ob_src, ob);

  BKE_view_layer_synced_ensure(scene, view_layer);
  Base *base = BKE_view_layer_base_find(view_layer, ob);
  BKE_view_layer_base_select_and_set_active(view_layer, base);

  return ob;
}

Object *BKE_object_add_for_data(Main *bmain,
                                const Scene *scene,
                                ViewLayer *view_layer,
                                int type,
                                const char *name,
                                ID *data,
                                bool do_id_user)
{
  /* same as object_add_common, except we don't create new ob->data */
  Object *ob = BKE_object_add_only_object(bmain, type, name);
  ob->data = (void *)data;
  if (do_id_user) {
    id_us_plus(data);
  }

  BKE_view_layer_base_deselect_all(scene, view_layer);
  DEG_id_tag_update_ex(
      bmain, &ob->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);

  LayerCollection *layer_collection = BKE_layer_collection_get_active(view_layer);
  BKE_collection_object_add(bmain, layer_collection->collection, ob);

  BKE_view_layer_synced_ensure(scene, view_layer);
  Base *base = BKE_view_layer_base_find(view_layer, ob);
  BKE_view_layer_base_select_and_set_active(view_layer, base);

  return ob;
}

void BKE_object_copy_softbody(Object *ob_dst, const Object *ob_src, const int flag)
{
  SoftBody *sb = ob_src->soft;
  const bool is_orig = (flag & LIB_ID_COPY_SET_COPIED_ON_WRITE) == 0;

  ob_dst->softflag = ob_src->softflag;
  if (sb == nullptr) {
    ob_dst->soft = nullptr;
    return;
  }

  SoftBody *sbn = (SoftBody *)MEM_dupallocN(sb);

  if ((flag & LIB_ID_COPY_CACHES) == 0) {
    sbn->totspring = sbn->totpoint = 0;
    sbn->bpoint = nullptr;
    sbn->bspring = nullptr;
  }
  else {
    sbn->totspring = sb->totspring;
    sbn->totpoint = sb->totpoint;

    if (sbn->bpoint) {
      int i;

      sbn->bpoint = (BodyPoint *)MEM_dupallocN(sbn->bpoint);

      for (i = 0; i < sbn->totpoint; i++) {
        if (sbn->bpoint[i].springs) {
          sbn->bpoint[i].springs = (int *)MEM_dupallocN(sbn->bpoint[i].springs);
        }
      }
    }

    if (sb->bspring) {
      sbn->bspring = (BodySpring *)MEM_dupallocN(sb->bspring);
    }
  }

  sbn->keys = nullptr;
  sbn->totkey = sbn->totpointkey = 0;

  sbn->scratch = nullptr;

  if (is_orig) {
    sbn->shared = (SoftBody_Shared *)MEM_dupallocN(sb->shared);
    sbn->shared->pointcache = BKE_ptcache_copy_list(
        &sbn->shared->ptcaches, &sb->shared->ptcaches, flag);
  }

  if (sb->effector_weights) {
    sbn->effector_weights = (EffectorWeights *)MEM_dupallocN(sb->effector_weights);
  }

  ob_dst->soft = sbn;
}

ParticleSystem *BKE_object_copy_particlesystem(ParticleSystem *psys, const int flag)
{
  ParticleSystem *psysn = (ParticleSystem *)MEM_dupallocN(psys);

  psys_copy_particles(psysn, psys);

  if (psys->clmd) {
    psysn->clmd = (ClothModifierData *)BKE_modifier_new(eModifierType_Cloth);
    BKE_modifier_copydata_ex((ModifierData *)psys->clmd, (ModifierData *)psysn->clmd, flag);
    psys->hair_in_mesh = psys->hair_out_mesh = nullptr;
  }

  BLI_duplicatelist(&psysn->targets, &psys->targets);

  psysn->pathcache = nullptr;
  psysn->childcache = nullptr;
  psysn->edit = nullptr;
  psysn->pdd = nullptr;
  psysn->effectors = nullptr;
  psysn->tree = nullptr;
  psysn->bvhtree = nullptr;
  psysn->batch_cache = nullptr;

  BLI_listbase_clear(&psysn->pathcachebufs);
  BLI_listbase_clear(&psysn->childcachebufs);

  if (flag & LIB_ID_COPY_SET_COPIED_ON_WRITE) {
    /* XXX Disabled, fails when evaluating depsgraph after copying ID with no main for preview
     * creation. */
    // BLI_assert((psys->flag & PSYS_SHARED_CACHES) == 0);
    psysn->flag |= PSYS_SHARED_CACHES;
    BLI_assert(psysn->pointcache != nullptr);
  }
  else {
    psysn->pointcache = BKE_ptcache_copy_list(&psysn->ptcaches, &psys->ptcaches, flag);
  }

  /* XXX(@ideasman42): from reading existing code this seems correct but intended usage of
   * point-cache should with cloth should be added in 'ParticleSystem'. */
  if (psysn->clmd) {
    psysn->clmd->point_cache = psysn->pointcache;
  }

  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
    id_us_plus((ID *)psysn->part);
  }

  return psysn;
}

void BKE_object_copy_particlesystems(Object *ob_dst, const Object *ob_src, const int flag)
{
  if (ob_dst->type != OB_MESH) {
    /* currently only mesh objects can have soft body */
    return;
  }

  BLI_listbase_clear(&ob_dst->particlesystem);
  LISTBASE_FOREACH (ParticleSystem *, psys, &ob_src->particlesystem) {
    ParticleSystem *npsys = BKE_object_copy_particlesystem(psys, flag);

    BLI_addtail(&ob_dst->particlesystem, npsys);

    /* need to update particle modifiers too */
    LISTBASE_FOREACH (ModifierData *, md, &ob_dst->modifiers) {
      if (md->type == eModifierType_ParticleSystem) {
        ParticleSystemModifierData *psmd = (ParticleSystemModifierData *)md;
        if (psmd->psys == psys) {
          psmd->psys = npsys;
        }
      }
      else if (md->type == eModifierType_DynamicPaint) {
        DynamicPaintModifierData *pmd = (DynamicPaintModifierData *)md;
        if (pmd->brush) {
          if (pmd->brush->psys == psys) {
            pmd->brush->psys = npsys;
          }
        }
      }
      else if (md->type == eModifierType_Fluid) {
        FluidModifierData *fmd = (FluidModifierData *)md;

        if (fmd->type == MOD_FLUID_TYPE_FLOW) {
          if (fmd->flow) {
            if (fmd->flow->psys == psys) {
              fmd->flow->psys = npsys;
            }
          }
        }
      }
    }
  }
}

static void copy_object_pose(Object *obn, const Object *ob, const int flag)
{
  /* NOTE: need to clear `obn->pose` pointer first,
   * so that #BKE_pose_copy_data works (otherwise there's a crash) */
  obn->pose = nullptr;
  BKE_pose_copy_data_ex(&obn->pose, ob->pose, flag, true); /* true = copy constraints */

  LISTBASE_FOREACH (bPoseChannel *, chan, &obn->pose->chanbase) {
    chan->flag &= ~(POSE_LOC | POSE_ROT | POSE_SIZE);

    /* XXX Remapping object pointing onto itself should be handled by generic
     *     BKE_library_remap stuff, but...
     *     the flush_constraint_targets callback am not sure about, so will delay that for now. */
    LISTBASE_FOREACH (bConstraint *, con, &chan->constraints) {
      ListBase targets = {nullptr, nullptr};

      if (BKE_constraint_targets_get(con, &targets)) {
        LISTBASE_FOREACH (bConstraintTarget *, ct, &targets) {
          if (ct->tar == ob) {
            ct->tar = obn;
          }
        }

        BKE_constraint_targets_flush(con, &targets, false);
      }
    }
  }
}

bool BKE_object_pose_context_check(const Object *ob)
{
  if ((ob) && (ob->type == OB_ARMATURE) && (ob->pose) && (ob->mode & OB_MODE_POSE)) {
    return true;
  }

  return false;
}

Object *BKE_object_pose_armature_get(Object *ob)
{
  if (ob == nullptr) {
    return nullptr;
  }

  if (BKE_object_pose_context_check(ob)) {
    return ob;
  }

  ob = BKE_modifiers_is_deformed_by_armature(ob);

  /* Only use selected check when non-active. */
  if (BKE_object_pose_context_check(ob)) {
    return ob;
  }

  return nullptr;
}

Object *BKE_object_pose_armature_get_with_wpaint_check(Object *ob)
{
  /* When not in weight paint mode. */
  if (ob) {
    switch (ob->type) {
      case OB_MESH: {
        if ((ob->mode & OB_MODE_WEIGHT_PAINT) == 0) {
          return nullptr;
        }
        break;
      }
      case OB_GPENCIL_LEGACY: {
        if ((ob->mode & OB_MODE_WEIGHT_GPENCIL_LEGACY) == 0) {
          return nullptr;
        }
        break;
      }
    }
  }
  return BKE_object_pose_armature_get(ob);
}

Object *BKE_object_pose_armature_get_visible(Object *ob,
                                             const Scene *scene,
                                             ViewLayer *view_layer,
                                             View3D *v3d)
{
  Object *ob_armature = BKE_object_pose_armature_get(ob);
  if (ob_armature) {
    BKE_view_layer_synced_ensure(scene, view_layer);
    Base *base = BKE_view_layer_base_find(view_layer, ob_armature);
    if (base) {
      if (BASE_VISIBLE(v3d, base)) {
        return ob_armature;
      }
    }
  }
  return nullptr;
}

Object **BKE_object_pose_array_get_ex(
    const Scene *scene, ViewLayer *view_layer, View3D *v3d, uint *r_objects_len, bool unique)
{
  BKE_view_layer_synced_ensure(scene, view_layer);
  Object *ob_active = BKE_view_layer_active_object_get(view_layer);
  Object *ob_pose = BKE_object_pose_armature_get(ob_active);
  Object **objects = nullptr;
  if (ob_pose == ob_active) {
    ObjectsInModeParams ob_params{};
    ob_params.object_mode = OB_MODE_POSE;
    ob_params.no_dup_data = unique;

    objects = BKE_view_layer_array_from_objects_in_mode_params(
        scene, view_layer, v3d, r_objects_len, &ob_params);
  }
  else if (ob_pose != nullptr) {
    *r_objects_len = 1;
    objects = (Object **)MEM_mallocN(sizeof(*objects), __func__);
    objects[0] = ob_pose;
  }
  else {
    *r_objects_len = 0;
    objects = (Object **)MEM_mallocN(0, __func__);
  }
  return objects;
}
Object **BKE_object_pose_array_get_unique(const Scene *scene,
                                          ViewLayer *view_layer,
                                          View3D *v3d,
                                          uint *r_objects_len)
{
  return BKE_object_pose_array_get_ex(scene, view_layer, v3d, r_objects_len, true);
}
Object **BKE_object_pose_array_get(const Scene *scene,
                                   ViewLayer *view_layer,
                                   View3D *v3d,
                                   uint *r_objects_len)
{
  return BKE_object_pose_array_get_ex(scene, view_layer, v3d, r_objects_len, false);
}

Base **BKE_object_pose_base_array_get_ex(
    const Scene *scene, ViewLayer *view_layer, View3D *v3d, uint *r_bases_len, bool unique)
{
  BKE_view_layer_synced_ensure(scene, view_layer);
  Base *base_active = BKE_view_layer_active_base_get(view_layer);
  Object *ob_pose = base_active ? BKE_object_pose_armature_get(base_active->object) : nullptr;
  Base *base_pose = nullptr;
  Base **bases = nullptr;

  if (base_active) {
    if (ob_pose == base_active->object) {
      base_pose = base_active;
    }
    else {
      base_pose = BKE_view_layer_base_find(view_layer, ob_pose);
    }
  }

  if (base_active && (base_pose == base_active)) {
    ObjectsInModeParams ob_params{};
    ob_params.object_mode = OB_MODE_POSE;
    ob_params.no_dup_data = unique;

    bases = BKE_view_layer_array_from_bases_in_mode_params(
        scene, view_layer, v3d, r_bases_len, &ob_params);
  }
  else if (base_pose != nullptr) {
    *r_bases_len = 1;
    bases = (Base **)MEM_mallocN(sizeof(*bases), __func__);
    bases[0] = base_pose;
  }
  else {
    *r_bases_len = 0;
    bases = (Base **)MEM_mallocN(0, __func__);
  }
  return bases;
}
Base **BKE_object_pose_base_array_get_unique(const Scene *scene,
                                             ViewLayer *view_layer,
                                             View3D *v3d,
                                             uint *r_bases_len)
{
  return BKE_object_pose_base_array_get_ex(scene, view_layer, v3d, r_bases_len, true);
}
Base **BKE_object_pose_base_array_get(const Scene *scene,
                                      ViewLayer *view_layer,
                                      View3D *v3d,
                                      uint *r_bases_len)
{
  return BKE_object_pose_base_array_get_ex(scene, view_layer, v3d, r_bases_len, false);
}

void BKE_object_transform_copy(Object *ob_tar, const Object *ob_src)
{
  copy_v3_v3(ob_tar->loc, ob_src->loc);
  copy_v3_v3(ob_tar->rot, ob_src->rot);
  copy_v4_v4(ob_tar->quat, ob_src->quat);
  copy_v3_v3(ob_tar->rotAxis, ob_src->rotAxis);
  ob_tar->rotAngle = ob_src->rotAngle;
  ob_tar->rotmode = ob_src->rotmode;
  copy_v3_v3(ob_tar->scale, ob_src->scale);
}

Object *BKE_object_duplicate(Main *bmain,
                             Object *ob,
                             eDupli_ID_Flags dupflag,
                             uint duplicate_options)
{
  const bool is_subprocess = (duplicate_options & LIB_ID_DUPLICATE_IS_SUBPROCESS) != 0;
  const bool is_root_id = (duplicate_options & LIB_ID_DUPLICATE_IS_ROOT_ID) != 0;
  int copy_flags = LIB_ID_COPY_DEFAULT;

  if (!is_subprocess) {
    BKE_main_id_newptr_and_tag_clear(bmain);
  }
  else {
    /* In case copying object is a sub-process of collection (or scene) copying, do not try to
     * re-assign RB objects to existing RBW collections. */
    copy_flags |= LIB_ID_COPY_RIGID_BODY_NO_COLLECTION_HANDLING;
  }
  if (is_root_id) {
    /* In case root duplicated ID is linked, assume we want to get a local copy of it and duplicate
     * all expected linked data. */
    if (ID_IS_LINKED(ob)) {
      dupflag |= USER_DUP_LINKED_ID;
    }
    duplicate_options &= ~LIB_ID_DUPLICATE_IS_ROOT_ID;
  }

  Material ***matarar;

  Object *obn = (Object *)BKE_id_copy_for_duplicate(bmain, &ob->id, dupflag, copy_flags);

  /* 0 == full linked. */
  if (dupflag == 0) {
    return obn;
  }

  if (dupflag & USER_DUP_MAT) {
    for (int i = 0; i < obn->totcol; i++) {
      BKE_id_copy_for_duplicate(bmain, (ID *)obn->mat[i], dupflag, copy_flags);
    }
  }
  if (dupflag & USER_DUP_PSYS) {
    LISTBASE_FOREACH (ParticleSystem *, psys, &obn->particlesystem) {
      BKE_id_copy_for_duplicate(bmain, (ID *)psys->part, dupflag, copy_flags);
    }
  }

  ID *id_old = (ID *)obn->data;
  ID *id_new = nullptr;
  const bool need_to_duplicate_obdata = (id_old != nullptr) && (id_old->newid == nullptr);

  switch (obn->type) {
    case OB_MESH:
      if (dupflag & USER_DUP_MESH) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_CURVES_LEGACY:
      if (dupflag & USER_DUP_CURVE) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_SURF:
      if (dupflag & USER_DUP_SURF) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_FONT:
      if (dupflag & USER_DUP_FONT) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_MBALL:
      if (dupflag & USER_DUP_MBALL) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_LAMP:
      if (dupflag & USER_DUP_LAMP) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_ARMATURE:
      if (dupflag & USER_DUP_ARM) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_LATTICE:
      if (dupflag & USER_DUP_LATTICE) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_CAMERA:
      if (dupflag & USER_DUP_CAMERA) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_LIGHTPROBE:
      if (dupflag & USER_DUP_LIGHTPROBE) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_SPEAKER:
      if (dupflag & USER_DUP_SPEAKER) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_GPENCIL_LEGACY:
      if (dupflag & USER_DUP_GPENCIL) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_CURVES:
      if (dupflag & USER_DUP_CURVES) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_POINTCLOUD:
      if (dupflag & USER_DUP_POINTCLOUD) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
    case OB_VOLUME:
      if (dupflag & USER_DUP_VOLUME) {
        id_new = BKE_id_copy_for_duplicate(bmain, id_old, dupflag, copy_flags);
      }
      break;
  }

  /* If obdata has been copied, we may also have to duplicate the materials assigned to it. */
  if (need_to_duplicate_obdata && !ELEM(id_new, nullptr, id_old)) {
    if (dupflag & USER_DUP_MAT) {
      matarar = BKE_object_material_array_p(obn);
      if (matarar) {
        for (int i = 0; i < obn->totcol; i++) {
          BKE_id_copy_for_duplicate(bmain, (ID *)(*matarar)[i], dupflag, copy_flags);
        }
      }
    }
  }

  if (!is_subprocess) {
    /* This code will follow into all ID links using an ID tagged with LIB_TAG_NEW. */
    BKE_libblock_relink_to_newid(bmain, &obn->id, 0);

#ifndef NDEBUG
    /* Call to `BKE_libblock_relink_to_newid` above is supposed to have cleared all those flags. */
    ID *id_iter;
    FOREACH_MAIN_ID_BEGIN (bmain, id_iter) {
      BLI_assert((id_iter->tag & LIB_TAG_NEW) == 0);
    }
    FOREACH_MAIN_ID_END;
#endif

    /* Cleanup. */
    BKE_main_id_newptr_and_tag_clear(bmain);
  }

  if (obn->type == OB_ARMATURE) {
    DEG_id_tag_update(&obn->id, ID_RECALC_GEOMETRY);
    if (obn->pose) {
      BKE_pose_tag_recalc(bmain, obn->pose);
    }
    //    BKE_pose_rebuild(bmain, obn, obn->data, true);
  }

  if (obn->data != nullptr) {
    DEG_id_tag_update_ex(bmain, (ID *)obn->data, ID_RECALC_EDITORS);
  }

  return obn;
}

bool BKE_object_is_libdata(const Object *ob)
{
  return (ob && ID_IS_LINKED(ob));
}

bool BKE_object_obdata_is_libdata(const Object *ob)
{
  /* Linked objects with local obdata are forbidden! */
  BLI_assert(!ob || !ob->data || (ID_IS_LINKED(ob) ? ID_IS_LINKED(ob->data) : true));
  return (ob && ob->data && ID_IS_LINKED(ob->data));
}

void BKE_object_obdata_size_init(Object *ob, const float size)
{
  /* apply radius as a scale to types that support it */
  switch (ob->type) {
    case OB_EMPTY: {
      ob->empty_drawsize *= size;
      break;
    }
    case OB_FONT: {
      Curve *cu = (Curve *)ob->data;
      cu->fsize *= size;
      break;
    }
    case OB_CAMERA: {
      Camera *cam = (Camera *)ob->data;
      cam->drawsize *= size;
      break;
    }
    case OB_LAMP: {
      Light *lamp = (Light *)ob->data;
      lamp->dist *= size;
      lamp->radius *= size;
      lamp->area_size *= size;
      lamp->area_sizey *= size;
      lamp->area_sizez *= size;
      break;
    }
    /* Only lattice (not mesh, curve, mball...),
     * because its got data when newly added */
    case OB_LATTICE: {
      Lattice *lt = (Lattice *)ob->data;
      float mat[4][4];

      unit_m4(mat);
      scale_m4_fl(mat, size);

      BKE_lattice_transform(lt, (float(*)[4])mat, false);
      break;
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Object Matrix Get/Set API
 * \{ */

void BKE_object_scale_to_mat3(Object *ob, float mat[3][3])
{
  float3 vec;
  mul_v3_v3v3(vec, ob->scale, ob->dscale);
  size_to_mat3(mat, vec);
}

void BKE_object_rot_to_mat3(const Object *ob, float mat[3][3], bool use_drot)
{
  float rmat[3][3], dmat[3][3];

  /* 'dmat' is the delta-rotation matrix, which will get (pre)multiplied
   * with the rotation matrix to yield the appropriate rotation
   */

  /* rotations may either be quats, eulers (with various rotation orders), or axis-angle */
  if (ob->rotmode > 0) {
    /* Euler rotations
     * (will cause gimbal lock, but this can be alleviated a bit with rotation orders). */
    eulO_to_mat3(rmat, ob->rot, ob->rotmode);
    eulO_to_mat3(dmat, ob->drot, ob->rotmode);
  }
  else if (ob->rotmode == ROT_MODE_AXISANGLE) {
    /* axis-angle - not really that great for 3D-changing orientations */
    axis_angle_to_mat3(rmat, ob->rotAxis, ob->rotAngle);
    axis_angle_to_mat3(dmat, ob->drotAxis, ob->drotAngle);
  }
  else {
    /* Quaternions are normalized before use to eliminate scaling issues. */
    float tquat[4];

    normalize_qt_qt(tquat, ob->quat);
    quat_to_mat3(rmat, tquat);

    normalize_qt_qt(tquat, ob->dquat);
    quat_to_mat3(dmat, tquat);
  }

  /* combine these rotations */
  if (use_drot) {
    mul_m3_m3m3(mat, dmat, rmat);
  }
  else {
    copy_m3_m3(mat, rmat);
  }
}

void BKE_object_mat3_to_rot(Object *ob, float mat[3][3], bool use_compat)
{
  BLI_ASSERT_UNIT_M3(mat);

  switch (ob->rotmode) {
    case ROT_MODE_QUAT: {
      float dquat[4];
      mat3_normalized_to_quat(ob->quat, mat);
      normalize_qt_qt(dquat, ob->dquat);
      invert_qt_normalized(dquat);
      mul_qt_qtqt(ob->quat, dquat, ob->quat);
      break;
    }
    case ROT_MODE_AXISANGLE: {
      float quat[4];
      float dquat[4];

      /* Without `drot` we could apply 'mat' directly. */
      mat3_normalized_to_quat(quat, mat);
      axis_angle_to_quat(dquat, ob->drotAxis, ob->drotAngle);
      invert_qt_normalized(dquat);
      mul_qt_qtqt(quat, dquat, quat);
      quat_to_axis_angle(ob->rotAxis, &ob->rotAngle, quat);
      break;
    }
    default: /* euler */
    {
      float quat[4];
      float dquat[4];

      /* Without `drot` we could apply 'mat' directly. */
      mat3_normalized_to_quat(quat, mat);
      eulO_to_quat(dquat, ob->drot, ob->rotmode);
      invert_qt_normalized(dquat);
      mul_qt_qtqt(quat, dquat, quat);
      /* End `drot` correction. */

      if (use_compat) {
        quat_to_compatible_eulO(ob->rot, ob->rot, ob->rotmode, quat);
      }
      else {
        quat_to_eulO(ob->rot, ob->rotmode, quat);
      }
      break;
    }
  }
}

void BKE_object_tfm_protected_backup(const Object *ob, ObjectTfmProtectedChannels *obtfm)
{

#define TFMCPY(_v) (obtfm->_v = ob->_v)
#define TFMCPY3D(_v) copy_v3_v3(obtfm->_v, ob->_v)
#define TFMCPY4D(_v) copy_v4_v4(obtfm->_v, ob->_v)

  TFMCPY3D(loc);
  TFMCPY3D(dloc);
  TFMCPY3D(scale);
  TFMCPY3D(dscale);
  TFMCPY3D(rot);
  TFMCPY3D(drot);
  TFMCPY4D(quat);
  TFMCPY4D(dquat);
  TFMCPY3D(rotAxis);
  TFMCPY3D(drotAxis);
  TFMCPY(rotAngle);
  TFMCPY(drotAngle);

#undef TFMCPY
#undef TFMCPY3D
#undef TFMCPY4D
}

void BKE_object_tfm_protected_restore(Object *ob,
                                      const ObjectTfmProtectedChannels *obtfm,
                                      const short protectflag)
{
  uint i;

  for (i = 0; i < 3; i++) {
    if (protectflag & (OB_LOCK_LOCX << i)) {
      ob->loc[i] = obtfm->loc[i];
      ob->dloc[i] = obtfm->dloc[i];
    }

    if (protectflag & (OB_LOCK_SCALEX << i)) {
      ob->scale[i] = obtfm->scale[i];
      ob->dscale[i] = obtfm->dscale[i];
    }

    if (protectflag & (OB_LOCK_ROTX << i)) {
      ob->rot[i] = obtfm->rot[i];
      ob->drot[i] = obtfm->drot[i];

      ob->quat[i + 1] = obtfm->quat[i + 1];
      ob->dquat[i + 1] = obtfm->dquat[i + 1];

      ob->rotAxis[i] = obtfm->rotAxis[i];
      ob->drotAxis[i] = obtfm->drotAxis[i];
    }
  }

  if ((protectflag & OB_LOCK_ROT4D) && (protectflag & OB_LOCK_ROTW)) {
    ob->quat[0] = obtfm->quat[0];
    ob->dquat[0] = obtfm->dquat[0];

    ob->rotAngle = obtfm->rotAngle;
    ob->drotAngle = obtfm->drotAngle;
  }
}

void BKE_object_tfm_copy(Object *object_dst, const Object *object_src)
{
#define TFMCPY(_v) (object_dst->_v = object_src->_v)
#define TFMCPY3D(_v) copy_v3_v3(object_dst->_v, object_src->_v)
#define TFMCPY4D(_v) copy_v4_v4(object_dst->_v, object_src->_v)

  TFMCPY3D(loc);
  TFMCPY3D(dloc);
  TFMCPY3D(scale);
  TFMCPY3D(dscale);
  TFMCPY3D(rot);
  TFMCPY3D(drot);
  TFMCPY4D(quat);
  TFMCPY4D(dquat);
  TFMCPY3D(rotAxis);
  TFMCPY3D(drotAxis);
  TFMCPY(rotAngle);
  TFMCPY(drotAngle);

#undef TFMCPY
#undef TFMCPY3D
#undef TFMCPY4D
}

void BKE_object_to_mat3(Object *ob, float r_mat[3][3]) /* no parent */
{
  float smat[3][3];
  float rmat[3][3];

  /* Scale. */
  BKE_object_scale_to_mat3(ob, smat);

  /* Rotation. */
  BKE_object_rot_to_mat3(ob, rmat, true);
  mul_m3_m3m3(r_mat, rmat, smat);
}

void BKE_object_to_mat4(Object *ob, float r_mat[4][4])
{
  float tmat[3][3];

  BKE_object_to_mat3(ob, tmat);

  copy_m4_m3(r_mat, tmat);

  add_v3_v3v3(r_mat[3], ob->loc, ob->dloc);
}

void BKE_object_matrix_local_get(Object *ob, float r_mat[4][4])
{
  if (ob->parent) {
    float par_imat[4][4];

    BKE_object_get_parent_matrix(ob, ob->parent, par_imat);
    invert_m4(par_imat);
    mul_m4_m4m4(r_mat, par_imat, ob->object_to_world);
  }
  else {
    copy_m4_m4(r_mat, ob->object_to_world);
  }
}

/**
 * \return success if \a mat is set.
 */
static bool ob_parcurve(Object *ob, Object *par, float r_mat[4][4])
{
  Curve *cu = (Curve *)par->data;
  float vec[4], quat[4], radius, ctime;

  /* NOTE: Curve cache is supposed to be evaluated here already, however there
   * are cases where we can not guarantee that. This includes, for example,
   * dependency cycles. We can't correct anything from here, since that would
   * cause threading conflicts.
   *
   * TODO(sergey): Some of the legit looking cases like #56619 need to be
   * looked into, and maybe curve cache (and other dependencies) are to be
   * evaluated prior to conversion. */
  if (par->runtime.curve_cache == nullptr) {
    return false;
  }
  if (par->runtime.curve_cache->anim_path_accum_length == nullptr) {
    return false;
  }

  /* `ctime` is now a proper var setting of Curve which gets set by Animato like any other var
   * that's animated, but this will only work if it actually is animated.
   *
   * We divide the curve-time calculated in the previous step by the length of the path,
   * to get a time factor, which then gets clamped to lie within 0.0 - 1.0 range. */
  if (cu->pathlen) {
    ctime = cu->ctime / cu->pathlen;
  }
  else {
    ctime = cu->ctime;
  }

  if (cu->flag & CU_PATH_CLAMP) {
    CLAMP(ctime, 0.0f, 1.0f);
  }

  unit_m4(r_mat);

  /* vec: 4 items! */
  if (BKE_where_on_path(
          par, ctime, vec, nullptr, (cu->flag & CU_FOLLOW) ? quat : nullptr, &radius, nullptr))
  {
    if (cu->flag & CU_FOLLOW) {
      quat_apply_track(quat, ob->trackflag, ob->upflag);
      normalize_qt(quat);
      quat_to_mat4(r_mat, quat);
    }
    if (cu->flag & CU_PATH_RADIUS) {
      float tmat[4][4], rmat[4][4];
      scale_m4_fl(tmat, radius);
      mul_m4_m4m4(rmat, tmat, r_mat);
      copy_m4_m4(r_mat, rmat);
    }
    copy_v3_v3(r_mat[3], vec);
  }

  return true;
}

static void ob_parbone(Object *ob, Object *par, float r_mat[4][4])
{
  float3 vec;

  if (par->type != OB_ARMATURE) {
    unit_m4(r_mat);
    return;
  }

  /* Make sure the bone is still valid */
  bPoseChannel *pchan = BKE_pose_channel_find_name(par->pose, ob->parsubstr);
  if (!pchan || !pchan->bone) {
    CLOG_WARN(
        &LOG, "Parent Bone: '%s' for Object: '%s' doesn't exist", ob->parsubstr, ob->id.name + 2);
    unit_m4(r_mat);
    return;
  }

  /* get bone transform */
  if (pchan->bone->flag & BONE_RELATIVE_PARENTING) {
    /* the new option uses the root - expected behavior, but differs from old... */
    /* XXX check on version patching? */
    copy_m4_m4(r_mat, pchan->chan_mat);
  }
  else {
    copy_m4_m4(r_mat, pchan->pose_mat);

    /* but for backwards compatibility, the child has to move to the tail */
    copy_v3_v3(vec, r_mat[1]);
    mul_v3_fl(vec, pchan->bone->length);
    add_v3_v3(r_mat[3], vec);
  }
}

static void give_parvert(Object *par, int nr, float vec[3])
{
  zero_v3(vec);

  if (par->type == OB_MESH) {
    Mesh *me = (Mesh *)par->data;
    BMEditMesh *em = me->edit_mesh;
    Mesh *me_eval = (em) ? BKE_object_get_editmesh_eval_final(par) :
                           BKE_object_get_evaluated_mesh(par);

    if (me_eval) {
      const Span<float3> positions = me_eval->vert_positions();
      int count = 0;
      int numVerts = me_eval->totvert;

      if (em && me_eval->runtime->wrapper_type == ME_WRAPPER_TYPE_BMESH) {
        numVerts = em->bm->totvert;
        if (em->bm->elem_table_dirty & BM_VERT) {
#ifdef VPARENT_THREADING_HACK
          BLI_mutex_lock(&vparent_lock);
          if (em->bm->elem_table_dirty & BM_VERT) {
            BM_mesh_elem_table_ensure(em->bm, BM_VERT);
          }
          BLI_mutex_unlock(&vparent_lock);
#else
          BLI_assert_msg(0, "Not safe for threading");
          BM_mesh_elem_table_ensure(em->bm, BM_VERT);
#endif
        }
        if (nr < numVerts) {
          if (me_eval && me_eval->runtime->edit_data && me_eval->runtime->edit_data->vertexCos) {
            add_v3_v3(vec, me_eval->runtime->edit_data->vertexCos[nr]);
          }
          else {
            const BMVert *v = BM_vert_at_index(em->bm, nr);
            add_v3_v3(vec, v->co);
          }
          count++;
        }
      }
      else if (CustomData_has_layer(&me_eval->vdata, CD_ORIGINDEX)) {
        const int *index = (const int *)CustomData_get_layer(&me_eval->vdata, CD_ORIGINDEX);
        /* Get the average of all verts with (original index == nr). */
        for (int i = 0; i < numVerts; i++) {
          if (index[i] == nr) {
            add_v3_v3(vec, positions[i]);
            count++;
          }
        }
      }
      else {
        if (nr < numVerts) {
          add_v3_v3(vec, positions[nr]);
          count++;
        }
      }

      if (count == 0) {
        /* keep as 0, 0, 0 */
      }
      else if (count > 0) {
        mul_v3_fl(vec, 1.0f / count);
      }
      else {
        /* use first index if its out of range */
        if (me_eval->totvert) {
          copy_v3_v3(vec, positions[0]);
        }
      }
    }
    else {
      CLOG_ERROR(&LOG,
                 "Evaluated mesh is needed to solve parenting, "
                 "object position can be wrong now");
    }
  }
  else if (ELEM(par->type, OB_CURVES_LEGACY, OB_SURF)) {
    ListBase *nurb;

    /* It is possible that a cycle in the dependency graph was resolved in a way that caused this
     * object to be evaluated before its dependencies. In this case the curve cache may be null. */
    if (par->runtime.curve_cache && par->runtime.curve_cache->deformed_nurbs.first != nullptr) {
      nurb = &par->runtime.curve_cache->deformed_nurbs;
    }
    else {
      Curve *cu = (Curve *)par->data;
      nurb = BKE_curve_nurbs_get(cu);
    }

    BKE_nurbList_index_get_co(nurb, nr, vec);
  }
  else if (par->type == OB_LATTICE) {
    Lattice *latt = (Lattice *)par->data;
    DispList *dl = par->runtime.curve_cache ?
                       BKE_displist_find(&par->runtime.curve_cache->disp, DL_VERTS) :
                       nullptr;
    float(*co)[3] = dl ? (float(*)[3])dl->verts : nullptr;
    int tot;

    if (latt->editlatt) {
      latt = latt->editlatt->latt;
    }

    tot = latt->pntsu * latt->pntsv * latt->pntsw;

    /* ensure dl is correct size */
    BLI_assert(dl == nullptr || dl->nr == tot);

    if (nr < tot) {
      if (co) {
        copy_v3_v3(vec, co[nr]);
      }
      else {
        copy_v3_v3(vec, latt->def[nr].vec);
      }
    }
  }
}

static void ob_parvert3(Object *ob, Object *par, float r_mat[4][4])
{
  /* in local ob space */
  if (OB_TYPE_SUPPORT_PARVERT(par->type)) {
    float cmat[3][3], v1[3], v2[3], v3[3], q[4];

    give_parvert(par, ob->par1, v1);
    give_parvert(par, ob->par2, v2);
    give_parvert(par, ob->par3, v3);

    tri_to_quat(q, v1, v2, v3);
    quat_to_mat3(cmat, q);
    copy_m4_m3(r_mat, cmat);

    mid_v3_v3v3v3(r_mat[3], v1, v2, v3);
  }
  else {
    unit_m4(r_mat);
  }
}

void BKE_object_get_parent_matrix(Object *ob, Object *par, float r_parentmat[4][4])
{
  float tmat[4][4];
  float vec[3];

  switch (ob->partype & PARTYPE) {
    case PAROBJECT: {
      bool ok = false;
      if (par->type == OB_CURVES_LEGACY) {
        if ((((Curve *)par->data)->flag & CU_PATH) && ob_parcurve(ob, par, tmat)) {
          ok = true;
        }
      }

      if (ok) {
        mul_m4_m4m4(r_parentmat, par->object_to_world, tmat);
      }
      else {
        copy_m4_m4(r_parentmat, par->object_to_world);
      }

      break;
    }
    case PARBONE:
      ob_parbone(ob, par, tmat);
      mul_m4_m4m4(r_parentmat, par->object_to_world, tmat);
      break;

    case PARVERT1:
      unit_m4(r_parentmat);
      give_parvert(par, ob->par1, vec);
      mul_v3_m4v3(r_parentmat[3], par->object_to_world, vec);
      break;
    case PARVERT3:
      ob_parvert3(ob, par, tmat);

      mul_m4_m4m4(r_parentmat, par->object_to_world, tmat);
      break;

    case PARSKEL:
      copy_m4_m4(r_parentmat, par->object_to_world);
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Matrix Evaluation API
 * \{ */

/**
 * \param r_originmat: Optional matrix that stores the space the object is in
 * (without its own matrix applied)
 */
static void solve_parenting(
    Object *ob, Object *par, const bool set_origin, float r_obmat[4][4], float r_originmat[3][3])
{
  float totmat[4][4];
  float tmat[4][4];
  float locmat[4][4];

  BKE_object_to_mat4(ob, locmat);

  BKE_object_get_parent_matrix(ob, par, totmat);

  /* total */
  mul_m4_m4m4(tmat, totmat, ob->parentinv);
  mul_m4_m4m4(r_obmat, tmat, locmat);

  if (r_originmat) {
    /* Usable `r_originmat`. */
    copy_m3_m4(r_originmat, tmat);
  }

  /* origin, for help line */
  if (set_origin) {
    if ((ob->partype & PARTYPE) == PARSKEL) {
      copy_v3_v3(ob->runtime.parent_display_origin, par->object_to_world[3]);
    }
    else {
      copy_v3_v3(ob->runtime.parent_display_origin, totmat[3]);
    }
  }
}

static void object_where_is_calc_ex(Depsgraph *depsgraph,
                                    Scene *scene,
                                    Object *ob,
                                    float ctime,
                                    RigidBodyWorld *rbw,
                                    float r_originmat[3][3])
{
  if (ob->parent) {
    Object *par = ob->parent;

    /* calculate parent matrix */
    solve_parenting(ob, par, true, ob->object_to_world, r_originmat);
  }
  else {
    BKE_object_to_mat4(ob, ob->object_to_world);
  }

  /* try to fall back to the scene rigid body world if none given */
  rbw = rbw ? rbw : scene->rigidbody_world;
  /* read values pushed into RBO from sim/cache... */
  BKE_rigidbody_sync_transforms(rbw, ob, ctime);

  /* solve constraints */
  if (ob->constraints.first && !(ob->transflag & OB_NO_CONSTRAINTS)) {
    bConstraintOb *cob;
    cob = BKE_constraints_make_evalob(depsgraph, scene, ob, nullptr, CONSTRAINT_OBTYPE_OBJECT);
    BKE_constraints_solve(depsgraph, &ob->constraints, cob, ctime);
    BKE_constraints_clear_evalob(cob);
  }

  /* set negative scale flag in object */
  if (is_negative_m4(ob->object_to_world)) {
    ob->transflag |= OB_NEG_SCALE;
  }
  else {
    ob->transflag &= ~OB_NEG_SCALE;
  }
}

void BKE_object_where_is_calc_time(Depsgraph *depsgraph, Scene *scene, Object *ob, float ctime)
{
  /* Execute drivers and animation. */
  const bool flush_to_original = DEG_is_active(depsgraph);
  const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(depsgraph,
                                                                                    ctime);
  BKE_animsys_evaluate_animdata(
      &ob->id, ob->adt, &anim_eval_context, ADT_RECALC_ALL, flush_to_original);
  object_where_is_calc_ex(depsgraph, scene, ob, ctime, nullptr, nullptr);
}

void BKE_object_where_is_calc_mat4(Object *ob, float r_obmat[4][4])
{
  if (ob->parent) {
    Object *par = ob->parent;
    solve_parenting(ob, par, false, r_obmat, nullptr);
  }
  else {
    BKE_object_to_mat4(ob, r_obmat);
  }
}

void BKE_object_where_is_calc_ex(
    Depsgraph *depsgraph, Scene *scene, RigidBodyWorld *rbw, Object *ob, float r_originmat[3][3])
{
  float ctime = DEG_get_ctime(depsgraph);
  object_where_is_calc_ex(depsgraph, scene, ob, ctime, rbw, r_originmat);
}
void BKE_object_where_is_calc(Depsgraph *depsgraph, Scene *scene, Object *ob)
{
  float ctime = DEG_get_ctime(depsgraph);
  object_where_is_calc_ex(depsgraph, scene, ob, ctime, nullptr, nullptr);
}

void BKE_object_workob_calc_parent(Depsgraph *depsgraph, Scene *scene, Object *ob, Object *workob)
{
  BKE_object_workob_clear(workob);

  unit_m4(workob->object_to_world);
  unit_m4(workob->parentinv);
  unit_m4(workob->constinv);

  /* Since this is used while calculating parenting,
   * at this moment ob_eval->parent is still nullptr. */
  workob->parent = DEG_get_evaluated_object(depsgraph, ob->parent);

  workob->trackflag = ob->trackflag;
  workob->upflag = ob->upflag;

  workob->partype = ob->partype;
  workob->par1 = ob->par1;
  workob->par2 = ob->par2;
  workob->par3 = ob->par3;

  /* The effects of constraints should NOT be included in the parent-inverse matrix. Constraints
   * are supposed to be applied after the object's local loc/rot/scale. If the (inverted) effect of
   * constraints would be included in the parent inverse matrix, these would be applied before the
   * object's local loc/rot/scale instead of after. For example, a "Copy Rotation" constraint would
   * rotate the object's local translation as well. See #82156. */

  STRNCPY(workob->parsubstr, ob->parsubstr);

  BKE_object_where_is_calc(depsgraph, scene, workob);
}

void BKE_object_apply_mat4_ex(Object *ob,
                              const float mat[4][4],
                              Object *parent,
                              const float parentinv[4][4],
                              const bool use_compat)
{
  /* see BKE_pchan_apply_mat4() for the equivalent 'pchan' function */

  float rot[3][3];

  if (parent != nullptr) {
    float rmat[4][4], diff_mat[4][4], imat[4][4], parent_mat[4][4];

    BKE_object_get_parent_matrix(ob, parent, parent_mat);

    mul_m4_m4m4(diff_mat, parent_mat, parentinv);
    invert_m4_m4(imat, diff_mat);
    mul_m4_m4m4(rmat, imat, mat); /* get the parent relative matrix */

    /* same as below, use rmat rather than mat */
    mat4_to_loc_rot_size(ob->loc, rot, ob->scale, rmat);
  }
  else {
    mat4_to_loc_rot_size(ob->loc, rot, ob->scale, mat);
  }

  BKE_object_mat3_to_rot(ob, rot, use_compat);

  sub_v3_v3(ob->loc, ob->dloc);

  if (ob->dscale[0] != 0.0f) {
    ob->scale[0] /= ob->dscale[0];
  }
  if (ob->dscale[1] != 0.0f) {
    ob->scale[1] /= ob->dscale[1];
  }
  if (ob->dscale[2] != 0.0f) {
    ob->scale[2] /= ob->dscale[2];
  }

  /* BKE_object_mat3_to_rot handles delta rotations */
}

void BKE_object_apply_mat4(Object *ob,
                           const float mat[4][4],
                           const bool use_compat,
                           const bool use_parent)
{
  BKE_object_apply_mat4_ex(ob, mat, use_parent ? ob->parent : nullptr, ob->parentinv, use_compat);
}

void BKE_object_apply_parent_inverse(Object *ob)
{
  /*
   * Use parent's world transform as the child's origin.
   *
   * Let:
   *    `local = identity`
   *    `world = orthonormalized(parent)`
   *
   * Then:
   *    `world = parent @ parentinv @ local`
   *    `inv(parent) @ world = parentinv`
   *    `parentinv = inv(parent) @ world`
   *
   * NOTE: If `ob->object_to_world` has shear, then this `parentinv` is insufficient because
   *    `parent @ parentinv => shearless result`
   *
   *    Thus, local will have shear which cannot be decomposed into TRS:
   *    `local = inv(parent @ parentinv) @ world`
   *
   *    This is currently not supported for consistency in the handling of shear during the other
   *    parenting ops: Parent (Keep Transform), Clear [Parent] and Keep Transform.
   */
  float par_locrot[4][4], par_imat[4][4];
  BKE_object_get_parent_matrix(ob, ob->parent, par_locrot);
  invert_m4_m4(par_imat, par_locrot);

  orthogonalize_m4_stable(par_locrot, 0, true);

  mul_m4_m4m4(ob->parentinv, par_imat, par_locrot);

  /* Now, preserve `world` given the new `parentinv`.
   *
   * `world = parent @ parentinv @ local`
   * `inv(parent) @ world = parentinv @ local`
   * `inv(parentinv) @ inv(parent) @ world = local`
   *
   * `local = inv(parentinv) @ inv(parent) @ world`
   */
  float ob_local[4][4];
  copy_m4_m4(ob_local, ob->parentinv);
  invert_m4(ob_local);
  mul_m4_m4_post(ob_local, par_imat);
  mul_m4_m4_post(ob_local, ob->object_to_world);

  /* Send use_compat=False so the rotation is predictable. */
  BKE_object_apply_mat4(ob, ob_local, false, false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Bounding Box API
 * \{ */

BoundBox *BKE_boundbox_alloc_unit()
{
  BoundBox *bb = MEM_cnew<BoundBox>(__func__);
  BKE_boundbox_init_from_minmax(bb, float3(-1), float3(1));
  return bb;
}

void BKE_boundbox_init_from_minmax(BoundBox *bb, const float min[3], const float max[3])
{
  bb->vec[0][0] = bb->vec[1][0] = bb->vec[2][0] = bb->vec[3][0] = min[0];
  bb->vec[4][0] = bb->vec[5][0] = bb->vec[6][0] = bb->vec[7][0] = max[0];

  bb->vec[0][1] = bb->vec[1][1] = bb->vec[4][1] = bb->vec[5][1] = min[1];
  bb->vec[2][1] = bb->vec[3][1] = bb->vec[6][1] = bb->vec[7][1] = max[1];

  bb->vec[0][2] = bb->vec[3][2] = bb->vec[4][2] = bb->vec[7][2] = min[2];
  bb->vec[1][2] = bb->vec[2][2] = bb->vec[5][2] = bb->vec[6][2] = max[2];
}

void BKE_boundbox_calc_center_aabb(const BoundBox *bb, float r_cent[3])
{
  r_cent[0] = 0.5f * (bb->vec[0][0] + bb->vec[4][0]);
  r_cent[1] = 0.5f * (bb->vec[0][1] + bb->vec[2][1]);
  r_cent[2] = 0.5f * (bb->vec[0][2] + bb->vec[1][2]);
}

void BKE_boundbox_calc_size_aabb(const BoundBox *bb, float r_size[3])
{
  r_size[0] = 0.5f * fabsf(bb->vec[0][0] - bb->vec[4][0]);
  r_size[1] = 0.5f * fabsf(bb->vec[0][1] - bb->vec[2][1]);
  r_size[2] = 0.5f * fabsf(bb->vec[0][2] - bb->vec[1][2]);
}

void BKE_boundbox_minmax(const BoundBox *bb,
                         const float obmat[4][4],
                         float r_min[3],
                         float r_max[3])
{
  int i;
  for (i = 0; i < 8; i++) {
    float vec[3];
    mul_v3_m4v3(vec, obmat, bb->vec[i]);
    minmax_v3v3_v3(r_min, r_max, vec);
  }
}

const BoundBox *BKE_object_boundbox_get(Object *ob)
{
  BoundBox *bb = nullptr;

  switch (ob->type) {
    case OB_MESH:
      bb = BKE_mesh_boundbox_get(ob);
      break;
    case OB_CURVES_LEGACY:
    case OB_SURF:
    case OB_FONT:
      bb = BKE_curve_boundbox_get(ob);
      break;
    case OB_MBALL:
      bb = BKE_mball_boundbox_get(ob);
      break;
    case OB_LATTICE:
      bb = BKE_lattice_boundbox_get(ob);
      break;
    case OB_ARMATURE:
      bb = BKE_armature_boundbox_get(ob);
      break;
    case OB_GPENCIL_LEGACY:
      bb = BKE_gpencil_boundbox_get(ob);
      break;
    case OB_CURVES:
      bb = BKE_curves_boundbox_get(ob);
      break;
    case OB_POINTCLOUD:
      bb = BKE_pointcloud_boundbox_get(ob);
      break;
    case OB_VOLUME:
      bb = BKE_volume_boundbox_get(ob);
      break;
    case OB_GREASE_PENCIL:
      bb = BKE_grease_pencil_boundbox_get(ob);
    default:
      break;
  }
  return bb;
}

void BKE_object_boundbox_calc_from_mesh(Object *ob, const Mesh *me_eval)
{
  float3 min(FLT_MAX);
  float3 max(-FLT_MAX);

  if (!BKE_mesh_wrapper_minmax(me_eval, min, max)) {
    min = float3(0);
    max = float3(0);
  }

  if (ob->runtime.bb == nullptr) {
    ob->runtime.bb = MEM_cnew<BoundBox>("DM-BoundBox");
  }

  BKE_boundbox_init_from_minmax(ob->runtime.bb, min, max);

  ob->runtime.bb->flag &= ~BOUNDBOX_DIRTY;
}

bool BKE_object_boundbox_calc_from_evaluated_geometry(Object *ob)
{
  using namespace blender;

  std::optional<Bounds<float3>> bounds;
  if (ob->runtime.geometry_set_eval) {
    bounds = ob->runtime.geometry_set_eval->compute_boundbox_without_instances();
  }
  else if (const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob)) {
    Bounds<float3> mesh_bounds{float3(std::numeric_limits<float>::max()),
                               float3(std::numeric_limits<float>::min())};
    if (BKE_mesh_wrapper_minmax(mesh_eval, mesh_bounds.min, mesh_bounds.max)) {
      bounds = bounds::merge(bounds, {mesh_bounds});
    }
  }
  else {
    return false;
  }

  if (ob->runtime.bb == nullptr) {
    ob->runtime.bb = MEM_cnew<BoundBox>(__func__);
  }
  if (bounds) {
    BKE_boundbox_init_from_minmax(ob->runtime.bb, bounds->min, bounds->max);
  }
  else {
    BKE_boundbox_init_from_minmax(ob->runtime.bb, float3(0), float3(0));
  }

  ob->runtime.bb->flag &= ~BOUNDBOX_DIRTY;

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Dimension Get/Set
 *
 * \warning Setting dimensions is prone to feedback loops in evaluation.
 * \{ */

void BKE_object_dimensions_get(Object *ob, float r_vec[3])
{
  const BoundBox *bb = BKE_object_boundbox_get(ob);
  if (bb) {
    float3 scale;
    mat4_to_size(scale, ob->object_to_world);

    r_vec[0] = fabsf(scale[0]) * (bb->vec[4][0] - bb->vec[0][0]);
    r_vec[1] = fabsf(scale[1]) * (bb->vec[2][1] - bb->vec[0][1]);
    r_vec[2] = fabsf(scale[2]) * (bb->vec[1][2] - bb->vec[0][2]);
  }
  else {
    zero_v3(r_vec);
  }
}

void BKE_object_dimensions_set_ex(Object *ob,
                                  const float value[3],
                                  int axis_mask,
                                  const float ob_scale_orig[3],
                                  const float ob_obmat_orig[4][4])
{
  const BoundBox *bb = BKE_object_boundbox_get(ob);
  if (bb) {
    float3 len;
    len.x = bb->vec[4][0] - bb->vec[0][0];
    len.y = bb->vec[2][1] - bb->vec[0][1];
    len.z = bb->vec[1][2] - bb->vec[0][2];

    for (int i = 0; i < 3; i++) {
      if (((1 << i) & axis_mask) == 0) {

        if (ob_scale_orig != nullptr) {
          const float scale_delta = len_v3(ob_obmat_orig[i]) / ob_scale_orig[i];
          if (isfinite(scale_delta)) {
            len[i] *= scale_delta;
          }
        }

        const float scale = copysignf(value[i] / len[i], ob->scale[i]);
        if (isfinite(scale)) {
          ob->scale[i] = scale;
        }
      }
    }
  }
}

void BKE_object_dimensions_set(Object *ob, const float value[3], int axis_mask)
{
  BKE_object_dimensions_set_ex(ob, value, axis_mask, nullptr, nullptr);
}

void BKE_object_minmax(Object *ob, float r_min[3], float r_max[3], const bool use_hidden)
{
  bool changed = false;

  switch (ob->type) {
    case OB_CURVES_LEGACY:
    case OB_FONT:
    case OB_SURF: {
      const BoundBox bb = *BKE_curve_boundbox_get(ob);
      BKE_boundbox_minmax(&bb, ob->object_to_world, r_min, r_max);
      changed = true;
      break;
    }
    case OB_MESH: {
      const BoundBox bb = *BKE_mesh_boundbox_get(ob);
      BKE_boundbox_minmax(&bb, ob->object_to_world, r_min, r_max);
      changed = true;
      break;
    }
    case OB_GPENCIL_LEGACY: {
      const BoundBox bb = *BKE_gpencil_boundbox_get(ob);
      BKE_boundbox_minmax(&bb, ob->object_to_world, r_min, r_max);
      changed = true;
      break;
    }
    case OB_LATTICE: {
      Lattice *lt = (Lattice *)ob->data;
      BPoint *bp = lt->def;
      int u, v, w;

      for (w = 0; w < lt->pntsw; w++) {
        for (v = 0; v < lt->pntsv; v++) {
          for (u = 0; u < lt->pntsu; u++, bp++) {
            float3 vec;
            mul_v3_m4v3(vec, ob->object_to_world, bp->vec);
            minmax_v3v3_v3(r_min, r_max, vec);
          }
        }
      }
      changed = true;
      break;
    }
    case OB_ARMATURE: {
      changed = BKE_pose_minmax(ob, r_min, r_max, use_hidden, false);
      break;
    }
    case OB_MBALL: {
      float ob_min[3], ob_max[3];

      changed = BKE_mball_minmax_ex(
          (const MetaBall *)ob->data, ob_min, ob_max, ob->object_to_world, 0);
      if (changed) {
        minmax_v3v3_v3(r_min, r_max, ob_min);
        minmax_v3v3_v3(r_min, r_max, ob_max);
      }
      break;
    }
    case OB_CURVES: {
      const BoundBox bb = *BKE_curves_boundbox_get(ob);
      BKE_boundbox_minmax(&bb, ob->object_to_world, r_min, r_max);
      changed = true;
      break;
    }
    case OB_POINTCLOUD: {
      const BoundBox bb = *BKE_pointcloud_boundbox_get(ob);
      BKE_boundbox_minmax(&bb, ob->object_to_world, r_min, r_max);
      changed = true;
      break;
    }
    case OB_VOLUME: {
      const BoundBox bb = *BKE_volume_boundbox_get(ob);
      BKE_boundbox_minmax(&bb, ob->object_to_world, r_min, r_max);
      changed = true;
      break;
    }
    case OB_GREASE_PENCIL: {
      const BoundBox bb = *BKE_grease_pencil_boundbox_get(ob);
      BKE_boundbox_minmax(&bb, ob->object_to_world, r_min, r_max);
      changed = true;
      break;
    }
  }

  if (changed == false) {
    float3 size = ob->scale;

    copy_v3_v3(size, ob->scale);
    if (ob->type == OB_EMPTY) {
      size *= ob->empty_drawsize;
    }

    minmax_v3v3_v3(r_min, r_max, ob->object_to_world[3]);

    float3 vec;
    copy_v3_v3(vec, ob->object_to_world[3]);
    add_v3_v3(vec, size);
    minmax_v3v3_v3(r_min, r_max, vec);

    copy_v3_v3(vec, ob->object_to_world[3]);
    sub_v3_v3(vec, size);
    minmax_v3v3_v3(r_min, r_max, vec);
  }
}

void BKE_object_empty_draw_type_set(Object *ob, const int value)
{
  ob->empty_drawtype = value;

  if (ob->type == OB_EMPTY && ob->empty_drawtype == OB_EMPTY_IMAGE) {
    if (!ob->iuser) {
      ob->iuser = MEM_cnew<ImageUser>("image user");
      ob->iuser->flag |= IMA_ANIM_ALWAYS;
      ob->iuser->frames = 100;
      ob->iuser->sfra = 1;
    }
  }
  else {
    MEM_SAFE_FREE(ob->iuser);
  }
}

bool BKE_object_empty_image_frame_is_visible_in_view3d(const Object *ob, const RegionView3D *rv3d)
{
  const char visibility_flag = ob->empty_image_visibility_flag;
  if (rv3d->is_persp) {
    return (visibility_flag & OB_EMPTY_IMAGE_HIDE_PERSPECTIVE) == 0;
  }

  return (visibility_flag & OB_EMPTY_IMAGE_HIDE_ORTHOGRAPHIC) == 0;
}

bool BKE_object_empty_image_data_is_visible_in_view3d(const Object *ob, const RegionView3D *rv3d)
{
  /* Caller is expected to check this. */
  BLI_assert(BKE_object_empty_image_frame_is_visible_in_view3d(ob, rv3d));

  const char visibility_flag = ob->empty_image_visibility_flag;

  if ((visibility_flag & (OB_EMPTY_IMAGE_HIDE_BACK | OB_EMPTY_IMAGE_HIDE_FRONT)) != 0) {
    float eps, dot;
    if (rv3d->is_persp) {
      /* NOTE: we could normalize the 'view_dir' then use 'eps'
       * however the issue with empty objects being visible when viewed from the side
       * is only noticeable in orthographic views. */
      float3 view_dir;
      sub_v3_v3v3(view_dir, rv3d->viewinv[3], ob->object_to_world[3]);
      dot = dot_v3v3(ob->object_to_world[2], view_dir);
      eps = 0.0f;
    }
    else {
      dot = dot_v3v3(ob->object_to_world[2], rv3d->viewinv[2]);
      eps = 1e-5f;
    }
    if (visibility_flag & OB_EMPTY_IMAGE_HIDE_BACK) {
      if (dot < eps) {
        return false;
      }
    }
    if (visibility_flag & OB_EMPTY_IMAGE_HIDE_FRONT) {
      if (dot > -eps) {
        return false;
      }
    }
  }

  if (visibility_flag & OB_EMPTY_IMAGE_HIDE_NON_AXIS_ALIGNED) {
    float3 proj, ob_z_axis;
    normalize_v3_v3(ob_z_axis, ob->object_to_world[2]);
    project_plane_v3_v3v3(proj, ob_z_axis, rv3d->viewinv[2]);
    const float proj_length_sq = len_squared_v3(proj);
    if (proj_length_sq > 1e-5f) {
      return false;
    }
  }

  return true;
}

bool BKE_object_minmax_empty_drawtype(const Object *ob, float r_min[3], float r_max[3])
{
  BLI_assert(ob->type == OB_EMPTY);
  float3 min(0), max(0);

  bool ok = false;
  const float radius = ob->empty_drawsize;

  switch (ob->empty_drawtype) {
    case OB_ARROWS: {
      max = float3(radius);
      ok = true;
      break;
    }
    case OB_PLAINAXES:
    case OB_CUBE:
    case OB_EMPTY_SPHERE: {
      min = float3(-radius);
      max = float3(radius);
      ok = true;
      break;
    }
    case OB_CIRCLE: {
      max[0] = max[2] = radius;
      min[0] = min[2] = -radius;
      ok = true;
      break;
    }
    case OB_SINGLE_ARROW: {
      max[2] = radius;
      ok = true;
      break;
    }
    case OB_EMPTY_CONE: {
      min = float3(-radius, 0.0f, -radius);
      max = float3(radius, radius * 2.0f, radius);
      ok = true;
      break;
    }
    case OB_EMPTY_IMAGE: {
      const float *ofs = ob->ima_ofs;
      /* NOTE: this is the best approximation that can be calculated without loading the image. */
      min[0] = ofs[0] * radius;
      min[1] = ofs[1] * radius;
      max[0] = radius + (ofs[0] * radius);
      max[1] = radius + (ofs[1] * radius);
      /* Since the image aspect can shrink the bounds towards the object origin,
       * adjust the min/max to account for that. */
      for (int i = 0; i < 2; i++) {
        CLAMP_MAX(min[i], 0.0f);
        CLAMP_MIN(max[i], 0.0f);
      }
      ok = true;
      break;
    }
  }

  if (ok) {
    copy_v3_v3(r_min, min);
    copy_v3_v3(r_max, max);
  }
  return ok;
}

bool BKE_object_minmax_dupli(Depsgraph *depsgraph,
                             Scene *scene,
                             Object *ob,
                             float r_min[3],
                             float r_max[3],
                             const bool use_hidden)
{
  bool ok = false;
  if ((ob->transflag & OB_DUPLI) == 0 && ob->runtime.geometry_set_eval == nullptr) {
    return ok;
  }

  ListBase *lb = object_duplilist(depsgraph, scene, ob);
  LISTBASE_FOREACH (DupliObject *, dob, lb) {
    if (((use_hidden == false) && (dob->no_draw != 0)) || dob->ob_data == nullptr) {
      /* pass */
    }
    else {
      Object temp_ob = blender::dna::shallow_copy(*dob->ob);
      /* Do not modify the original bounding-box. */
      temp_ob.runtime.bb = nullptr;
      BKE_object_replace_data_on_shallow_copy(&temp_ob, dob->ob_data);
      const BoundBox *bb = BKE_object_boundbox_get(&temp_ob);

      if (bb) {
        int i;
        for (i = 0; i < 8; i++) {
          float3 vec;
          mul_v3_m4v3(vec, dob->mat, bb->vec[i]);
          minmax_v3v3_v3(r_min, r_max, vec);
        }

        ok = true;
      }

      MEM_SAFE_FREE(temp_ob.runtime.bb);
    }
  }
  free_object_duplilist(lb); /* does restore */

  return ok;
}

struct GPencilStrokePointIterData {
  const float (*obmat)[4];

  void (*point_func_cb)(const float co[3], void *user_data);
  void *user_data;
};

static void foreach_display_point_gpencil_stroke_fn(bGPDlayer * /*layer*/,
                                                    bGPDframe * /*frame*/,
                                                    bGPDstroke *stroke,
                                                    void *thunk)
{
  GPencilStrokePointIterData *iter_data = (GPencilStrokePointIterData *)thunk;
  {
    bGPDspoint *pt;
    int i;
    for (i = 0, pt = stroke->points; i < stroke->totpoints; i++, pt++) {
      float3 co;
      mul_v3_m4v3(co, iter_data->obmat, &pt->x);
      iter_data->point_func_cb(co, iter_data->user_data);
    }
  }
}

void BKE_object_foreach_display_point(Object *ob,
                                      const float obmat[4][4],
                                      void (*func_cb)(const float[3], void *),
                                      void *user_data)
{
  /* TODO: point-cloud and curves object support. */
  const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob);
  float3 co;

  if (mesh_eval != nullptr) {
    const Span<float3> positions = mesh_eval->vert_positions();
    for (const int i : positions.index_range()) {
      mul_v3_m4v3(co, obmat, positions[i]);
      func_cb(co, user_data);
    }
  }
  else if (ob->type == OB_GPENCIL_LEGACY) {
    GPencilStrokePointIterData iter_data{};
    iter_data.obmat = obmat;
    iter_data.point_func_cb = func_cb;
    iter_data.user_data = user_data;

    BKE_gpencil_visible_stroke_iter(
        (bGPdata *)ob->data, nullptr, foreach_display_point_gpencil_stroke_fn, &iter_data);
  }
  else if (ob->runtime.curve_cache && ob->runtime.curve_cache->disp.first) {
    LISTBASE_FOREACH (DispList *, dl, &ob->runtime.curve_cache->disp) {
      const float *v3 = dl->verts;
      int totvert = dl->nr;
      int i;

      for (i = 0; i < totvert; i++, v3 += 3) {
        mul_v3_m4v3(co, obmat, v3);
        func_cb(co, user_data);
      }
    }
  }
}

void BKE_scene_foreach_display_point(Depsgraph *depsgraph,
                                     void (*func_cb)(const float[3], void *),
                                     void *user_data)
{
  DEGObjectIterSettings deg_iter_settings{};
  deg_iter_settings.depsgraph = depsgraph;
  deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY | DEG_ITER_OBJECT_FLAG_VISIBLE |
                            DEG_ITER_OBJECT_FLAG_DUPLI;
  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, ob) {
    if ((ob->base_flag & BASE_SELECTED) != 0) {
      BKE_object_foreach_display_point(ob, ob->object_to_world, func_cb, user_data);
    }
  }
  DEG_OBJECT_ITER_END;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Transform Channels (Backup/Restore)
 * \{ */

/**
 * See struct members from #Object in DNA_object_types.h
 */
struct ObTfmBack {
  float loc[3], dloc[3];
  float scale[3], dscale[3];
  float rot[3], drot[3];
  float quat[4], dquat[4];
  float rotAxis[3], drotAxis[3];
  float rotAngle, drotAngle;
  float obmat[4][4];
  float parentinv[4][4];
  float constinv[4][4];
  float imat[4][4];
};

void *BKE_object_tfm_backup(Object *ob)
{
  ObTfmBack *obtfm = (ObTfmBack *)MEM_mallocN(sizeof(ObTfmBack), "ObTfmBack");
  copy_v3_v3(obtfm->loc, ob->loc);
  copy_v3_v3(obtfm->dloc, ob->dloc);
  copy_v3_v3(obtfm->scale, ob->scale);
  copy_v3_v3(obtfm->dscale, ob->dscale);
  copy_v3_v3(obtfm->rot, ob->rot);
  copy_v3_v3(obtfm->drot, ob->drot);
  copy_qt_qt(obtfm->quat, ob->quat);
  copy_qt_qt(obtfm->dquat, ob->dquat);
  copy_v3_v3(obtfm->rotAxis, ob->rotAxis);
  copy_v3_v3(obtfm->drotAxis, ob->drotAxis);
  obtfm->rotAngle = ob->rotAngle;
  obtfm->drotAngle = ob->drotAngle;
  copy_m4_m4(obtfm->obmat, ob->object_to_world);
  copy_m4_m4(obtfm->parentinv, ob->parentinv);
  copy_m4_m4(obtfm->constinv, ob->constinv);
  copy_m4_m4(obtfm->imat, ob->world_to_object);

  return (void *)obtfm;
}

void BKE_object_tfm_restore(Object *ob, void *obtfm_pt)
{
  ObTfmBack *obtfm = (ObTfmBack *)obtfm_pt;
  copy_v3_v3(ob->loc, obtfm->loc);
  copy_v3_v3(ob->dloc, obtfm->dloc);
  copy_v3_v3(ob->scale, obtfm->scale);
  copy_v3_v3(ob->dscale, obtfm->dscale);
  copy_v3_v3(ob->rot, obtfm->rot);
  copy_v3_v3(ob->drot, obtfm->drot);
  copy_qt_qt(ob->quat, obtfm->quat);
  copy_qt_qt(ob->dquat, obtfm->dquat);
  copy_v3_v3(ob->rotAxis, obtfm->rotAxis);
  copy_v3_v3(ob->drotAxis, obtfm->drotAxis);
  ob->rotAngle = obtfm->rotAngle;
  ob->drotAngle = obtfm->drotAngle;
  copy_m4_m4(ob->object_to_world, obtfm->obmat);
  copy_m4_m4(ob->parentinv, obtfm->parentinv);
  copy_m4_m4(ob->constinv, obtfm->constinv);
  copy_m4_m4(ob->world_to_object, obtfm->imat);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Evaluation/Update API
 * \{ */

void BKE_object_handle_update_ex(Depsgraph *depsgraph,
                                 Scene *scene,
                                 Object *ob,
                                 RigidBodyWorld *rbw)
{
  const ID *object_data = (ID *)ob->data;
  const bool recalc_object = (ob->id.recalc & ID_RECALC_ALL) != 0;
  const bool recalc_data = (object_data != nullptr) ?
                               ((object_data->recalc & ID_RECALC_ALL) != 0) :
                               false;
  if (!recalc_object && !recalc_data) {
    return;
  }
  /* Speed optimization for animation lookups. */
  if (ob->pose != nullptr) {
    BKE_pose_channels_hash_ensure(ob->pose);
    if (ob->pose->flag & POSE_CONSTRAINTS_NEED_UPDATE_FLAGS) {
      BKE_pose_update_constraint_flags(ob->pose);
    }
  }
  if (recalc_data) {
    if (ob->type == OB_ARMATURE) {
      /* this happens for reading old files and to match library armatures
       * with poses we do it ahead of BKE_object_where_is_calc to ensure animation
       * is evaluated on the rebuilt pose, otherwise we get incorrect poses
       * on file load */
      if (ob->pose == nullptr || (ob->pose->flag & POSE_RECALC)) {
        /* No need to pass `bmain` here, we assume we do not need to rebuild DEG from here. */
        BKE_pose_rebuild(nullptr, ob, (bArmature *)ob->data, true);
      }
    }
  }
  /* XXX new animsys warning: depsgraph tag ID_RECALC_GEOMETRY should not skip drivers,
   * which is only in BKE_object_where_is_calc now */
  /* XXX: should this case be ID_RECALC_TRANSFORM instead? */
  if (recalc_object || recalc_data) {
    if (G.debug & G_DEBUG_DEPSGRAPH_EVAL) {
      printf("recalcob %s\n", ob->id.name + 2);
    }
    BKE_object_where_is_calc_ex(depsgraph, scene, rbw, ob, nullptr);
  }

  if (recalc_data) {
    BKE_object_handle_data_update(depsgraph, scene, ob);
  }
}

void BKE_object_handle_update(Depsgraph *depsgraph, Scene *scene, Object *ob)
{
  BKE_object_handle_update_ex(depsgraph, scene, ob, nullptr);
}

void BKE_object_sculpt_data_create(Object *ob)
{
  BLI_assert((ob->sculpt == nullptr) && (ob->mode & OB_MODE_ALL_SCULPT));
  ob->sculpt = MEM_new<SculptSession>(__func__);
  ob->sculpt->mode_type = (eObjectMode)ob->mode;
}

bool BKE_object_obdata_texspace_get(Object *ob,
                                    char **r_texspace_flag,
                                    float **r_texspace_location,
                                    float **r_texspace_size)
{
  if (ob->data == nullptr) {
    return false;
  }

  switch (GS(((ID *)ob->data)->name)) {
    case ID_ME: {
      BKE_mesh_texspace_get_reference(
          (Mesh *)ob->data, r_texspace_flag, r_texspace_location, r_texspace_size);
      break;
    }
    case ID_CU_LEGACY: {
      Curve *cu = (Curve *)ob->data;
      BKE_curve_texspace_ensure(cu);
      if (r_texspace_flag) {
        *r_texspace_flag = &cu->texspace_flag;
      }
      if (r_texspace_location) {
        *r_texspace_location = cu->texspace_location;
      }
      if (r_texspace_size) {
        *r_texspace_size = cu->texspace_size;
      }
      break;
    }
    case ID_MB: {
      MetaBall *mb = (MetaBall *)ob->data;
      if (r_texspace_flag) {
        *r_texspace_flag = &mb->texspace_flag;
      }
      if (r_texspace_location) {
        *r_texspace_location = mb->texspace_location;
      }
      if (r_texspace_size) {
        *r_texspace_size = mb->texspace_size;
      }
      break;
    }
    default:
      return false;
  }
  return true;
}

Mesh *BKE_object_get_evaluated_mesh_no_subsurf(const Object *object)
{
  /* First attempt to retrieve the evaluated mesh from the evaluated geometry set. Most
   * object types either store it there or add a reference to it if it's owned elsewhere. */
  blender::bke::GeometrySet *geometry_set_eval = object->runtime.geometry_set_eval;
  if (geometry_set_eval) {
    /* Some areas expect to be able to modify the evaluated mesh in limited ways. Theoretically
     * this should be avoided, or at least protected with a lock, so a const mesh could be returned
     * from this function. We use a const_cast instead of #get_mesh_for_write, because that might
     * result in a copy of the mesh when it is shared. */
    Mesh *mesh = const_cast<Mesh *>(geometry_set_eval->get_mesh_for_read());
    if (mesh) {
      return mesh;
    }
  }

  /* Some object types do not yet add the evaluated mesh to an evaluated geometry set, if they do
   * not support evaluating to multiple data types. Eventually this should be removed, when all
   * object types use #geometry_set_eval. */
  ID *data_eval = object->runtime.data_eval;
  if (data_eval && GS(data_eval->name) == ID_ME) {
    return reinterpret_cast<Mesh *>(data_eval);
  }

  return nullptr;
}

Mesh *BKE_object_get_evaluated_mesh(const Object *object)
{
  Mesh *mesh = BKE_object_get_evaluated_mesh_no_subsurf(object);
  if (!mesh) {
    return nullptr;
  }

  if (object->data && GS(((const ID *)object->data)->name) == ID_ME) {
    mesh = BKE_mesh_wrapper_ensure_subdivision(mesh);
  }

  return mesh;
}

Mesh *BKE_object_get_pre_modified_mesh(const Object *object)
{
  if (object->type == OB_MESH && object->runtime.data_orig != nullptr) {
    BLI_assert(object->id.tag & LIB_TAG_COPIED_ON_WRITE);
    BLI_assert(object->id.orig_id != nullptr);
    BLI_assert(object->runtime.data_orig->orig_id == ((Object *)object->id.orig_id)->data);
    Mesh *result = (Mesh *)object->runtime.data_orig;
    BLI_assert((result->id.tag & LIB_TAG_COPIED_ON_WRITE) != 0);
    BLI_assert((result->id.tag & LIB_TAG_COPIED_ON_WRITE_EVAL_RESULT) == 0);
    return result;
  }
  BLI_assert((object->id.tag & LIB_TAG_COPIED_ON_WRITE) == 0);
  return (Mesh *)object->data;
}

Mesh *BKE_object_get_original_mesh(const Object *object)
{
  Mesh *result = nullptr;
  if (object->id.orig_id == nullptr) {
    BLI_assert((object->id.tag & LIB_TAG_COPIED_ON_WRITE) == 0);
    result = (Mesh *)object->data;
  }
  else {
    BLI_assert((object->id.tag & LIB_TAG_COPIED_ON_WRITE) != 0);
    result = (Mesh *)((Object *)object->id.orig_id)->data;
  }
  BLI_assert(result != nullptr);
  BLI_assert((result->id.tag & (LIB_TAG_COPIED_ON_WRITE | LIB_TAG_COPIED_ON_WRITE_EVAL_RESULT)) ==
             0);
  return result;
}

Mesh *BKE_object_get_editmesh_eval_final(const Object *object)
{
  BLI_assert(!DEG_is_original_id(&object->id));
  BLI_assert(object->type == OB_MESH);

  const Mesh *mesh = static_cast<const Mesh *>(object->data);
  if (mesh->edit_mesh == nullptr) {
    /* Happens when requesting material of evaluated 3d font object: the evaluated object get
     * converted to mesh, and it does not have edit mesh. */
    return nullptr;
  }

  return reinterpret_cast<Mesh *>(object->runtime.data_eval);
}

Mesh *BKE_object_get_editmesh_eval_cage(const Object *object)
{
  BLI_assert(!DEG_is_original_id(&object->id));
  BLI_assert(object->type == OB_MESH);

  const Mesh *mesh = static_cast<const Mesh *>(object->data);
  BLI_assert(mesh->edit_mesh != nullptr);
  UNUSED_VARS_NDEBUG(mesh);

  return object->runtime.editmesh_eval_cage;
}

Lattice *BKE_object_get_lattice(const Object *object)
{
  ID *data = (ID *)object->data;
  if (data == nullptr || GS(data->name) != ID_LT) {
    return nullptr;
  }

  Lattice *lt = (Lattice *)data;
  if (lt->editlatt) {
    return lt->editlatt->latt;
  }

  return lt;
}

Lattice *BKE_object_get_evaluated_lattice(const Object *object)
{
  ID *data_eval = object->runtime.data_eval;

  if (data_eval == nullptr || GS(data_eval->name) != ID_LT) {
    return nullptr;
  }

  Lattice *lt_eval = (Lattice *)data_eval;
  if (lt_eval->editlatt) {
    return lt_eval->editlatt->latt;
  }

  return lt_eval;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Point Cache
 * \{ */

static int pc_cmp(const void *a, const void *b)
{
  const LinkData *ad = (const LinkData *)a, *bd = (const LinkData *)b;
  if (POINTER_AS_INT(ad->data) > POINTER_AS_INT(bd->data)) {
    return 1;
  }

  return 0;
}

/* TODO: Review the usages of this function, currently with COW it will be called for orig object
 * and then again for COW copies of it, think this is bad since there is no guarantee that we get
 * the same stack index in both cases? Order is important since this index is used for filenames on
 * disk. */
int BKE_object_insert_ptcache(Object *ob)
{
  LinkData *link = nullptr;
  int i = 0;

  BLI_listbase_sort(&ob->pc_ids, pc_cmp);

  for (link = (LinkData *)ob->pc_ids.first, i = 0; link; link = link->next, i++) {
    int index = POINTER_AS_INT(link->data);

    if (i < index) {
      break;
    }
  }

  link = MEM_cnew<LinkData>("PCLink");
  link->data = POINTER_FROM_INT(i);
  BLI_addtail(&ob->pc_ids, link);

  return i;
}

static int pc_findindex(ListBase *listbase, int index)
{
  int number = 0;

  if (listbase == nullptr) {
    return -1;
  }

  LinkData *link = (LinkData *)listbase->first;
  while (link) {
    if (POINTER_AS_INT(link->data) == index) {
      return number;
    }

    number++;
    link = link->next;
  }

  return -1;
}

void BKE_object_delete_ptcache(Object *ob, int index)
{
  int list_index = pc_findindex(&ob->pc_ids, index);
  LinkData *link = (LinkData *)BLI_findlink(&ob->pc_ids, list_index);
  BLI_freelinkN(&ob->pc_ids, link);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Data Shape Key Insert
 * \{ */

/** Mesh */
static KeyBlock *insert_meshkey(Main *bmain, Object *ob, const char *name, const bool from_mix)
{
  Mesh *me = (Mesh *)ob->data;
  Key *key = me->key;
  KeyBlock *kb;
  int newkey = 0;

  if (key == nullptr) {
    key = me->key = BKE_key_add(bmain, (ID *)me);
    key->type = KEY_RELATIVE;
    newkey = 1;
  }

  if (newkey || from_mix == false) {
    /* create from mesh */
    kb = BKE_keyblock_add_ctime(key, name, false);
    BKE_keyblock_convert_from_mesh(me, key, kb);
  }
  else {
    /* copy from current values */
    int totelem;
    float *data = BKE_key_evaluate_object(ob, &totelem);

    /* create new block with prepared data */
    kb = BKE_keyblock_add_ctime(key, name, false);
    kb->data = data;
    kb->totelem = totelem;
  }

  return kb;
}
/** Lattice */
static KeyBlock *insert_lattkey(Main *bmain, Object *ob, const char *name, const bool from_mix)
{
  Lattice *lt = (Lattice *)ob->data;
  Key *key = lt->key;
  KeyBlock *kb;
  int newkey = 0;

  if (key == nullptr) {
    key = lt->key = BKE_key_add(bmain, (ID *)lt);
    key->type = KEY_RELATIVE;
    newkey = 1;
  }

  if (newkey || from_mix == false) {
    kb = BKE_keyblock_add_ctime(key, name, false);
    if (!newkey) {
      KeyBlock *basekb = (KeyBlock *)key->block.first;
      kb->data = MEM_dupallocN(basekb->data);
      kb->totelem = basekb->totelem;
    }
    else {
      BKE_keyblock_convert_from_lattice(lt, kb);
    }
  }
  else {
    /* copy from current values */
    int totelem;
    float *data = BKE_key_evaluate_object(ob, &totelem);

    /* create new block with prepared data */
    kb = BKE_keyblock_add_ctime(key, name, false);
    kb->totelem = totelem;
    kb->data = data;
  }

  return kb;
}
/** Curve */
static KeyBlock *insert_curvekey(Main *bmain, Object *ob, const char *name, const bool from_mix)
{
  Curve *cu = (Curve *)ob->data;
  Key *key = cu->key;
  KeyBlock *kb;
  ListBase *lb = BKE_curve_nurbs_get(cu);
  int newkey = 0;

  if (key == nullptr) {
    key = cu->key = BKE_key_add(bmain, (ID *)cu);
    key->type = KEY_RELATIVE;
    newkey = 1;
  }

  if (newkey || from_mix == false) {
    /* create from curve */
    kb = BKE_keyblock_add_ctime(key, name, false);
    if (!newkey) {
      KeyBlock *basekb = (KeyBlock *)key->block.first;
      kb->data = MEM_dupallocN(basekb->data);
      kb->totelem = basekb->totelem;
    }
    else {
      BKE_keyblock_convert_from_curve(cu, kb, lb);
    }
  }
  else {
    /* copy from current values */
    int totelem;
    float *data = BKE_key_evaluate_object(ob, &totelem);

    /* create new block with prepared data */
    kb = BKE_keyblock_add_ctime(key, name, false);
    kb->totelem = totelem;
    kb->data = data;
  }

  return kb;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Shape Key API
 * \{ */

KeyBlock *BKE_object_shapekey_insert(Main *bmain,
                                     Object *ob,
                                     const char *name,
                                     const bool from_mix)
{
  KeyBlock *key = nullptr;

  switch (ob->type) {
    case OB_MESH:
      key = insert_meshkey(bmain, ob, name, from_mix);
      break;
    case OB_CURVES_LEGACY:
    case OB_SURF:
      key = insert_curvekey(bmain, ob, name, from_mix);
      break;
    case OB_LATTICE:
      key = insert_lattkey(bmain, ob, name, from_mix);
      break;
    default:
      break;
  }

  /* Set the first active when none is set when called from RNA. */
  if (key != nullptr) {
    if (ob->shapenr <= 0) {
      ob->shapenr = 1;
    }
  }

  return key;
}

bool BKE_object_shapekey_free(Main *bmain, Object *ob)
{
  Key **key_p, *key;

  key_p = BKE_key_from_object_p(ob);
  if (ELEM(nullptr, key_p, *key_p)) {
    return false;
  }

  key = *key_p;
  *key_p = nullptr;

  BKE_id_free_us(bmain, key);

  return true;
}

bool BKE_object_shapekey_remove(Main *bmain, Object *ob, KeyBlock *kb)
{
  Key *key = BKE_key_from_object(ob);
  short kb_index;

  if (key == nullptr) {
    return false;
  }

  kb_index = BLI_findindex(&key->block, kb);
  BLI_assert(kb_index != -1);

  LISTBASE_FOREACH (KeyBlock *, rkb, &key->block) {
    if (rkb->relative == kb_index) {
      /* remap to the 'Basis' */
      rkb->relative = 0;
    }
    else if (rkb->relative >= kb_index) {
      /* Fix positional shift of the keys when kb is deleted from the list */
      rkb->relative -= 1;
    }
  }

  BLI_remlink(&key->block, kb);
  key->totkey--;
  if (key->refkey == kb) {
    key->refkey = (KeyBlock *)key->block.first;

    if (key->refkey) {
      /* apply new basis key on original data */
      switch (ob->type) {
        case OB_MESH: {
          Mesh *mesh = (Mesh *)ob->data;
          MutableSpan<float3> positions = mesh->vert_positions_for_write();
          BKE_keyblock_convert_to_mesh(
              key->refkey, reinterpret_cast<float(*)[3]>(positions.data()), mesh->totvert);
          break;
        }
        case OB_CURVES_LEGACY:
        case OB_SURF:
          BKE_keyblock_convert_to_curve(
              key->refkey, (Curve *)ob->data, BKE_curve_nurbs_get((Curve *)ob->data));
          break;
        case OB_LATTICE:
          BKE_keyblock_convert_to_lattice(key->refkey, (Lattice *)ob->data);
          break;
      }
    }
  }

  if (kb->data) {
    MEM_freeN(kb->data);
  }
  MEM_freeN(kb);

  /* Unset active when all are freed. */
  if (BLI_listbase_is_empty(&key->block)) {
    ob->shapenr = 0;
  }
  else if (ob->shapenr > 1) {
    ob->shapenr--;
  }

  if (key->totkey == 0) {
    BKE_object_shapekey_free(bmain, ob);
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Query API
 * \{ */

bool BKE_object_parent_loop_check(const Object *par, const Object *ob)
{
  /* test if 'ob' is a parent somewhere in par's parents */
  if (par == nullptr) {
    return false;
  }
  if (ob == par) {
    return true;
  }
  return BKE_object_parent_loop_check(par->parent, ob);
}

bool BKE_object_flag_test_recursive(const Object *ob, short flag)
{
  if (ob->flag & flag) {
    return true;
  }
  if (ob->parent) {
    return BKE_object_flag_test_recursive(ob->parent, flag);
  }

  return false;
}

bool BKE_object_is_child_recursive(const Object *ob_parent, const Object *ob_child)
{
  for (ob_child = ob_child->parent; ob_child; ob_child = ob_child->parent) {
    if (ob_child == ob_parent) {
      return true;
    }
  }
  return false;
}

int BKE_object_is_modified(Scene *scene, Object *ob)
{
  /* Always test on original object since evaluated object may no longer
   * have shape keys or modifiers that were used to evaluate it. */
  ob = DEG_get_original_object(ob);

  int flag = 0;

  if (BKE_key_from_object(ob)) {
    flag |= eModifierMode_Render | eModifierMode_Realtime;
  }
  else {
    ModifierData *md;
    VirtualModifierData virtualModifierData;
    /* cloth */
    for (md = BKE_modifiers_get_virtual_modifierlist(ob, &virtualModifierData);
         md && (flag != (eModifierMode_Render | eModifierMode_Realtime));
         md = md->next)
    {
      if ((flag & eModifierMode_Render) == 0 &&
          BKE_modifier_is_enabled(scene, md, eModifierMode_Render)) {
        flag |= eModifierMode_Render;
      }

      if ((flag & eModifierMode_Realtime) == 0 &&
          BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime))
      {
        flag |= eModifierMode_Realtime;
      }
    }
  }

  return flag;
}

bool BKE_object_moves_in_time(const Object *object, bool recurse_parent)
{
  /* If object has any sort of animation data assume it is moving. */
  if (BKE_animdata_id_is_animated(&object->id)) {
    return true;
  }
  if (!BLI_listbase_is_empty(&object->constraints)) {
    return true;
  }
  if (recurse_parent && object->parent != nullptr) {
    return BKE_object_moves_in_time(object->parent, true);
  }
  return false;
}

static bool object_moves_in_time(const Object *object)
{
  return BKE_object_moves_in_time(object, true);
}

static bool object_deforms_in_time(Object *object)
{
  if (BKE_key_from_object(object) != nullptr) {
    return true;
  }
  if (!BLI_listbase_is_empty(&object->modifiers)) {
    return true;
  }
  return object_moves_in_time(object);
}

static bool constructive_modifier_is_deform_modified(Object *ob, ModifierData *md)
{
  /* TODO(sergey): Consider generalizing this a bit so all modifier logic
   * is concentrated in MOD_{modifier}.c file,
   */
  if (md->type == eModifierType_Array) {
    ArrayModifierData *amd = (ArrayModifierData *)md;
    /* TODO(sergey): Check if curve is deformed. */
    return (amd->start_cap != nullptr && object_moves_in_time(amd->start_cap)) ||
           (amd->end_cap != nullptr && object_moves_in_time(amd->end_cap)) ||
           (amd->curve_ob != nullptr && object_moves_in_time(amd->curve_ob)) ||
           (amd->offset_ob != nullptr && object_moves_in_time(amd->offset_ob));
  }
  if (md->type == eModifierType_Mirror) {
    MirrorModifierData *mmd = (MirrorModifierData *)md;
    return mmd->mirror_ob != nullptr &&
           (object_moves_in_time(mmd->mirror_ob) || object_moves_in_time(ob));
  }
  if (md->type == eModifierType_Screw) {
    ScrewModifierData *smd = (ScrewModifierData *)md;
    return smd->ob_axis != nullptr && object_moves_in_time(smd->ob_axis);
  }
  if (md->type == eModifierType_MeshSequenceCache) {
    /* NOTE: Not ideal because it's unknown whether topology changes or not.
     * This will be detected later, so by assuming it's only deformation
     * going on here we allow baking deform-only mesh to Alembic and have
     * proper motion blur after that.
     */
    return true;
  }
  if (md->type == eModifierType_Nodes) {
    /* Not ideal for performance to always assume this is animated,
     * but hard to detect in general. The better long term solution is likely
     * to replace BKE_object_is_deform_modified by a test if the object was
     * modified by the depsgraph when changing frames. */
    return true;
  }
  return false;
}

static bool modifiers_has_animation_check(const Object *ob)
{
  /* TODO(sergey): This is a bit code duplication with depsgraph, but
   * would be nicer to solve this as a part of new dependency graph
   * work, so we avoid conflicts and so.
   */
  if (ob->adt != nullptr) {
    AnimData *adt = ob->adt;
    if (adt->action != nullptr) {
      LISTBASE_FOREACH (FCurve *, fcu, &adt->action->curves) {
        if (fcu->rna_path && strstr(fcu->rna_path, "modifiers[")) {
          return true;
        }
      }
    }
    LISTBASE_FOREACH (FCurve *, fcu, &adt->drivers) {
      if (fcu->rna_path && strstr(fcu->rna_path, "modifiers[")) {
        return true;
      }
    }
  }
  return false;
}

int BKE_object_is_deform_modified(Scene *scene, Object *ob)
{
  /* Always test on original object since evaluated object may no longer
   * have shape keys or modifiers that were used to evaluate it. */
  ob = DEG_get_original_object(ob);

  ModifierData *md;
  VirtualModifierData virtualModifierData;
  int flag = 0;
  const bool is_modifier_animated = modifiers_has_animation_check(ob);

  if (BKE_key_from_object(ob)) {
    flag |= eModifierMode_Realtime | eModifierMode_Render;
  }

  if (ob->type == OB_CURVES_LEGACY) {
    Curve *cu = (Curve *)ob->data;
    if (cu->taperobj != nullptr && object_deforms_in_time(cu->taperobj)) {
      flag |= eModifierMode_Realtime | eModifierMode_Render;
    }
  }

  /* cloth */
  for (md = BKE_modifiers_get_virtual_modifierlist(ob, &virtualModifierData);
       md && (flag != (eModifierMode_Render | eModifierMode_Realtime));
       md = md->next)
  {
    const ModifierTypeInfo *mti = BKE_modifier_get_info((const ModifierType)md->type);
    bool can_deform = mti->type == eModifierTypeType_OnlyDeform || is_modifier_animated;

    if (!can_deform) {
      can_deform = constructive_modifier_is_deform_modified(ob, md);
    }

    if (can_deform) {
      if (!(flag & eModifierMode_Render) &&
          BKE_modifier_is_enabled(scene, md, eModifierMode_Render)) {
        flag |= eModifierMode_Render;
      }

      if (!(flag & eModifierMode_Realtime) &&
          BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
        flag |= eModifierMode_Realtime;
      }
    }
  }

  return flag;
}

int BKE_object_scenes_users_get(Main *bmain, Object *ob)
{
  int num_scenes = 0;
  LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
    if (BKE_collection_has_object_recursive(scene->master_collection, ob)) {
      num_scenes++;
    }
  }
  return num_scenes;
}

MovieClip *BKE_object_movieclip_get(Scene *scene, Object *ob, bool use_default)
{
  MovieClip *clip = use_default ? scene->clip : nullptr;
  bConstraint *con = (bConstraint *)ob->constraints.first, *scon = nullptr;

  while (con) {
    if (con->type == CONSTRAINT_TYPE_CAMERASOLVER) {
      if (scon == nullptr || (scon->flag & CONSTRAINT_OFF)) {
        scon = con;
      }
    }

    con = con->next;
  }

  if (scon) {
    bCameraSolverConstraint *solver = (bCameraSolverConstraint *)scon->data;
    if ((solver->flag & CAMERASOLVER_ACTIVECLIP) == 0) {
      clip = solver->clip;
    }
    else {
      clip = scene->clip;
    }
  }

  return clip;
}

bool BKE_object_supports_material_slots(Object *ob)
{
  return ELEM(ob->type,
              OB_MESH,
              OB_CURVES_LEGACY,
              OB_SURF,
              OB_FONT,
              OB_MBALL,
              OB_CURVES,
              OB_POINTCLOUD,
              OB_VOLUME,
              OB_GPENCIL_LEGACY,
              OB_GREASE_PENCIL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Runtime
 * \{ */

void BKE_object_runtime_reset(Object *object)
{
  memset(&object->runtime, 0, sizeof(object->runtime));
}

void BKE_object_runtime_reset_on_copy(Object *object, const int /*flag*/)
{
  Object_Runtime *runtime = &object->runtime;
  runtime->data_eval = nullptr;
  runtime->gpd_eval = nullptr;
  runtime->mesh_deform_eval = nullptr;
  runtime->curve_cache = nullptr;
  runtime->object_as_temp_mesh = nullptr;
  runtime->pose_backup = nullptr;
  runtime->object_as_temp_curve = nullptr;
  runtime->geometry_set_eval = nullptr;

  runtime->crazyspace_deform_imats = nullptr;
  runtime->crazyspace_deform_cos = nullptr;
}

void BKE_object_runtime_free_data(Object *object)
{
  /* Currently this is all that's needed. */
  BKE_object_free_derived_caches(object);

  BKE_object_runtime_reset(object);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Relationships
 * \{ */

/**
 * Find an associated armature object.
 */
static Object *obrel_armature_find(Object *ob)
{
  Object *ob_arm = nullptr;

  if (ob->parent && ob->partype == PARSKEL && ob->parent->type == OB_ARMATURE) {
    ob_arm = ob->parent;
  }
  else {
    ModifierData *mod;
    for (mod = (ModifierData *)ob->modifiers.first; mod; mod = mod->next) {
      if (mod->type == eModifierType_Armature) {
        ob_arm = ((ArmatureModifierData *)mod)->object;
      }
    }
  }

  return ob_arm;
}

static bool obrel_list_test(Object *ob)
{
  return ob && !(ob->id.tag & LIB_TAG_DOIT);
}

static void obrel_list_add(LinkNode **links, Object *ob)
{
  BLI_linklist_prepend(links, ob);
  ob->id.tag |= LIB_TAG_DOIT;
}

LinkNode *BKE_object_relational_superset(const Scene *scene,
                                         ViewLayer *view_layer,
                                         eObjectSet objectSet,
                                         eObRelationTypes includeFilter)
{
  LinkNode *links = nullptr;

  /* Remove markers from all objects */
  BKE_view_layer_synced_ensure(scene, view_layer);
  LISTBASE_FOREACH (Base *, base, BKE_view_layer_object_bases_get(view_layer)) {
    base->object->id.tag &= ~LIB_TAG_DOIT;
  }

  /* iterate over all selected and visible objects */
  LISTBASE_FOREACH (Base *, base, BKE_view_layer_object_bases_get(view_layer)) {
    if (objectSet == OB_SET_ALL) {
      /* as we get all anyways just add it */
      Object *ob = base->object;
      obrel_list_add(&links, ob);
    }
    else {
      if ((objectSet == OB_SET_SELECTED && BASE_SELECTED_EDITABLE(((View3D *)nullptr), base)) ||
          (objectSet == OB_SET_VISIBLE && BASE_EDITABLE(((View3D *)nullptr), base)))
      {
        Object *ob = base->object;

        if (obrel_list_test(ob)) {
          obrel_list_add(&links, ob);
        }

        /* parent relationship */
        if (includeFilter & (OB_REL_PARENT | OB_REL_PARENT_RECURSIVE)) {
          Object *parent = ob->parent;
          if (obrel_list_test(parent)) {

            obrel_list_add(&links, parent);

            /* recursive parent relationship */
            if (includeFilter & OB_REL_PARENT_RECURSIVE) {
              parent = parent->parent;
              while (obrel_list_test(parent)) {

                obrel_list_add(&links, parent);
                parent = parent->parent;
              }
            }
          }
        }

        /* child relationship */
        if (includeFilter & (OB_REL_CHILDREN | OB_REL_CHILDREN_RECURSIVE)) {
          LISTBASE_FOREACH (Base *, local_base, BKE_view_layer_object_bases_get(view_layer)) {
            if (BASE_EDITABLE(((View3D *)nullptr), local_base)) {

              Object *child = local_base->object;
              if (obrel_list_test(child)) {
                if ((includeFilter & OB_REL_CHILDREN_RECURSIVE &&
                     BKE_object_is_child_recursive(ob, child)) ||
                    (includeFilter & OB_REL_CHILDREN && child->parent && child->parent == ob))
                {
                  obrel_list_add(&links, child);
                }
              }
            }
          }
        }

        /* include related armatures */
        if (includeFilter & OB_REL_MOD_ARMATURE) {
          Object *arm = obrel_armature_find(ob);
          if (obrel_list_test(arm)) {
            obrel_list_add(&links, arm);
          }
        }
      }
    }
  }

  return links;
}

LinkNode *BKE_object_groups(Main *bmain, Scene *scene, Object *ob)
{
  LinkNode *collection_linknode = nullptr;
  Collection *collection = nullptr;
  while ((collection = BKE_collection_object_find(bmain, scene, collection, ob))) {
    BLI_linklist_prepend(&collection_linknode, collection);
  }

  return collection_linknode;
}

void BKE_object_groups_clear(Main *bmain, Scene *scene, Object *ob)
{
  Collection *collection = nullptr;
  while ((collection = BKE_collection_object_find(bmain, scene, collection, ob))) {
    BKE_collection_object_remove(bmain, collection, ob, false);
    DEG_id_tag_update(&collection->id, ID_RECALC_COPY_ON_WRITE);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object KD-Tree
 * \{ */

KDTree_3d *BKE_object_as_kdtree(Object *ob, int *r_tot)
{
  KDTree_3d *tree = nullptr;
  uint tot = 0;

  switch (ob->type) {
    case OB_MESH: {
      Mesh *me = (Mesh *)ob->data;
      uint i;

      Mesh *me_eval = ob->runtime.mesh_deform_eval ? ob->runtime.mesh_deform_eval :
                                                     BKE_object_get_evaluated_mesh(ob);
      const int *index;

      if (me_eval && (index = (const int *)CustomData_get_layer(&me_eval->vdata, CD_ORIGINDEX))) {
        const Span<float3> positions = me->vert_positions();

        /* Tree over-allocates in case where some verts have #ORIGINDEX_NONE. */
        tot = 0;
        tree = BLI_kdtree_3d_new(positions.size());

        /* We don't how many verts from the DM we can use. */
        for (i = 0; i < positions.size(); i++) {
          if (index[i] != ORIGINDEX_NONE) {
            float co[3];
            mul_v3_m4v3(co, ob->object_to_world, positions[i]);
            BLI_kdtree_3d_insert(tree, index[i], co);
            tot++;
          }
        }
      }
      else {
        const Span<float3> positions = me->vert_positions();

        tot = positions.size();
        tree = BLI_kdtree_3d_new(tot);

        for (i = 0; i < tot; i++) {
          float co[3];
          mul_v3_m4v3(co, ob->object_to_world, positions[i]);
          BLI_kdtree_3d_insert(tree, i, co);
        }
      }

      BLI_kdtree_3d_balance(tree);
      break;
    }
    case OB_CURVES_LEGACY:
    case OB_SURF: {
      /* TODO: take deformation into account */
      Curve *cu = (Curve *)ob->data;
      uint i, a;

      Nurb *nu;

      tot = BKE_nurbList_verts_count_without_handles(&cu->nurb);
      tree = BLI_kdtree_3d_new(tot);
      i = 0;

      nu = (Nurb *)cu->nurb.first;
      while (nu) {
        if (nu->bezt) {
          BezTriple *bezt;

          bezt = nu->bezt;
          a = nu->pntsu;
          while (a--) {
            float co[3];
            mul_v3_m4v3(co, ob->object_to_world, bezt->vec[1]);
            BLI_kdtree_3d_insert(tree, i++, co);
            bezt++;
          }
        }
        else {
          BPoint *bp;

          bp = nu->bp;
          a = nu->pntsu * nu->pntsv;
          while (a--) {
            float co[3];
            mul_v3_m4v3(co, ob->object_to_world, bp->vec);
            BLI_kdtree_3d_insert(tree, i++, co);
            bp++;
          }
        }
        nu = nu->next;
      }

      BLI_kdtree_3d_balance(tree);
      break;
    }
    case OB_LATTICE: {
      /* TODO: take deformation into account */
      Lattice *lt = (Lattice *)ob->data;
      BPoint *bp;
      uint i;

      tot = lt->pntsu * lt->pntsv * lt->pntsw;
      tree = BLI_kdtree_3d_new(tot);
      i = 0;

      for (bp = lt->def; i < tot; bp++) {
        float co[3];
        mul_v3_m4v3(co, ob->object_to_world, bp->vec);
        BLI_kdtree_3d_insert(tree, i++, co);
      }

      BLI_kdtree_3d_balance(tree);
      break;
    }
  }

  *r_tot = tot;
  return tree;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Modifier Utilities
 * \{ */

/**
 * Set "ignore cache" flag for all caches on this object.
 */
static void object_cacheIgnoreClear(Object *ob, int state)
{
  ListBase pidlist;
  BKE_ptcache_ids_from_object(&pidlist, ob, nullptr, 0);

  LISTBASE_FOREACH (PTCacheID *, pid, &pidlist) {
    if (pid->cache) {
      if (state) {
        pid->cache->flag |= PTCACHE_IGNORE_CLEAR;
      }
      else {
        pid->cache->flag &= ~PTCACHE_IGNORE_CLEAR;
      }
    }
  }

  BLI_freelistN(&pidlist);
}

bool BKE_object_modifier_update_subframe(Depsgraph *depsgraph,
                                         Scene *scene,
                                         Object *ob,
                                         bool update_mesh,
                                         int parent_recursion,
                                         float frame,
                                         int type)
{
  const bool flush_to_original = DEG_is_active(depsgraph);
  ModifierData *md = BKE_modifiers_findby_type(ob, (ModifierType)type);

  if (type == eModifierType_DynamicPaint) {
    DynamicPaintModifierData *pmd = (DynamicPaintModifierData *)md;

    /* if other is dynamic paint canvas, don't update */
    if (pmd && pmd->canvas) {
      return true;
    }
  }
  else if (type == eModifierType_Fluid) {
    FluidModifierData *fmd = (FluidModifierData *)md;

    if (fmd && (fmd->type & MOD_FLUID_TYPE_DOMAIN) != 0) {
      return true;
    }
  }

  /* if object has parents, update them too */
  if (parent_recursion) {
    int recursion = parent_recursion - 1;
    bool no_update = false;
    if (ob->parent) {
      no_update |= BKE_object_modifier_update_subframe(
          depsgraph, scene, ob->parent, false, recursion, frame, type);
    }
    if (ob->track) {
      no_update |= BKE_object_modifier_update_subframe(
          depsgraph, scene, ob->track, false, recursion, frame, type);
    }

    /* Skip sub-frame if object is parented to vertex of a dynamic paint canvas. */
    if (no_update && ELEM(ob->partype, PARVERT1, PARVERT3)) {
      return false;
    }

    /* also update constraint targets */
    LISTBASE_FOREACH (bConstraint *, con, &ob->constraints) {
      ListBase targets = {nullptr, nullptr};

      if (BKE_constraint_targets_get(con, &targets)) {
        LISTBASE_FOREACH (bConstraintTarget *, ct, &targets) {
          if (ct->tar) {
            BKE_object_modifier_update_subframe(
                depsgraph, scene, ct->tar, false, recursion, frame, type);
          }
        }
        /* free temp targets */
        BKE_constraint_targets_flush(con, &targets, false);
      }
    }
  }

  /* was originally ID_RECALC_ALL - TODO: which flags are really needed??? */
  /* TODO(sergey): What about animation? */
  const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(depsgraph,
                                                                                    frame);

  ob->id.recalc |= ID_RECALC_ALL;
  if (update_mesh) {
    BKE_animsys_evaluate_animdata(
        &ob->id, ob->adt, &anim_eval_context, ADT_RECALC_ANIM, flush_to_original);
    /* Ignore cache clear during sub-frame updates to not mess up cache validity. */
    object_cacheIgnoreClear(ob, 1);
    BKE_object_handle_update(depsgraph, scene, ob);
    object_cacheIgnoreClear(ob, 0);
  }
  else {
    BKE_object_where_is_calc_time(depsgraph, scene, ob, frame);
  }

  /* for curve following objects, parented curve has to be updated too */
  if (ob->type == OB_CURVES_LEGACY) {
    Curve *cu = (Curve *)ob->data;
    BKE_animsys_evaluate_animdata(
        &cu->id, cu->adt, &anim_eval_context, ADT_RECALC_ANIM, flush_to_original);
  }
  /* and armatures... */
  if (ob->type == OB_ARMATURE) {
    bArmature *arm = (bArmature *)ob->data;
    BKE_animsys_evaluate_animdata(
        &arm->id, arm->adt, &anim_eval_context, ADT_RECALC_ANIM, flush_to_original);
    BKE_pose_where_is(depsgraph, scene, ob);
  }

  return false;
}

void BKE_object_update_select_id(Main *bmain)
{
  Object *ob = (Object *)bmain->objects.first;
  int select_id = 1;
  while (ob) {
    ob->runtime.select_id = select_id++;
    ob = (Object *)ob->id.next;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Conversion
 * \{ */

Mesh *BKE_object_to_mesh(Depsgraph *depsgraph, Object *object, bool preserve_all_data_layers)
{
  BKE_object_to_mesh_clear(object);

  Mesh *mesh = BKE_mesh_new_from_object(depsgraph, object, preserve_all_data_layers, false);
  object->runtime.object_as_temp_mesh = mesh;
  return mesh;
}

void BKE_object_to_mesh_clear(Object *object)
{
  if (object->runtime.object_as_temp_mesh == nullptr) {
    return;
  }
  BKE_id_free(nullptr, object->runtime.object_as_temp_mesh);
  object->runtime.object_as_temp_mesh = nullptr;
}

Curve *BKE_object_to_curve(Object *object, Depsgraph *depsgraph, bool apply_modifiers)
{
  BKE_object_to_curve_clear(object);

  Curve *curve = BKE_curve_new_from_object(object, depsgraph, apply_modifiers);
  object->runtime.object_as_temp_curve = curve;
  return curve;
}

void BKE_object_to_curve_clear(Object *object)
{
  if (object->runtime.object_as_temp_curve == nullptr) {
    return;
  }
  BKE_id_free(nullptr, object->runtime.object_as_temp_curve);
  object->runtime.object_as_temp_curve = nullptr;
}

void BKE_object_check_uuids_unique_and_report(const Object *object)
{
  BKE_pose_check_uuids_unique_and_report(object->pose);
  BKE_modifier_check_uuids_unique_and_report(object);
}

void BKE_object_modifiers_lib_link_common(void *userData, Object *ob, ID **idpoin, int cb_flag)
{
  BlendLibReader *reader = (BlendLibReader *)userData;

  BLO_read_id_address(reader, &ob->id, idpoin);
  if (*idpoin != nullptr && (cb_flag & IDWALK_CB_USER) != 0) {
    id_us_plus_no_lib(*idpoin);
  }
}

SubsurfModifierData *BKE_object_get_last_subsurf_modifier(const Object *ob)
{
  ModifierData *md = (ModifierData *)(ob->modifiers.last);

  while (md) {
    if (md->type == eModifierType_Subsurf) {
      break;
    }

    md = md->prev;
  }

  return (SubsurfModifierData *)(md);
}

void BKE_object_replace_data_on_shallow_copy(Object *ob, ID *new_data)
{
  ob->type = BKE_object_obdata_to_type(new_data);
  ob->data = (void *)new_data;
  ob->runtime.geometry_set_eval = nullptr;
  ob->runtime.data_eval = new_data;
  if (ob->runtime.bb != nullptr) {
    ob->runtime.bb->flag |= BOUNDBOX_DIRTY;
  }
  ob->id.py_instance = nullptr;
}

/** \} */
