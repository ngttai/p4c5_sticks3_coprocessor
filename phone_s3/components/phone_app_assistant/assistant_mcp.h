/*
 * Device tools the Xiaozhi server can call (MCP): status, speaker volume, screen brightness.
 * Names follow the xiaozhi-esp32 convention (self.*) so the server's model already knows them.
 */
#pragma once

#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the tools on a freshly created engine; returns false when none could be added. */
bool assistant_mcp_register(esp_mcp_t *mcp);

#ifdef __cplusplus
}
#endif
