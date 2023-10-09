/* SPDX-FileCopyrightText: 2023 Blender Developers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_anim_defaults.h"
#include "DNA_anim_types.h"
#include "DNA_defaults.h"

#include "BLI_listbase.h"
#include "BLI_listbase_wrapper.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BKE_fcurve.h"

#include "ED_keyframing.hh"

#include "MEM_guardedalloc.h"

#include "atomic_ops.h"

#include "ANIM_animation.hh"

#include <cstdio>
#include <cstring>

namespace blender::animrig {

static animrig::Layer *animationlayer_alloc()
{
  AnimationLayer *layer = DNA_struct_default_alloc(AnimationLayer);
  return &layer->wrap();
}
static animrig::Strip *animationstrip_alloc_infinite(const eAnimationStrip_type type)
{
  AnimationStrip *strip;
  switch (type) {
    case ANIM_STRIP_TYPE_KEYFRAME: {
      KeyframeAnimationStrip *key_strip = MEM_new<KeyframeAnimationStrip>(__func__);
      strip = &key_strip->strip;
      break;
    }
  }

  BLI_assert_msg(strip, "unsupported strip type");

  /* Copy the default AnimationStrip fields into the allocated data-block. */
  memcpy(strip, DNA_struct_default_get(AnimationStrip), sizeof(*strip));
  return &strip->wrap();
}

/* Copied from source/blender/blenkernel/intern/grease_pencil.cc. It also has a shrink_array()
 * function, if we ever need one (we will). */
template<typename T> static void grow_array(T **array, int *num, const int add_num)
{
  BLI_assert(add_num > 0);
  const int new_array_num = *num + add_num;
  T *new_array = reinterpret_cast<T *>(MEM_cnew_array<T *>(new_array_num, __func__));

  blender::uninitialized_relocate_n(*array, *num, new_array);
  if (*array != nullptr) {
    MEM_freeN(*array);
  }

  *array = new_array;
  *num = new_array_num;
}

/* ----- Animation C++ implementation ----------- */

blender::Span<const Layer *> Animation::layers() const
{
  return blender::Span<Layer *>{reinterpret_cast<Layer **>(this->layer_array),
                                this->layer_array_num};
}
blender::MutableSpan<Layer *> Animation::layers()
{
  return blender::MutableSpan<Layer *>{reinterpret_cast<Layer **>(this->layer_array),
                                       this->layer_array_num};
}
const Layer *Animation::layer(const int64_t index) const
{
  return &this->layer_array[index]->wrap();
}
Layer *Animation::layer(const int64_t index)
{
  return &this->layer_array[index]->wrap();
}

Layer *Animation::layer_add(const char *name)
{
  using namespace blender::animrig;

  Layer *new_layer = animationlayer_alloc();
  STRNCPY_UTF8(new_layer->name, name);

  /* FIXME: For now, just add a keyframe strip. This may not be the right choice
   * going forward, and maybe it's better to allocate the strip at the first
   * use. */
  ::AnimationStrip *strip = animationstrip_alloc_infinite(ANIM_STRIP_TYPE_KEYFRAME);
  BLI_addtail(&new_layer->strips, strip);

  /* Add the new layer to the layer array. */
  grow_array<::AnimationLayer *>(&this->layer_array, &this->layer_array_num, 1);
  this->layer_active_index = this->layer_array_num - 1;
  this->layer_array[this->layer_active_index] = new_layer;

  return new_layer;
}

blender::Span<const Output *> Animation::outputs() const
{
  return blender::Span<Output *>{reinterpret_cast<Output **>(this->output_array),
                                 this->output_array_num};
}
blender::MutableSpan<Output *> Animation::outputs()
{
  return blender::MutableSpan<Output *>{reinterpret_cast<Output **>(this->output_array),
                                        this->output_array_num};
}
const Output *Animation::output(const int64_t index) const
{
  return &this->output_array[index]->wrap();
}
Output *Animation::output(const int64_t index)
{
  return &this->output_array[index]->wrap();
}

