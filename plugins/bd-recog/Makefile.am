AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = bdrecog.la

bdrecog_la_SOURCES       = src/bd_recog_engine.c
bdrecog_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS) \
               -lcurl -lxml2 -ljson-c

include $(top_srcdir)/build/rules/uniplugin.am
