SET(SOURCES ui_Screen1.c
    ui_Screen2.c
    ui_Screen3.c
    ui_Screen4.c
    ui_Screen5.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_img_rev_counter_png.c
    ui_img_temp_icon_png.c
    ui_img_rev_fuel_png.c
    ui_img_fuel_icon_png.c
    ui_img_temp_exhaust_png.c
    ui_img_exhaust_icon_png.c
    ui_img_fuel_temp_png.c
    ui_img_oil_temp_png.c
    ui_img_oil_icon_png.c)

add_library(ui ${SOURCES})
