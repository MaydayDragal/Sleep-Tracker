idf_component_register(
    SRCS "bodynet.c"
    INCLUDE_DIRS "include"
    REQUIRES bt
)
# Uses NimBLE (BLE central) to talk to WT9011DCL body sensors + the Polar H10.
# Enable NimBLE and raise max connections in sdkconfig when implementing:
#   CONFIG_BT_ENABLED=y, CONFIG_BT_NIMBLE_ENABLED=y,
#   CONFIG_BT_NIMBLE_MAX_CONNECTIONS >= (body sensors + H10)