Output *Animation::output_add(ID *animated_id)
{
  Output &output = MEM_new<AnimationOutput>(__func__)->wrap();

  output.idtype = GS(animated_id->name);
  output.stable_index = atomic_add_and_fetch_int32(&this->last_output_stable_index, 1);

  /* The ID type bytes can be stripped from the name, as that information is
   * already stored in output.idtype. This also makes it easier to combine
   * names when multiple IDs share the same output. */
  STRNCPY_UTF8(output.fallback, animated_id->name + 2);

  // TODO: turn this into an actually nice function.
  output.runtime.id = MEM_new<ID *>(__func__);
  output.runtime.num_ids = 1;
  *(output.runtime.id) = animated_id;

  /* Add the new output to the output array. */
  grow_array<::AnimationOutput *>(&this->output_array, &this->output_array_num, 1);
  this->output_array[this->output_array_num - 1] = &output;

  return &output;
}

template<> KeyframeStrip &Strip::as<KeyframeStrip>()
{
  BLI_assert_msg(type == ANIM_STRIP_TYPE_KEYFRAME,
                 "Strip is not of type ANIM_STRIP_TYPE_KEYFRAME");
  return *reinterpret_cast<KeyframeStrip *>(this);
}

AnimationChannelsForOutput *KeyframeStrip::chans_for_out(const AnimationOutput *out)
{
  /* FIXME: use a hash map lookup for this. */
  for (AnimationChannelsForOutput *channels :
       ListBaseWrapper<AnimationChannelsForOutput>(&this->channels_for_output))
  {
    if (channels->output_stable_index == out->stable_index) {
      return channels;
    }
  }

  AnimationChannelsForOutput *channels = MEM_new<AnimationChannelsForOutput>(__func__);
  channels->output_stable_index = out->stable_index;
  BLI_addtail(&this->channels_for_output, channels);

  return channels;
}

FCurve *KeyframeStrip::fcurve_find_or_create(const AnimationOutput *out,
                                             const char *rna_path,
                                             const int array_index)
{
  AnimationChannelsForOutput *channels = this->chans_for_out(out);

  FCurve *fcurve = BKE_fcurve_find(&channels->fcurves, rna_path, array_index);
  if (fcurve) {
    return fcurve;
  }

  /* Copied from ED_action_fcurve_ensure(). */
  /* TODO: move to separate function, call that from both places. */
  fcurve = BKE_fcurve_create();
  fcurve->rna_path = BLI_strdup(rna_path);
  fcurve->array_index = array_index;

  fcurve->flag = (FCURVE_VISIBLE | FCURVE_SELECTED);
  fcurve->auto_smoothing = U.auto_smoothing_new;

  if (BLI_listbase_is_empty(&channels->fcurves)) {
    fcurve->flag |= FCURVE_ACTIVE; /* First curve is added active. */
  }
  BLI_addhead(&channels->fcurves, fcurve);
  return fcurve;
}

FCurve *keyframe_insert(Strip *strip,
                        const AnimationOutput *out,
                        const char *rna_path,
                        const int array_index,
                        const float value,
                        const float time,
                        const eBezTriple_KeyframeType keytype)
{
  if (strip->type != ANIM_STRIP_TYPE_KEYFRAME) {
    /* TODO: handle this properly, in a way that can be communicated to the user. */
    std::fprintf(stderr,
                 "Strip is not of type ANIM_STRIP_TYPE_KEYFRAME, unable to insert keys here\n");
    return nullptr;
  }

  KeyframeStrip &key_strip = strip->wrap().as<KeyframeStrip>();
  FCurve *fcurve = key_strip.fcurve_find_or_create(out, rna_path, array_index);

  if (!BKE_fcurve_is_keyframable(fcurve)) {
    /* TODO: handle this properly, in a way that can be communicated to the user. */
    std::fprintf(stderr,
                 "FCurve %s[%d] for output %s doesn't allow inserting keys.\n",
                 rna_path,
                 array_index,
                 out->fallback);
    return nullptr;
  }

  /* TODO: Move this function from the editors module to the animrig module. */
  /* TODO: Handle the eInsertKeyFlags. */
  const int index = insert_vert_fcurve(fcurve, time, value, keytype, eInsertKeyFlags(0));
  if (index < 0) {
    std::fprintf(stderr,
                 "Could not insert key into FCurve %s[%d] for output %s.\n",
                 rna_path,
                 array_index,
                 out->fallback);
    return nullptr;
  }

  return fcurve;
}

}  // namespace blender::animrig
