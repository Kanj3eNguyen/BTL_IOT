set(src_dirs        .
                    ../who_app_common)

set(include_dirs    .
                    ../who_app_common)

set(requires who_pp_ocr_v6 who_frame_lcd_disp)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
