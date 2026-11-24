#pragma once

/* This BSP ships with LVGL. Define BSP_CONFIG_NO_GRAPHIC_LIB=1 on the compiler
 * command line to build only the hardware layer (bsp_display_new() etc.). */
#if !defined(BSP_CONFIG_NO_GRAPHIC_LIB)
#define BSP_CONFIG_NO_GRAPHIC_LIB (0)
#endif
