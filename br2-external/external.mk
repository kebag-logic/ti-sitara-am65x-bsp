# Include every package .mk under this external tree
include $(sort $(wildcard $(BR2_EXTERNAL_KL_AM62X_PATH)/package/*/*.mk))

# rauc 1.15.2 + OpenSSL 3.6 force OPENSSL_NO_ENGINE; we sign with a PEM key so drop the deprecated PKCS#11 engine path
RAUC_CONF_OPTS += -Dpkcs11_engine=false
HOST_RAUC_CONF_OPTS += -Dpkcs11_engine=false
