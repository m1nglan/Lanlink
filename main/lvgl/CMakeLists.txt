SET(SOURCES screens/ui_main.c
    screens/ui_Balance.c
    screens/ui_Server.c
    screens/ui_chat.c
    screens/ui_home.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_lan_png.c
    images/ui_img_lighting_png.c
    images/ui_img_link_png.c
    images/ui_img_loading_png.c
    images/ui_img_deepseek_png.c
    images/ui_img_balance_png.c
    images/ui_img_1230237158.c
    images/ui_img_message_png.c
    images/ui_img_penggang_48_png.c
    images/ui_img_2034640467.c
    images/ui_img_bg_png.c
    images/ui_img_1926284110.c
    images/ui_img_bloudy_png.c
    images/ui_img_lighting_1_png.c
    images/ui_img_rain_1_png.c
    images/ui_img_snow_4_png.c
    images/ui_img_wind_3_png.c
    fonts/ui_font_balance22.c
    fonts/ui_font_ch14.c
    fonts/ui_font_date18.c
    fonts/ui_font_icon30.c
    fonts/ui_font_time25.c
    fonts/ui_font_time52.c
    fonts/ui_font_updown10.c
    fonts/ui_font_weather18.c)

add_library(ui ${SOURCES})
