# Replaces ESP-IDF's second-stage bootloader `main` component (same name wins):
# ESP-IDF's bootloader_start.c plus the boot guard call before the image load.
# bootloader_hooks.h stays in the stock main component's directory.
idf_build_get_property(idf_path IDF_PATH)
idf_component_register(SRCS "bootloader_start.c" "bootguard_boot.c"
                       INCLUDE_DIRS "." "../../components/bootguard/include"
                       PRIV_INCLUDE_DIRS "${idf_path}/components/bootloader/subproject/main"
                       REQUIRES bootloader bootloader_support esp_rom hal soc log)

# Use the default linker scripts
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
