set(src_dirs        .)

set(include_dirs    .)

set(requires        who_frame_cap)

set(ocr_component   pp_ocr_v6 espressif__pp_ocr_v6)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
idf_component_optional_requires(PUBLIC ${ocr_component})
