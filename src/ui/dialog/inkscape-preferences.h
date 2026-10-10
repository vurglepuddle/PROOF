// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Inkscape Preferences dialog
 */
/* Authors:
 *   Carl Hetherington
 *   Marco Scholten
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Bruno Dilly <bruno.dilly@gmail.com>
 *
 * Copyright (C) 2004-2013 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_DIALOG_INKSCAPE_PREFERENCES_H
#define INKSCAPE_UI_DIALOG_INKSCAPE_PREFERENCES_H

#include <gtkmm/colorbutton.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/frame.h>
#include <gtkmm/notebook.h>
#include <gtkmm/searchentry2.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/treestore.h>

#include "preferences.h"
#include "ui/dialog/dialog-base.h"
#include "ui/widget/preferences-widget.h"
#include "ui/widget/spinbutton.h"

// UPDATE THIS IF YOU'RE ADDING PREFS PAGES.
// Otherwise the commands that open the dialog with the new page will fail.

enum
{
    PREFS_PAGE_TOOLS,
    PREFS_PAGE_TOOLS_SELECTOR,
    PREFS_PAGE_TOOLS_NODE,
    PREFS_PAGE_TOOLS_TWEAK,
    PREFS_PAGE_TOOLS_ZOOM,
    PREFS_PAGE_TOOLS_MEASURE,
    PREFS_PAGE_TOOLS_SHAPES,
    PREFS_PAGE_TOOLS_SHAPES_RECT,
    PREFS_PAGE_TOOLS_SHAPES_3DBOX,
    PREFS_PAGE_TOOLS_SHAPES_ELLIPSE,
    PREFS_PAGE_TOOLS_SHAPES_STAR,
    PREFS_PAGE_TOOLS_SHAPES_SPIRAL,
    PREFS_PAGE_TOOLS_PENCIL,
    PREFS_PAGE_TOOLS_PEN,
    PREFS_PAGE_TOOLS_CALLIGRAPHY,
    PREFS_PAGE_TOOLS_TEXT,
    PREFS_PAGE_TOOLS_SPRAY,
    PREFS_PAGE_TOOLS_ERASER,
    PREFS_PAGE_TOOLS_PAINTBUCKET,
    PREFS_PAGE_TOOLS_GRADIENT,
    PREFS_PAGE_TOOLS_DROPPER,
    PREFS_PAGE_TOOLS_CONNECTOR,
    PREFS_PAGE_TOOLS_LPETOOL,
    PREFS_PAGE_UI,
    PREFS_PAGE_UI_THEME,
    PREFS_PAGE_UI_TOOLBARS,
    PREFS_PAGE_UI_WINDOWS,
    PREFS_PAGE_UI_COLOR_PICKERS,
    PREFS_PAGE_UI_GRIDS,
    PREFS_PAGE_COMMAND_PALETTE,
    PREFS_PAGE_UI_KEYBOARD_SHORTCUTS,
    PREFS_PAGE_BEHAVIOR,
    PREFS_PAGE_BEHAVIOR_SELECTING,
    PREFS_PAGE_BEHAVIOR_TRANSFORMS,
    PREFS_PAGE_BEHAVIOR_SCROLLING,
    PREFS_PAGE_BEHAVIOR_SNAPPING,
    PREFS_PAGE_BEHAVIOR_STEPS,
    PREFS_PAGE_BEHAVIOR_CLONES,
    PREFS_PAGE_BEHAVIOR_MASKS,
    PREFS_PAGE_BEHAVIOR_MARKERS,
    PREFS_PAGE_BEHAVIOR_CLIPBOARD,
    PREFS_PAGE_BEHAVIOR_CLEANUP,
    PREFS_PAGE_BEHAVIOR_LPE,
    PREFS_PAGE_IO,
    PREFS_PAGE_IO_MOUSE,
    PREFS_PAGE_IO_SVGOUTPUT,
    PREFS_PAGE_IO_SVGEXPORT,
    PREFS_PAGE_IO_CMS,
    PREFS_PAGE_IO_AUTOSAVE,
    PREFS_PAGE_IO_OPENCLIPART,
    PREFS_PAGE_SYSTEM,
    PREFS_PAGE_BITMAPS,
    PREFS_PAGE_RENDERING,
    PREFS_PAGE_SPELLCHECK,
    PREFS_PAGE_TEMPLATES,
    PREFS_PAGE_NOTFOUND
};

namespace Glib {
class Regex;
} // namespace Glib

namespace Gtk {
class Scale;
} // namespace Gtk

namespace Inkscape::UI::Dialog {

class InkscapePreferences final : public DialogBase
{
public:
    InkscapePreferences();
    ~InkscapePreferences() final;

    void showPage(); // Show page indicated by "/dialogs/preferences/page".

protected:
    Gtk::Frame _page_frame;
    Gtk::Label _page_title;
    Gtk::TreeView _page_list;
    Gtk::SearchEntry2 _search;
    Glib::RefPtr<Gtk::TreeStore> _page_list_model;
    Gtk::Widget *_highlighted_widget = nullptr;
    Glib::RefPtr<Gtk::TreeModelFilter> _page_list_model_filter;
    Glib::RefPtr<Gtk::TreeModelSort> _page_list_model_sort;
    std::vector<Gtk::Label *> _search_results;
    Glib::RefPtr<Glib::Regex> _rx;
    int _num_results = 0;
    bool _show_all = false;

    //Pagelist model columns:
    class PageListModelColumns : public Gtk::TreeModel::ColumnRecord
    {
    public:
        PageListModelColumns()
        { Gtk::TreeModelColumnRecord::add(_col_name); Gtk::TreeModelColumnRecord::add(_col_page); Gtk::TreeModelColumnRecord::add(_col_id); }
        Gtk::TreeModelColumn<Glib::ustring> _col_name;
        Gtk::TreeModelColumn<int> _col_id;
        Gtk::TreeModelColumn<UI::Widget::DialogPage*> _col_page;
    };
    PageListModelColumns _page_list_columns;

    // clang-format off
    UI::Widget::DialogPage _page_tools;
    UI::Widget::DialogPage _page_selector;
    UI::Widget::DialogPage _page_node;
    UI::Widget::DialogPage _page_tweak;
    UI::Widget::DialogPage _page_spray;
    UI::Widget::DialogPage _page_zoom;
    UI::Widget::DialogPage _page_measure;
    UI::Widget::DialogPage _page_shapes;
    UI::Widget::DialogPage _page_pencil;
    UI::Widget::DialogPage _page_pen;
    UI::Widget::DialogPage _page_calligraphy;
    UI::Widget::DialogPage _page_text;
    UI::Widget::DialogPage _page_gradient;
    UI::Widget::DialogPage _page_connector;
    UI::Widget::DialogPage _page_dropper;
    UI::Widget::DialogPage _page_lpetool;

    UI::Widget::DialogPage _page_rectangle;
    UI::Widget::DialogPage _page_3dbox;
    UI::Widget::DialogPage _page_ellipse;
    UI::Widget::DialogPage _page_star;
    UI::Widget::DialogPage _page_spiral;
    UI::Widget::DialogPage _page_paintbucket;
    UI::Widget::DialogPage _page_eraser;

    UI::Widget::DialogPage _page_ui;
    UI::Widget::DialogPage _page_notfound;
    UI::Widget::DialogPage _page_theme;
    UI::Widget::DialogPage _page_toolbars;
    UI::Widget::DialogPage _page_windows;
    UI::Widget::DialogPage _page_grids;
    UI::Widget::DialogPage _page_command_palette;
    UI::Widget::DialogPage _page_color_pickers;

    UI::Widget::DialogPage _page_behavior;
    UI::Widget::DialogPage _page_select;
    UI::Widget::DialogPage _page_transforms;
    UI::Widget::DialogPage _page_scrolling;
    UI::Widget::DialogPage _page_snapping;
    UI::Widget::DialogPage _page_steps;
    UI::Widget::DialogPage _page_clones;
    UI::Widget::DialogPage _page_mask;
    UI::Widget::DialogPage _page_clipboard;
    UI::Widget::DialogPage _page_cleanup;
    UI::Widget::DialogPage _page_lpe;

    UI::Widget::DialogPage _page_io;
    UI::Widget::DialogPage _page_mouse;
    UI::Widget::DialogPage _page_svgoutput;
    UI::Widget::DialogPage _page_svgexport;
    UI::Widget::DialogPage _page_cms;
    UI::Widget::DialogPage _page_autosave;

    UI::Widget::DialogPage _page_rendering;
    UI::Widget::DialogPage _page_system;
    UI::Widget::DialogPage _page_bitmaps;
    UI::Widget::DialogPage _page_spellcheck;
    UI::Widget::DialogPage _page_templates;

    UI::Widget::DialogPage _page_keyshortcuts;
    Glib::RefPtr<Gtk::SizeGroup> _keyboard_sizegroup;
    UI::Widget::PrefSpinButton _mouse_sens;
    UI::Widget::PrefSpinButton _mouse_thres;
    UI::Widget::PrefSlider      _mouse_grabsize;
    UI::Widget::PrefCheckButton _mouse_use_ext_input;
    UI::Widget::PrefCheckButton _mouse_switch_on_ext_input;
    std::unique_ptr<Preferences::PreferencesObserver> _handle_size;

    UI::Widget::PrefSpinButton _scroll_wheel;
    UI::Widget::PrefSpinButton _scroll_arrow_px;
    UI::Widget::PrefSpinButton _scroll_arrow_acc;
    UI::Widget::PrefSpinButton _scroll_auto_speed;
    UI::Widget::PrefSpinButton _scroll_auto_thres;
    UI::Widget::PrefCheckButton _scroll_space;

    Gtk::Scale      *_slider_snapping_delay;

    UI::Widget::PrefCheckButton _snap_default;
    UI::Widget::PrefCheckButton _snap_indicator;
    UI::Widget::PrefCheckButton _snap_closest_only;
    UI::Widget::PrefCheckButton _snap_mouse_pointer;
    UI::Widget::PrefCheckButton _snap_indicator_distance;
    UI::Widget::PrefCheckButton _snap_always_grid;
    UI::Widget::PrefCheckButton _snap_always_guide;
    UI::Widget::PrefCheckButton _snap_always_object;
    UI::Widget::PrefCheckButton _snap_always_align;
    UI::Widget::PrefCheckButton _snap_always_dist;

    UI::Widget::SpinButton   _steps_rot_snap;
    UI::Widget::PrefCheckButton _steps_rot_relative;
    UI::Widget::PrefCheckButton _steps_compass;
    UI::Widget::PrefSpinUnit    _steps_arrow;
    UI::Widget::PrefCheckButton _move_rotated;
    UI::Widget::PrefSpinUnit    _steps_scale;
    UI::Widget::PrefSpinUnit    _steps_inset;
    UI::Widget::PrefSpinButton  _steps_zoom;
    UI::Widget::PrefCheckButton _middle_mouse_zoom;
    UI::Widget::PrefSpinButton  _steps_rotate;

    UI::Widget::PrefRadioButton _t_sel_trans_obj;
    UI::Widget::PrefRadioButton _t_sel_trans_outl;
    UI::Widget::PrefRadioButton _t_sel_cue_none;
    UI::Widget::PrefRadioButton _t_sel_cue_mark;
    UI::Widget::PrefRadioButton _t_sel_cue_box;
    UI::Widget::PrefRadioButton _t_too_off;
    UI::Widget::PrefRadioButton _t_too_group;
    UI::Widget::PrefRadioButton _t_too_layer;
    UI::Widget::PrefRadioButton _t_bbox_visual;
    UI::Widget::PrefRadioButton _t_bbox_geometric;

    UI::Widget::PrefCheckButton _t_cvg_keep_objects;
    UI::Widget::PrefCheckButton _t_cvg_convert_whole_groups;
    UI::Widget::PrefCheckButton _t_node_show_outline;
    UI::Widget::PrefCheckButton _t_node_live_outline;
    UI::Widget::PrefCheckButton _t_node_live_objects;
    UI::Widget::PrefCheckButton _t_node_pathflash_enabled;
    UI::Widget::PrefCheckButton _t_node_pathflash_selected;
    UI::Widget::PrefSpinButton  _t_node_pathflash_timeout;
    UI::Widget::PrefCheckButton _t_node_show_path_direction;
    UI::Widget::PrefCheckButton _t_node_single_node_transform_handles;
    UI::Widget::PrefColorPicker _t_node_pathoutline_color;
    UI::Widget::PrefSpinButton  _t_node_delete_flat_corner;

    UI::Widget::PrefCombo       _t_node_delete_mode;
    UI::Widget::PrefCombo       _t_node_delete_mode1;
    UI::Widget::PrefCombo       _t_node_delete_mode2;
    UI::Widget::PrefCombo       _t_node_delete_mode3;
    UI::Widget::PrefCombo       _t_node_cut_mode;

    // Command Palette
    UI::Widget::PrefCheckButton _cp_show_full_action_name;
    UI::Widget::PrefCheckButton _cp_show_untranslated_name;

    UI::Widget::PrefCombo _gtk_theme;
    UI::Widget::PrefOpenFolder _sys_user_themes_dir_copy;
    UI::Widget::PrefOpenFolder _sys_user_icons_dir_copy;
    UI::Widget::PrefCombo _icon_theme;
    UI::Widget::PrefCheckButton _dark_theme;
    UI::Widget::PrefSlider _contrast_theme;
    UI::Widget::PrefCheckButton _enable_animations;
    UI::Widget::PrefCheckButton _narrow_spinbutton;
    UI::Widget::PrefCheckButton _compact_colorselector;
    UI::Widget::PrefCheckButton _symbolic_icons;
    UI::Widget::PrefCheckButton _symbolic_base_colors;
    int                         _symbolic_base_row;
    UI::Widget::PrefColorPicker _symbolic_base_color;
    int                         _symbolic_custom_row;
    UI::Widget::PrefColorPicker _symbolic_warning_color;
    UI::Widget::PrefColorPicker _symbolic_error_color;
    UI::Widget::PrefColorPicker _symbolic_success_color;
    /*     Gtk::Image *_complementary_colors; */
    UI::Widget::PrefCombo _misc_small_toolbar;
    UI::Widget::PrefCombo _misc_small_secondary;
    UI::Widget::PrefCombo _misc_small_tools;
    UI::Widget::PrefCombo _menu_icons;

    UI::Widget::PrefRadioButton _win_dockable;
    UI::Widget::PrefRadioButton _win_floating;
    UI::Widget::PrefRadioButton _win_native;
    UI::Widget::PrefRadioButton _win_gtk;
    UI::Widget::PrefRadioButton _win_save_dialog_pos_on;
    UI::Widget::PrefRadioButton _win_save_dialog_pos_off;
    UI::Widget::PrefCombo       _win_default_size;
    UI::Widget::PrefRadioButton _win_ontop_none;
    UI::Widget::PrefRadioButton _win_ontop_normal;
    UI::Widget::PrefRadioButton _win_ontop_agressive;
    UI::Widget::PrefRadioButton _win_dialogs_labels_auto;
    UI::Widget::PrefRadioButton _win_dialogs_labels_active;
    UI::Widget::PrefRadioButton _win_dialogs_labels_off;
    UI::Widget::PrefCheckButton _win_dialogs_tab_close_btn;
    UI::Widget::PrefRadioButton _win_save_geom_off;
    UI::Widget::PrefRadioButton _win_save_geom;
    UI::Widget::PrefRadioButton _win_save_geom_prefs;
    UI::Widget::PrefCombo       _win_start_mode;
    UI::Widget::PrefCheckButton _win_hide_task;
    UI::Widget::PrefCheckButton _win_save_viewport;
    UI::Widget::PrefCheckButton _win_zoom_resize;
    UI::Widget::PrefCheckButton _win_version_in_title;

    UI::Widget::PrefCheckButton _pencil_average_all_sketches;

    UI::Widget::PrefCheckButton _calligrapy_keep_selected;

    UI::Widget::PrefCheckButton _connector_ignore_text;

    UI::Widget::PrefRadioButton _clone_option_parallel;
    UI::Widget::PrefRadioButton _clone_option_stay;
    UI::Widget::PrefRadioButton _clone_option_transform;
    UI::Widget::PrefRadioButton _clone_option_unlink;
    UI::Widget::PrefRadioButton _clone_option_delete;
    UI::Widget::PrefRadioButton _clone_option_keep;
    UI::Widget::PrefCheckButton _clone_relink_on_duplicate;
    UI::Widget::PrefCheckButton _clone_to_curves;
    UI::Widget::PrefCheckButton _clone_ignore_to_curves;

    UI::Widget::PrefCheckButton _mask_mask_on_top;
    UI::Widget::PrefCheckButton _mask_mask_remove;
    UI::Widget::PrefCheckButton _mask_mask_on_ungroup;
    UI::Widget::PrefRadioButton _mask_grouping_none;
    UI::Widget::PrefRadioButton _mask_grouping_separate;
    UI::Widget::PrefRadioButton _mask_grouping_all;
    UI::Widget::PrefCheckButton _mask_ungrouping;

    UI::Widget::PrefSpinButton  _filter_multi_threaded;
    UI::Widget::PrefSpinButton  _rendering_cache_size;
    UI::Widget::PrefSpinButton  _rendering_xray_radius;
    UI::Widget::PrefSpinButton  _rendering_outline_overlay_opacity;
    UI::Widget::PrefColorPicker _rendering_object_outline_color;
    UI::Widget::PrefColorPicker _rendering_clip_outline_color;
    UI::Widget::PrefColorPicker _rendering_mask_outline_color;
    UI::Widget::PrefCombo       _canvas_update_strategy;
    UI::Widget::PrefCheckButton _canvas_request_opengl;
    UI::Widget::PrefRadioButton _blur_quality_best;
    UI::Widget::PrefRadioButton _blur_quality_better;
    UI::Widget::PrefRadioButton _blur_quality_normal;
    UI::Widget::PrefRadioButton _blur_quality_worse;
    UI::Widget::PrefRadioButton _blur_quality_worst;
    UI::Widget::PrefRadioButton _filter_quality_best;
    UI::Widget::PrefRadioButton _filter_quality_better;
    UI::Widget::PrefRadioButton _filter_quality_normal;
    UI::Widget::PrefRadioButton _filter_quality_worse;
    UI::Widget::PrefRadioButton _filter_quality_worst;
