XBE_TITLE = SaveBackup
GEN_XISO = $(XBE_TITLE).iso

# Point this at your nxdk checkout. By default we expect nxdk to live in a
# "nxdk" subdirectory of this project, but you can override it on the command
# line, e.g.  make NXDK_DIR=/path/to/nxdk
NXDK_DIR ?= $(CURDIR)/nxdk

# Networking is required for the QR login + upload step.
NXDK_NET = y

# mbed TLS (clone into third_party/mbedtls, branch mbedtls-3.6.2). Exclude the
# files that need a host filesystem / POSIX sockets we do not provide.
MBEDTLS_DIR = $(CURDIR)/third_party/mbedtls
MBEDTLS_SRCS := $(filter-out %/net_sockets.c %/psa_its_file.c %/psa_crypto_storage.c,\
	$(wildcard $(MBEDTLS_DIR)/library/*.c))

SRCS = \
	$(CURDIR)/src/main.c \
	$(CURDIR)/src/scan_udata.c \
	$(CURDIR)/src/parse_meta.c \
	$(CURDIR)/src/eeprom_export.c \
	$(CURDIR)/src/report.c \
	$(CURDIR)/src/zip_export.c \
	$(CURDIR)/src/unzip_export.c \
	$(CURDIR)/src/net_auth.c \
	$(CURDIR)/src/app_ui.c \
	$(CURDIR)/src/ui_font.c \
	$(CURDIR)/src/app_input.c \
	$(CURDIR)/src/base64.c \
	$(CURDIR)/src/session_store.c \
	$(CURDIR)/src/save_dates.c \
	$(CURDIR)/src/content_hash.c \
	$(CURDIR)/src/upload.c \
	$(CURDIR)/src/xbmc_profiles.c \
	$(CURDIR)/src/xbl_account.c \
	$(CURDIR)/third_party/https_client.c \
	$(CURDIR)/third_party/qrcodegen.c \
	$(CURDIR)/third_party/nxdk_entropy.c \
	$(CURDIR)/third_party/nxdk_mbedtls_time.c \
	$(CURDIR)/third_party/miniz.c \
	$(MBEDTLS_SRCS)

# miniz: build only the in-memory deflate/zip core. We stream the archive to
# disk through our own WinAPI write callback, so the stdio/time helpers are not
# needed and would not map cleanly onto the Xbox kernel.
CFLAGS += -I$(CURDIR)/third_party -DMINIZ_NO_STDIO -DMINIZ_NO_TIME
NXDK_CFLAGS += -I$(NXDK_DIR)/lib/hal

# mbed TLS configuration for the Xbox (weak entropy, no filesystem).
NXDK_CFLAGS += -I$(MBEDTLS_DIR)/include
NXDK_CFLAGS += -I$(CURDIR)/third_party
NXDK_CFLAGS += -DMBEDTLS_CONFIG_FILE=\"mbedtls_nxdk_config.h\"

include $(NXDK_DIR)/Makefile

# Shadow nxdk's lwipopts.h so lwIP does not draw debug text over the QR code.
NXDK_CFLAGS := -I$(CURDIR)/lwip_override $(NXDK_CFLAGS)

# nxdk-cc defines _WIN32; undefine it for the two mbed TLS files that would
# otherwise pull in Windows-only headers.
$(MBEDTLS_DIR)/library/platform_util.obj: NXDK_CFLAGS += -U_WIN32
$(MBEDTLS_DIR)/library/x509_crt.obj: NXDK_CFLAGS += -U_WIN32
