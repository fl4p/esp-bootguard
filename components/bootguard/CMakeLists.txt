# WHOLE_ARCHIVE: the startup constructor that clears the bootloader's `loading`
# flag must be linked even when the app calls nothing from this component.
idf_component_register(SRCS "bootguard.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bootloader_support freertos
                       WHOLE_ARCHIVE)