#if CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 18, 0)
    UI::Widget::PrefCheckButton _cairo_dithering;
#endif

    UI::Widget::PrefCheckButton _canvas_developer_mode_enabled;
    UI::Widget::PrefSpinButton  _canvas_tile_size;
    UI::Widget::PrefSpinButton  _canvas_render_time_limit;
    UI::Widget::PrefCombo       _canvas_pixelstreamer_method;
    UI::Widget::PrefSpinButton  _canvas_padding;
    UI::Widget::PrefSpinButton  _canvas_prerender;
    UI::Widget::PrefSpinButton  _canvas_preempt;
    UI::Widget::PrefSpinButton  _canvas_coarsener_min_size;
    UI::Widget::PrefSpinButton  _canvas_coarsener_glue_size;
    UI::Widget::PrefSpinButton  _canvas_coarsener_min_fullness;
    UI::Widget::PrefCheckButton _canvas_debug_framecheck;
    UI::Widget::PrefCheckButton _canvas_debug_logging;
    UI::Widget::PrefCheckButton _canvas_debug_delay_redraw;
    UI::Widget::PrefSpinButton  _canvas_debug_delay_redraw_time;
    UI::Widget::PrefCheckButton _canvas_debug_show_redraw;
    UI::Widget::PrefCheckButton _canvas_debug_show_unclean;
    UI::Widget::PrefCheckButton _canvas_debug_show_snapshot;
    UI::Widget::PrefCheckButton _canvas_debug_show_clean;
    UI::Widget::PrefCheckButton _canvas_debug_disable_redraw;
    UI::Widget::PrefCheckButton _canvas_debug_sticky_decoupled;
    UI::Widget::PrefCheckButton _canvas_debug_animate;

    UI::Widget::PrefCheckButton _trans_scale_stroke;
    UI::Widget::PrefCheckButton _trans_scale_corner;
    UI::Widget::PrefCheckButton _trans_gradient;
    UI::Widget::PrefCheckButton _trans_pattern;
    UI::Widget::PrefCheckButton _trans_dash_scale;
    UI::Widget::PrefRadioButton _trans_optimized;
    UI::Widget::PrefRadioButton _trans_preserved;

    UI::Widget::PrefRadioButton _sel_all;
    UI::Widget::PrefRadioButton _sel_current;
    UI::Widget::PrefRadioButton _sel_recursive;
    UI::Widget::PrefCheckButton _sel_hidden;
    UI::Widget::PrefCheckButton _sel_locked;
    UI::Widget::PrefCheckButton _sel_inlayer_same;
    UI::Widget::PrefCheckButton _sel_touch_topmost_only;
    UI::Widget::PrefCheckButton _sel_layer_deselects;
    UI::Widget::PrefCheckButton _sel_cycle;
    UI::Widget::PrefCheckButton _sel_zero_opacity;

    UI::Widget::PrefRadioButton _clipboard_style_computed;
    UI::Widget::PrefRadioButton _clipboard_style_verbatim;

    UI::Widget::PrefCheckButton _cleanup_swatches;

    UI::Widget::PrefCheckButton _lpe_copy_mirroricons;
    UI::Widget::PrefCheckButton _lpe_show_experimental;

    UI::Widget::PrefSpinButton  _importexport_export_res;
    UI::Widget::PrefSpinButton  _importexport_import_res;
    UI::Widget::PrefCheckButton _importexport_import_res_override;
    UI::Widget::PrefCheckButton _rendering_image_outline;
    UI::Widget::PrefColorPicker _rendering_image_outline_color;
    UI::Widget::PrefSlider      _snap_delay;
    UI::Widget::PrefSlider      _snap_weight;
    UI::Widget::PrefSlider      _snap_persistence;
    UI::Widget::PrefEntry       _font_sample;
    UI::Widget::PrefCheckButton _font_dialog;
    UI::Widget::PrefCheckButton _font_missing_dialog; ///< PROOF
    UI::Widget::PrefCheckButton _font_missing_highlight; ///< PROOF
    UI::Widget::PrefCombo       _font_unit_type;
    UI::Widget::PrefCheckButton _font_output_px;
    UI::Widget::PrefCheckButton _font_fontsdir_system;
    UI::Widget::PrefCheckButton _font_fontsdir_user;
    UI::Widget::PrefMultiEntry  _font_fontdirs_custom;

    UI::Widget::PrefCheckButton _misc_comment;
    UI::Widget::PrefCheckButton _misc_default_metadata;
    UI::Widget::PrefCheckButton _export_all_extensions;
    UI::Widget::PrefCheckButton _misc_forkvectors;
    UI::Widget::PrefSpinButton  _misc_gradientangle;
    UI::Widget::PrefSpinButton  _recently_used_fonts_size;
    UI::Widget::PrefCheckButton _misc_gradient_collect;
    UI::Widget::PrefCheckButton _misc_scripts;

    // System page
    UI::Widget::PrefSpinButton  _misc_simpl;
    UI::Widget::PrefSpinButton  _undo_size;
    UI::Widget::PrefCheckButton _undo_limit;
    Gtk::Entry                  _sys_user_prefs;
    Gtk::Entry                  _sys_tmp_files;
    Gtk::Entry                  _sys_extension_dir;
    UI::Widget::PrefOpenFolder _sys_user_config;
    UI::Widget::PrefOpenFolder _sys_user_extension_dir;
    UI::Widget::PrefOpenFolder _sys_user_themes_dir;
    UI::Widget::PrefOpenFolder _sys_user_ui_dir;
    UI::Widget::PrefOpenFolder _sys_user_fonts_dir;
    UI::Widget::PrefOpenFolder _sys_user_icons_dir;
    UI::Widget::PrefOpenFolder _sys_user_keys_dir;
    UI::Widget::PrefOpenFolder _sys_user_palettes_dir;
    UI::Widget::PrefOpenFolder _sys_user_templates_dir;
    UI::Widget::PrefOpenFolder _sys_user_symbols_dir;
    UI::Widget::PrefOpenFolder _sys_user_paint_servers_dir;
    UI::Widget::PrefMultiEntry _sys_fontdirs_custom;
    UI::Widget::PrefEntryFile  _sys_shared_path;
    Gtk::Entry                 _sys_user_cache;
    Gtk::Entry                 _sys_data;
    Gtk::TextView              _sys_icon;
    Gtk::ScrolledWindow        _sys_icon_scroll;
    Gtk::TextView              _sys_systemdata;
    Gtk::ScrolledWindow        _sys_systemdata_scroll;

    // UI page
    UI::Widget::PrefCombo       _ui_languages;
    UI::Widget::PrefCheckButton _ui_colorsliders_top;
    UI::Widget::PrefSpinButton  _misc_recent;
    UI::Widget::PrefCheckButton _ui_rulersel;
    UI::Widget::PrefCheckButton _ui_realworldzoom;
    UI::Widget::PrefCheckButton _ui_pageorigin;
    UI::Widget::PrefCheckButton _ui_partialdynamic;
    UI::Widget::ZoomCorrRulerSlider _ui_zoom_correction;
    UI::Widget::PrefCheckButton _ui_yaxisdown;
    UI::Widget::PrefCheckButton _ui_rotationlock;
    UI::Widget::PrefCheckButton _ui_cursorscaling;
    UI::Widget::PrefCheckButton _ui_cursor_shadow;

    //Spellcheck
    UI::Widget::PrefCombo       _spell_language;
    UI::Widget::PrefCombo       _spell_language2;
    UI::Widget::PrefCombo       _spell_language3;
    UI::Widget::PrefCheckButton _spell_ignorenumbers;
    UI::Widget::PrefCheckButton _spell_ignoreallcaps;

    // Templates
    Gtk::Button                 _templates_default;

    // Bitmaps
    UI::Widget::PrefCombo       _misc_overs_bitmap;
    UI::Widget::PrefEntryFileButtonHBox       _misc_bitmap_editor;
    UI::Widget::PrefEntryFileButtonHBox       _misc_svg_editor;
    UI::Widget::PrefCheckButton _misc_bitmap_autoreload;
    UI::Widget::PrefSpinButton  _bitmap_copy_res;
    UI::Widget::PrefCheckButton _bitmap_ask;
    UI::Widget::PrefCheckButton _svg_ask;
    UI::Widget::PrefCombo       _bitmap_link;
    UI::Widget::PrefCombo       _svg_link;
    UI::Widget::PrefCombo       _bitmap_scale;
    UI::Widget::PrefSpinButton  _bitmap_import_quality;

    UI::Widget::PrefEntry       _kb_search;
    UI::Widget::PrefCombo       _kb_filelist;

    UI::Widget::PrefCheckButton _save_use_current_dir;
    UI::Widget::PrefEntryButtonHBox _save_default_filename;
    UI::Widget::PrefCheckButton _save_autosave_enable;
    UI::Widget::PrefSpinButton  _save_autosave_interval;
    UI::Widget::PrefEditFolder  _save_autosave_path_dir;
    UI::Widget::PrefSpinButton  _save_autosave_max;

    Gtk::ComboBoxText   _cms_display_profile;
    UI::Widget::PrefCheckButton     _cms_from_user;
    UI::Widget::PrefCombo           _cms_intent;

    UI::Widget::PrefCheckButton     _cms_softproof;
    UI::Widget::PrefCheckButton     _cms_gamutwarn;
    Gtk::ColorButton    _cms_gamutcolor;
    Gtk::ComboBoxText   _cms_proof_profile;
    UI::Widget::PrefCombo           _cms_proof_intent;
    UI::Widget::PrefCheckButton     _cms_proof_blackpoint;

    Gtk::Notebook       _grids_notebook;
    UI::Widget::PrefRadioButton     _grids_no_emphasize_on_zoom;
    UI::Widget::PrefRadioButton     _grids_emphasize_on_zoom;
    UI::Widget::DialogPage          _grids_xy;
    UI::Widget::DialogPage          _grids_axonom;
    // CanvasXYGrid properties:
        UI::Widget::PrefUnit            _grids_xy_units;
        UI::Widget::PrefSpinButton      _grids_xy_origin_x;
        UI::Widget::PrefSpinButton      _grids_xy_origin_y;
        UI::Widget::PrefSpinButton      _grids_xy_spacing_x;
        UI::Widget::PrefSpinButton      _grids_xy_spacing_y;
        UI::Widget::PrefColorPicker     _grids_xy_color;
        UI::Widget::PrefColorPicker     _grids_xy_empcolor;
        UI::Widget::PrefSpinButton      _grids_xy_empspacing;
        UI::Widget::PrefCheckButton     _grids_xy_dotted;
    // CanvasAxonomGrid properties:
        UI::Widget::PrefUnit            _grids_axonom_units;
        UI::Widget::PrefSpinButton      _grids_axonom_origin_x;
        UI::Widget::PrefSpinButton      _grids_axonom_origin_y;
        UI::Widget::PrefSpinButton      _grids_axonom_spacing_y;
        UI::Widget::PrefSpinButton      _grids_axonom_angle_x;
        UI::Widget::PrefSpinButton      _grids_axonom_angle_z;
        UI::Widget::PrefCheckButton     _grids_axonom_angle_y_vertical;
        UI::Widget::PrefColorPicker     _grids_axonom_color;
        UI::Widget::PrefColorPicker     _grids_axonom_empcolor;
        UI::Widget::PrefSpinButton      _grids_axonom_empspacing;

    // SVG Output page:
    UI::Widget::PrefCheckButton   _svgoutput_usenamedcolors;
    UI::Widget::PrefCheckButton   _svgoutput_usesodipodiabsref;
    UI::Widget::PrefSpinButton    _svgoutput_numericprecision;
    UI::Widget::PrefSpinButton    _svgoutput_minimumexponent;
    UI::Widget::PrefCheckButton   _svgoutput_inlineattrs;
    UI::Widget::PrefSpinButton    _svgoutput_indent;
    UI::Widget::PrefCombo         _svgoutput_pathformat;
    UI::Widget::PrefCheckButton   _svgoutput_forcerepeatcommands;

    // Attribute Checking controls for SVG Output page:
    UI::Widget::PrefCheckButton   _svgoutput_attrwarn;
    UI::Widget::PrefCheckButton   _svgoutput_attrremove;
    UI::Widget::PrefCheckButton   _svgoutput_stylepropwarn;
    UI::Widget::PrefCheckButton   _svgoutput_stylepropremove;
    UI::Widget::PrefCheckButton   _svgoutput_styledefaultswarn;
    UI::Widget::PrefCheckButton   _svgoutput_styledefaultsremove;
    UI::Widget::PrefCheckButton   _svgoutput_check_reading;
    UI::Widget::PrefCheckButton   _svgoutput_check_editing;
    UI::Widget::PrefCheckButton   _svgoutput_check_writing;

    // SVG Output export:
    UI::Widget::PrefCheckButton   _svgexport_insert_text_fallback;
    UI::Widget::PrefCheckButton   _svgexport_insert_mesh_polyfill;
    UI::Widget::PrefCheckButton   _svgexport_insert_hatch_polyfill;
    UI::Widget::PrefCheckButton   _svgexport_remove_marker_auto_start_reverse;
    UI::Widget::PrefCheckButton   _svgexport_remove_marker_context_paint;
    // clang-format on

    Gtk::Notebook _kb_notebook;
    UI::Widget::DialogPage _kb_page_shortcuts;
    UI::Widget::DialogPage _kb_page_modifiers;
    gboolean _kb_shortcuts_loaded;
    /*
     * Keyboard shortcut members
     */
    Glib::RefPtr<Gtk::TreeStore> _kb_store;
    Gtk::TreeView _kb_tree;
    Gtk::CellRendererAccel _kb_shortcut_renderer;
    Glib::RefPtr<Gtk::TreeModelFilter> _kb_filter;

    /*
     * Keyboard modifiers interface
     */
    class ModifierColumns final : public Gtk::TreeModel::ColumnRecord {
    public:
        ModifierColumns() {
            add(name);
            add(id);
            add(description);
            add(and_modifiers);
            add(user_set);
        }
        ~ModifierColumns() final = default;

        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> id;
        Gtk::TreeModelColumn<Glib::ustring> description;
        Gtk::TreeModelColumn<Glib::ustring> and_modifiers;
        Gtk::TreeModelColumn<unsigned int> user_set;
        Gtk::TreeModelColumn<unsigned int> is_enabled;
    };
    ModifierColumns _mod_columns;
    Glib::RefPtr<Gtk::TreeStore> _mod_store;
    Gtk::TreeView _mod_tree;
    Gtk::ToggleButton _kb_mod_ctrl;
    Gtk::ToggleButton _kb_mod_shift;
    Gtk::ToggleButton _kb_mod_alt;
    Gtk::ToggleButton _kb_mod_meta;
    Gtk::CheckButton _kb_mod_enabled;

    bool _kb_is_updated = false;

    int _minimum_width  = 0;
    int _minimum_height = 0;
    int _natural_width  = 0;
    int _natural_height = 0;

    bool GetSizeRequest(const Gtk::TreeModel::iterator& iter);

    void measure_vfunc(Gtk::Orientation const orientation, int const for_size,
                       int &minimum, int &natural,
                       int &minimum_baseline, int &natural_baseline) const final
    {
        minimum = (orientation == Gtk::Orientation::HORIZONTAL ? _minimum_width : _minimum_height);
        natural = (orientation == Gtk::Orientation::HORIZONTAL ? _natural_width : _natural_height);
    }

    int _sb_width = 0;
    UI::Widget::DialogPage *_current_page = nullptr;

    Gtk::TreeModel::iterator AddPage(UI::Widget::DialogPage& p, Glib::ustring title, int id);
    Gtk::TreeModel::iterator AddPage(UI::Widget::DialogPage& p, Glib::ustring title, Gtk::TreeModel::iterator parent, int id);
    Gtk::TreePath get_next_result(Gtk::TreeModel::iterator& iter, bool check_children = true);
    Gtk::TreePath get_prev_result(Gtk::TreeModel::iterator& iter, bool iterate = true);
    bool matchPage(Gtk::TreeModel::const_iterator const &iter);

    static void AddSelcueCheckbox(UI::Widget::DialogPage& p, Glib::ustring const &prefs_path, bool def_value);
    static void AddGradientCheckbox(UI::Widget::DialogPage& p, Glib::ustring const &prefs_path, bool def_value);
    static void AddLayerChangeCheckbox(UI::Widget::DialogPage &p, Glib::ustring const &prefs_path, bool def_value);
    static void AddPageChangeCheckbox(UI::Widget::DialogPage &p, Glib::ustring const &prefs_path, bool def_value);
    static void AddConvertGuidesCheckbox(UI::Widget::DialogPage& p, Glib::ustring const &prefs_path, bool def_value);
    static void AddFirstAndLastCheckbox(UI::Widget::DialogPage& p, Glib::ustring const &prefs_path, bool def_value);
    static void AddDotSizeSpinbutton(UI::Widget::DialogPage& p, Glib::ustring const &prefs_path, double def_value);
    static void AddBaseSimplifySpinbutton(UI::Widget::DialogPage& p, Glib::ustring const &prefs_path, double def_value);
    static void AddNewObjectsStyle(UI::Widget::DialogPage& p, Glib::ustring const &prefs_path, Glib::ustring const &item_type = "", const gchar* banner = nullptr);

    void on_pagelist_selection_changed();
    void show_not_found();
    void show_nothing_on_page();
    void show_try_search();
    void on_reset_prefs_clicked();
    void on_search_changed();
    void highlight_results(Glib::ustring const &key, Gtk::TreeModel::iterator &iter);
    void goto_first_result();

    void get_widgets_in_grid(Glib::ustring const &key, Gtk::Widget *widget);
    int num_widgets_in_grid(Glib::ustring const &key, Gtk::Widget *widget);
    void remove_highlight(Gtk::Label *label);
    void add_highlight(Gtk::Label *label, Glib::ustring const &key);

    bool recursive_filter(Glib::ustring &key, Gtk::TreeModel::const_iterator const &row);
    bool on_navigate_key_pressed(unsigned keyval, unsigned keycode, Gdk::ModifierType state);

    void initPageTools();
    void initPageUI();
    void initPageBehavior();
    void initPageIO();

    void initPageRendering();
    void initPageSpellcheck();
    void initPageTemplates();
    void initPageBitmaps();
    void initPageSystem();
    void initPageI18n(); // Do we still need it?
    void initKeyboardShortcuts(Gtk::TreeModel::iterator iter_ui);

    /*
     * Functions for the Keyboard shortcut editor panel
     */
    void onKBReset();
    void onKBImport();
    void onKBExport();
    void onKBList();
    void onKBRealize();
    void onKBListKeyboardShortcuts();
    void onKBTreeEdited (const Glib::ustring& path, guint accel_key, Gdk::ModifierType accel_mods, guint hardware_keycode);
    void onKBTreeCleared(const Glib::ustring& path_string);
    void onKBSearchKeyReleased();
    bool onKBSearchFilter(const Gtk::TreeModel::const_iterator& iter);
    static void onKBShortcutRenderer(Gtk::CellRenderer *renderer,
                                     Gtk::TreeModel::const_iterator const &iter);
    void on_modifier_selection_changed();
    void on_modifier_enabled();
    void on_modifier_edited();

private:
  Gtk::TreeModel::iterator searchRows(char const* srch, Gtk::TreeModel::iterator& iter, Gtk::TreeModel::Children list_model_childern);
  void themeChange(bool contrastslider = false);
  void comboThemeChange();
  void contrastThemeChange();
  void preferDarkThemeChange();
  void symbolicThemeCheck();
  void toggleSymbolic();
  bool usingCustomColors();
  void changeIconsColors();
  void resetIconsColorsFromTheme();
  void resetIconsColors(bool themechange = false);
  void resetIconsColorsWrapper();
  void get_highlight_colors(Colors::Color &colorsetbase, Colors::Color &colorsetsuccess,
                            Colors::Color &colorsetwarning, Colors::Color &colorseterror);

  std::map<Glib::ustring, bool> dark_themes;
  bool _init;
  Inkscape::PrefObserver _theme_oberver;
};

} // namespace Inkscape::UI::Dialog

#endif //INKSCAPE_UI_DIALOG_INKSCAPE_PREFERENCES_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
