/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "ANIM_action.hh"
#include "ANIM_fcurve.hh"

#include "BKE_action.hh"
#include "BKE_anim_data.hh"
#include "BKE_fcurve.hh"
#include "BKE_global.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"

#include "ED_anim_api.hh"

#include "BLI_listbase.h"

#include "CLG_log.h"
#include "testing/testing.h"

namespace blender::animrig::tests {
class ActionFilterTest : public testing::Test {
 public:
  Main *bmain;
  Action *action;
  Object *cube;
  Object *suzanne;

  static void SetUpTestSuite()
  {
    /* BKE_id_free() hits a code path that uses CLOG, which crashes if not initialised properly. */
    CLG_init();

    /* To make id_can_have_animdata() and friends work, the `id_types` array needs to be set up. */
    BKE_idtype_init();
  }

  static void TearDownTestSuite()
  {
    CLG_exit();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain; /* For BKE_animdata_free(). */

    action = &static_cast<bAction *>(BKE_id_new(bmain, ID_AC, "ACÄnimåtië"))->wrap();
    cube = BKE_object_add_only_object(bmain, OB_EMPTY, "Küüübus");
    suzanne = BKE_object_add_only_object(bmain, OB_EMPTY, "OBSuzanne");
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }
};

TEST_F(ActionFilterTest, bindings_expanded_or_not)
{
  Binding &bind_cube = action->binding_add();
  Binding &bind_suzanne = action->binding_add();
  ASSERT_TRUE(action->assign_id(&bind_cube, cube->id));
  ASSERT_TRUE(action->assign_id(&bind_suzanne, suzanne->id));

  Layer &layer = action->layer_add("Kübus layer");
  KeyframeStrip &key_strip = layer.strip_add(Strip::Type::Keyframe).as<KeyframeStrip>();

  /* Create multiple FCurves for multiple Bindings. */
  const KeyframeSettings settings = get_keyframe_settings(false);
  ASSERT_EQ(SingleKeyingResult::SUCCESS,
            key_strip.keyframe_insert(bind_cube, {"location", 0}, {1.0f, 0.25f}, settings));
  ASSERT_EQ(SingleKeyingResult::SUCCESS,
            key_strip.keyframe_insert(bind_cube, {"location", 1}, {1.0f, 0.25f}, settings));
  ASSERT_EQ(SingleKeyingResult::SUCCESS,
            key_strip.keyframe_insert(bind_suzanne, {"location", 0}, {1.0f, 0.25f}, settings));
  ASSERT_EQ(SingleKeyingResult::SUCCESS,
            key_strip.keyframe_insert(bind_suzanne, {"location", 1}, {1.0f, 0.25f}, settings));

  FCurve *fcu_cube_loc_x = key_strip.fcurve_find(bind_cube, {"location", 0});
  FCurve *fcu_cube_loc_y = key_strip.fcurve_find(bind_cube, {"location", 1});
  ASSERT_NE(nullptr, fcu_cube_loc_x);
  ASSERT_NE(nullptr, fcu_cube_loc_y);

  /* Mock an bAnimContext for the Animation editor, with the above Animation showing. */
  SpaceAction saction = {0};
  saction.action = action;
  saction.action_binding_handle = bind_cube.handle;
  saction.ads.filterflag = ADS_FILTER_ALL_BINDINGS;

  bAnimContext ac = {0};
  ac.datatype = ANIMCONT_ACTION;
  ac.data = action;
  ac.spacetype = SPACE_ACTION;
  ac.sl = reinterpret_cast<SpaceLink *>(&saction);
  ac.obact = cube;
  ac.ads = &saction.ads;

  { /* Test with collapsed bindings. */
    bind_cube.set_expanded(false);
    bind_suzanne.set_expanded(false);

    /* This should produce 2 bindings and no FCurves. */
    ListBase anim_data = {nullptr, nullptr};
    eAnimFilter_Flags filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE |
                                ANIMFILTER_FOREDIT | ANIMFILTER_NODUPLIS |
                                ANIMFILTER_LIST_CHANNELS);
    const int num_entries = ANIM_animdata_filter(
        &ac, &anim_data, filter, ac.data, eAnimCont_Types(ac.datatype));
    EXPECT_EQ(2, num_entries);
    EXPECT_EQ(2, BLI_listbase_count(&anim_data));

    ASSERT_GE(num_entries, 1)
        << "Missing 1st ANIMTYPE_ACTION_BINDING entry, stopping to prevent crash";
    const bAnimListElem *first_ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 0));
    EXPECT_EQ(ANIMTYPE_ACTION_BINDING, first_ale->type);
    EXPECT_EQ(ALE_ACTION_BINDING, first_ale->datatype);
    EXPECT_EQ(&cube->id, first_ale->id) << "id should be the animated ID (" << cube->id.name
                                        << ") but is (" << first_ale->id->name << ")";
    EXPECT_EQ(cube->adt, first_ale->adt) << "adt should be the animated ID's animation data";
    EXPECT_EQ(&action->id, first_ale->fcurve_owner_id) << "fcurve_owner_id should be the Action";
    EXPECT_EQ(&action->id, first_ale->key_data) << "key_data should be the Action";
    EXPECT_EQ(&bind_cube, first_ale->data);
    EXPECT_EQ(bind_cube.binding_flags, first_ale->flag);

    ASSERT_GE(num_entries, 2)
        << "Missing 2nd ANIMTYPE_ACTION_BINDING entry, stopping to prevent crash";
    const bAnimListElem *second_ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 1));
    EXPECT_EQ(ANIMTYPE_ACTION_BINDING, second_ale->type);
    EXPECT_EQ(&bind_suzanne, second_ale->data);
    /* Assume the rest is set correctly, as it's the same code as tested above. */

    ANIM_animdata_freelist(&anim_data);
  }

  { /* Test with one expanded and one collapsed binding. */
    bind_cube.set_expanded(true);
    bind_suzanne.set_expanded(false);

    /* This should produce 2 bindings and 2 FCurves. */
    ListBase anim_data = {nullptr, nullptr};
    eAnimFilter_Flags filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE |
                                ANIMFILTER_FOREDIT | ANIMFILTER_NODUPLIS |
                                ANIMFILTER_LIST_CHANNELS);
    const int num_entries = ANIM_animdata_filter(
        &ac, &anim_data, filter, ac.data, eAnimCont_Types(ac.datatype));
    EXPECT_EQ(4, num_entries);
    EXPECT_EQ(4, BLI_listbase_count(&anim_data));

    /* First should be Cube binding. */
    ASSERT_GE(num_entries, 1) << "Missing 1st ale, stopping to prevent crash";
    const bAnimListElem *ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 0));
    EXPECT_EQ(ANIMTYPE_ACTION_BINDING, ale->type);
    EXPECT_EQ(&bind_cube, ale->data);

    /* After that the Cube's FCurves. */
    ASSERT_GE(num_entries, 2) << "Missing 2nd ale, stopping to prevent crash";
    ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 1));
    EXPECT_EQ(ANIMTYPE_FCURVE, ale->type);
    EXPECT_EQ(fcu_cube_loc_x, ale->data);
    EXPECT_EQ(bind_cube.handle, ale->binding_handle);

    ASSERT_GE(num_entries, 3) << "Missing 3rd ale, stopping to prevent crash";
    ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 2));
    EXPECT_EQ(ANIMTYPE_FCURVE, ale->type);
    EXPECT_EQ(fcu_cube_loc_y, ale->data);
    EXPECT_EQ(bind_cube.handle, ale->binding_handle);

    /* And finally the Suzanne binding. */
    ASSERT_GE(num_entries, 4) << "Missing 4th ale, stopping to prevent crash";
    ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 3));
    EXPECT_EQ(ANIMTYPE_ACTION_BINDING, ale->type);
    EXPECT_EQ(&bind_suzanne, ale->data);

    ANIM_animdata_freelist(&anim_data);
  }

  { /* Test one expanded and one collapsed binding, and one Binding and one FCurve selected. */
    bind_cube.set_expanded(true);
    bind_cube.set_selected(false);
    bind_suzanne.set_expanded(false);
    bind_suzanne.set_selected(true);

    fcu_cube_loc_x->flag &= ~FCURVE_SELECTED;
    fcu_cube_loc_y->flag |= FCURVE_SELECTED;

    /* This should produce 1 binding and 1 FCurve. */
    ListBase anim_data = {nullptr, nullptr};
    eAnimFilter_Flags filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE |
                                ANIMFILTER_SEL | ANIMFILTER_FOREDIT | ANIMFILTER_NODUPLIS |
                                ANIMFILTER_LIST_CHANNELS);
    const int num_entries = ANIM_animdata_filter(
        &ac, &anim_data, filter, ac.data, eAnimCont_Types(ac.datatype));
    EXPECT_EQ(2, num_entries);
    EXPECT_EQ(2, BLI_listbase_count(&anim_data));

    /* First should be Cube's selected FCurve. */
    const bAnimListElem *ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 0));
    EXPECT_EQ(ANIMTYPE_FCURVE, ale->type);
    EXPECT_EQ(fcu_cube_loc_y, ale->data);

    /* Second the Suzanne binding, as that's the only selected binding. */
    ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 1));
    EXPECT_EQ(ANIMTYPE_ACTION_BINDING, ale->type);
    EXPECT_EQ(&bind_suzanne, ale->data);

    ANIM_animdata_freelist(&anim_data);
  }
}

