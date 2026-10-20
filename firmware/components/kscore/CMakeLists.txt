# Portable core: compiled for ESP-IDF here and for the host by test/run_tests.py.
idf_component_register(
    SRCS "ksa_dtmf_rx.c" "ksa_frame.c" "ksa_protocol.c" "ksb_logic.c" "ksc_card.c"
         "ksui_render.c" "ksui_font.c"
    INCLUDE_DIRS "include"
)
