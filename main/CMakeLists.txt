# main/CMakeLists.txt —— feature/world-execute-me 专属播放器固件构建配置
idf_component_register(
    SRCS "main.c"
         "world_execute_player.c"
         "cover.c"
         "tinf/tinflate.c"
         "tinf/tinfzlib.c"
         "tinf/adler32.c"
    INCLUDE_DIRS "." "tinf"
    REQUIRES bsp esp_timer
    EMBED_FILES "world_execute_data.bin" "deepseek_cover.bin"
)