TEST_F(ActionFilterTest, layered_action_active_fcurves)
{
  Binding &bind_cube = action->binding_add();
  /* The Action+Binding has to be assigned to what the bAnimContext thinks is the active Object.
   * See the BLI_assert_msg() call in the ANIMCONT_ACTION case of ANIM_animdata_filter(). */
  ASSERT_TRUE(action->assign_id(&bind_cube, cube->id));

  Layer &layer = action->layer_add("Kübus layer");
  KeyframeStrip &key_strip = layer.strip_add(Strip::Type::Keyframe).as<KeyframeStrip>();

  /* Create multiple FCurves. */
  const KeyframeSettings settings = get_keyframe_settings(false);
  ASSERT_EQ(SingleKeyingResult::SUCCESS,
            key_strip.keyframe_insert(bind_cube, {"location", 0}, {1.0f, 0.25f}, settings));
  ASSERT_EQ(SingleKeyingResult::SUCCESS,
            key_strip.keyframe_insert(bind_cube, {"location", 1}, {1.0f, 0.25f}, settings));

  /* Set one F-Curve as the active one, and the other as inactive. The latter is necessary because
   * by default the first curve is automatically marked active, but that's too trivial a test case
   * (it's too easy to mistakenly just return the first-seen F-Curve). */
  FCurve *fcurve_active = key_strip.fcurve_find(bind_cube, {"location", 1});
  fcurve_active->flag |= FCURVE_ACTIVE;
  FCurve *fcurve_other = key_strip.fcurve_find(bind_cube, {"location", 0});
  fcurve_other->flag &= ~FCURVE_ACTIVE;

  /* Mock an bAnimContext for the Action editor. */
  SpaceAction saction = {0};
  saction.action = action;
  saction.action_binding_handle = bind_cube.handle;
  saction.ads.filterflag = ADS_FILTER_ALL_BINDINGS;

  bAnimContext ac = {0};
  ac.datatype = ANIMCONT_ACTION;
  ac.data = action;
  ac.spacetype = SPACE_ACTION;
  ac.sl = reinterpret_cast<SpaceLink *>(&saction);
  ac.obact = cube;
  ac.ads = &saction.ads;

  {
    /* This should produce just the active F-Curve. */
    ListBase anim_data = {nullptr, nullptr};
    eAnimFilter_Flags filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE |
                                ANIMFILTER_FCURVESONLY | ANIMFILTER_ACTIVE);
    const int num_entries = ANIM_animdata_filter(
        &ac, &anim_data, filter, ac.data, eAnimCont_Types(ac.datatype));
    EXPECT_EQ(1, num_entries);
    EXPECT_EQ(1, BLI_listbase_count(&anim_data));

    const bAnimListElem *first_ale = static_cast<bAnimListElem *>(BLI_findlink(&anim_data, 0));
    EXPECT_EQ(ANIMTYPE_FCURVE, first_ale->type);
    EXPECT_EQ(ALE_FCURVE, first_ale->datatype);
    EXPECT_EQ(fcurve_active, first_ale->data);

    ANIM_animdata_freelist(&anim_data);
  }
}

}  // namespace blender::animrig::tests
