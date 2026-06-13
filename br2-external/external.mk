# Include every package .mk under this external tree
include $(sort $(wildcard $(BR2_EXTERNAL_KL_AM62X_PATH)/package/*/*.mk))
